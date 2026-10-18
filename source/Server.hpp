// Localhost TCP server. Network threads only queue request lines; the game thread
// answers them in DrainRequests() so every engine call happens on the main thread.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace slorm
{
	bool StartServer(uint16_t Port, std::string& Error);
	void StopServer();

	// Cheap check so the per-event pump costs nothing when idle.
	bool HasPendingRequests();

	// Called from the game thread.
	void DrainRequests(const std::function<std::string(const std::string&)>& Handle);
}
