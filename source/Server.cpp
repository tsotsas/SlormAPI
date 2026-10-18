#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include "Server.hpp"
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace slorm
{
	namespace
	{
		struct Pending
		{
			std::string Request;
			std::string Response;
			bool Done = false;
			std::mutex Mutex;
			std::condition_variable Cv;
		};

		std::atomic<bool> g_Running = false;
		SOCKET g_Listener = INVALID_SOCKET;
		std::thread g_AcceptThread;

		std::mutex g_QueueMutex;
		std::deque<std::shared_ptr<Pending>> g_Queue;
		std::atomic<int> g_PendingCount = 0;

		std::mutex g_ClientsMutex;
		std::set<SOCKET> g_Clients;

		bool SendAll(SOCKET Sock, const std::string& Data)
		{
			size_t sent = 0;
			while (sent < Data.size())
			{
				int n = send(Sock, Data.data() + sent, static_cast<int>((std::min<size_t>)(Data.size() - sent, 1 << 20)), 0);
				if (n <= 0) return false;
				sent += n;
			}
			return true;
		}

		void ClientLoop(SOCKET Sock)
		{
			std::string buffer;
			bool firstLine = true;
			char chunk[65536];
			while (g_Running)
			{
				int n = recv(Sock, chunk, sizeof(chunk), 0);
				if (n <= 0) break;
				buffer.append(chunk, n);
				// a web page can make the browser send HTTP to 127.0.0.1: such a connection is dropped unread
				if (firstLine)
				{
					for (const char* m : { "GET ", "POST ", "PUT ", "HEAD ", "OPTIONS ", "DELETE ", "PATCH ", "CONNECT " })
						if (buffer.compare(0, strlen(m), m) == 0) goto out;
					if (buffer.find('\n') != std::string::npos) firstLine = false;
				}
				if (buffer.size() > (size_t(16) << 20) && buffer.find('\n') == std::string::npos) goto out;   // no line end in 16 MB

				size_t nl;
				while ((nl = buffer.find('\n')) != std::string::npos)
				{
					auto p = std::make_shared<Pending>();
					p->Request = buffer.substr(0, nl);
					buffer.erase(0, nl + 1);
					if (!p->Request.empty() && p->Request.back() == '\r') p->Request.pop_back();
					if (p->Request.empty()) continue;

					{
						std::lock_guard lock(g_QueueMutex);
						g_Queue.push_back(p);
						++g_PendingCount;
					}

					std::unique_lock lock(p->Mutex);
					while (!p->Done && g_Running)
						p->Cv.wait_for(lock, std::chrono::milliseconds(200));
					if (!p->Done) goto out;

					if (!SendAll(Sock, p->Response + "\n")) goto out;
				}
			}
		out:
			{
				std::lock_guard lock(g_ClientsMutex);
				g_Clients.erase(Sock);
			}
			closesocket(Sock);
		}

		void AcceptLoop()
		{
			while (g_Running)
			{
				SOCKET client = accept(g_Listener, nullptr, nullptr);
				if (client == INVALID_SOCKET) break;
				{
					std::lock_guard lock(g_ClientsMutex);
					g_Clients.insert(client);
				}
				std::thread(ClientLoop, client).detach();
			}
		}
	}

	bool StartServer(uint16_t Port, std::string& Error)
	{
		WSADATA wsa;
		if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { Error = "WSAStartup failed"; return false; }

		g_Listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (g_Listener == INVALID_SOCKET) { Error = "socket() failed"; return false; }

		// Loopback only: the API must never be reachable from the network.
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_port = htons(Port);
		addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

		BOOL exclusive = TRUE;
		setsockopt(g_Listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

		if (bind(g_Listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR)
		{
			Error = "bind() failed on 127.0.0.1:" + std::to_string(Port) + " (WSA " + std::to_string(WSAGetLastError()) + ")";
			closesocket(g_Listener);
			g_Listener = INVALID_SOCKET;
			return false;
		}
		if (listen(g_Listener, 8) == SOCKET_ERROR) { Error = "listen() failed"; return false; }

		g_Running = true;
		g_AcceptThread = std::thread(AcceptLoop);
		return true;
	}

	void StopServer()
	{
		if (!g_Running.exchange(false)) return;
		closesocket(g_Listener);
		g_Listener = INVALID_SOCKET;
		{
			std::lock_guard lock(g_ClientsMutex);
			for (SOCKET s : g_Clients) shutdown(s, SD_BOTH);
		}
		if (g_AcceptThread.joinable()) g_AcceptThread.join();
		WSACleanup();
	}

	bool HasPendingRequests()
	{
		return g_PendingCount.load(std::memory_order_relaxed) > 0;
	}

	void DrainRequests(const std::function<std::string(const std::string&)>& Handle)
	{
		std::deque<std::shared_ptr<Pending>> work;
		{
			std::lock_guard lock(g_QueueMutex);
			work.swap(g_Queue);
			g_PendingCount = 0;
		}
		for (auto& p : work)
		{
			std::string response = Handle(p->Request);
			{
				std::lock_guard lock(p->Mutex);
				p->Response = std::move(response);
				p->Done = true;
			}
			p->Cv.notify_one();
		}
	}
}
