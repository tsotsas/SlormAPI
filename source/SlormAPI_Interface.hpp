// SlormAPI's interface for other Aurie modules (copy this header into your mod).
//
//   SlormAPIInterface* api = nullptr;
//   Aurie::ObGetInterface(SLORMAPI_INTERFACE_NAME, reinterpret_cast<Aurie::AurieInterfaceBase*&>(api));
//   const char* response = api->Request(R"({"cmd":"get","path":"current_hero"})");
//
// Request() runs one SlormAPI command - the same newline-free JSON protocol as the optional TCP server (get, set, call,
// instances, hook, hooklog, overlay, batch, ...; see the SlormAPI README) - on the calling thread, which must be the
// game thread (call it from a YYToolkit callback). The response is {"ok": true, "result": ...} or {"ok": false,
// "error": "..."}; the returned string stays valid until the next Request() call.
//
// Events:
//
//   api->Subscribe("enemy_killed", [](const char* Event, const char* DataJson, void* User) { ... }, nullptr);
//
// Callbacks run on the game thread once per frame, after the frame's events happened; DataJson is only valid during
// the call. "*" subscribes to every event. Events: see the SlormAPI README.
#pragma once
#include <Aurie/shared.hpp>
#include <cstdint>

inline constexpr const char* SLORMAPI_INTERFACE_NAME = "SlormAPI";

using SlormEventCallback = void (*)(const char* Event, const char* DataJson, void* User);

struct SlormAPIInterface : public Aurie::AurieInterfaceBase
{
	virtual const char* Request(const char* RequestJson) = 0;
	virtual uint64_t Subscribe(const char* Event, SlormEventCallback Callback, void* User) = 0;
	virtual void Unsubscribe(uint64_t SubscriptionId) = 0;
};
