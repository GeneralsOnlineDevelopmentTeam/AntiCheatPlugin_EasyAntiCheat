#pragma once
#include <stdint.h>
#include <stddef.h>
#include <windows.h>
#include <string>
#include <iosfwd>
#include <fstream>
#include <filesystem>
#include <cassert>
#include <map>
#include <unordered_set>
#include <chrono>
#include <mutex>
#include <atomic>

// Epic SDK
#include "EOS/Include/eos_types.h"
#include "EOS/Include/eos_base.h"
#include "EOS/Include/eos_integratedplatform_types.h"
#include "EOS/Include/eos_init.h"
#include "EOS/Include/eos_integratedplatform.h"
#include "EOS/Include/Windows/eos_Windows.h"
#include "EOS/Include/eos_logging.h"
#include "EOS/Include/eos_sdk.h"
#include "EOS/Include/eos_anticheatclient.h"
#include "EOS/Include/eos_anticheatcommon_types.h"


#pragma comment(lib, "../EOS/Lib/EOSSDK-Win32-Shipping.lib")

#ifdef _WIN32
#ifdef PLUGIN_EXPORTS
#define PLUGIN_API extern "C" __declspec(dllexport)
#else
#define PLUGIN_API extern "C" __declspec(dllimport)
#endif
#else
#define PLUGIN_API extern "C"
#endif

// ------------------------------------------------------------
// Callback Types
// ------------------------------------------------------------

typedef void (*LoggingFunc)(const char*);
typedef void (*SendMessageViaTransportFunc)(uint32_t, const void*, uint32_t);

typedef void (*ACPlayerActionRequiredCallbackFunc)(
	uint32_t userId,
	const char* reasonString,
	int actionType,
	int actionReason
	);

typedef void (*ACIntegrityViolationCallbackFunc)(
	const char* violationString,
	int violationID
	);

// NOTE: must stay byte-for-byte compatible with EConnectionState in the game's
// GameNetwork/GeneralsOnline/PluginInterfaces.h
enum class EConnectionState : uint8_t
{
	NOT_CONNECTED = 0,
	CONNECTING_DIRECT = 1,
	FINDING_ROUTE = 2,
	CONNECTED_DIRECT = 3,
	CONNECTION_FAILED = 4,
	CONNECTION_DISCONNECTED = 5
};

// NOTE: must stay compatible with ENetworkChannels in the game's PluginInterfaces.h
enum class ENetworkChannels : uint8_t
{
	Game = 0,
	Anticheat = 1,
	Signalling = 2,
	Ping,
	Pong
};

// NOTE: must stay compatible with EPacketReliability in the game's PluginInterfaces.h
enum class EPacketReliability : int32_t
{
	PACKET_RELIABILITY_UNRELIABLE_UNORDERED = 0,
	PACKET_RELIABILITY_RELIABLE_UNORDERED = 1,
	PACKET_RELIABILITY_RELIABLE_ORDERED = 2
};

// Signature is dictated by the game's FuncDefInitialize / OnConnectionStateChangedCallbackFunc
typedef void (*ConnectionStateChangedCallbackFunc)(
	const char* middlewareUserID,
	uint64_t goUserID,
	EConnectionState connectionState
	);

// Logging sinks are atomic rather than mutex-protected: PluginLog() is invoked
// from EOS SDK worker threads, and taking g_StateMutex there would invert the
// lock order against threads that hold g_StateMutex while inside an EOS call.
extern std::atomic<LoggingFunc> g_fnLoggingFunc;
extern std::atomic<LoggingFunc> g_fnLobbyChatOutput;

extern EOS_HPlatform g_EOSPlatformHandle;

typedef void (*LoginCallback)(bool bSuccess);

// Thread synchronization for global state
extern std::recursive_mutex g_StateMutex;

// ------------------------------------------------------------
// Exported API
// ------------------------------------------------------------

// Callback registration
PLUGIN_API void SetLoggingFunction(LoggingFunc cb);
PLUGIN_API void SetLobbyChatOutputFunction(LoggingFunc cb);
PLUGIN_API void SetACActionRequiredCallback(ACPlayerActionRequiredCallbackFunc cb);
PLUGIN_API void SetACIntegrityViolationOccurredCallback(ACIntegrityViolationCallbackFunc cb);
PLUGIN_API void SetSendMessageViaTransportCallback(SendMessageViaTransportFunc cb);

// Required plugin functions
PLUGIN_API void ACMessageArrivedViaTransport(uint32_t sourceUserID, void* data, uint32_t dataLen);

PLUGIN_API void Tick();
PLUGIN_API bool IsLoggedIn();
PLUGIN_API bool IsExternalProcessRunning();

PLUGIN_API int GetAnticheatIdentifier();

PLUGIN_API bool GetMiddlewareAuthToken(char* buffer, size_t bufferSize);

PLUGIN_API int Initialize(ConnectionStateChangedCallbackFunc connectionStateChangedCB);
PLUGIN_API void Shutdown();
PLUGIN_API void BeginSession();
PLUGIN_API void EndSession();

PLUGIN_API bool RegisterPlayer(const char* middlewareUserID, uint32_t goUserID);
PLUGIN_API bool DeregisterPlayer(const char* middlewareUserID, uint32_t goUserID);

PLUGIN_API void Login(const char* gameToken, LoginCallback cb);
PLUGIN_API void RefreshToken(const char* gameToken, LoginCallback cb);

// ------------------------------------------------------------
// Transport API
//
// The game resolves every one of these at load time and unloads the plugin if
// any is missing, so they must always be exported. This plugin does not provide
// its own secure transport: DoesACPluginProvideSecureGameTransport() returns
// false and the game keeps using its own mesh/WebSocket transport, which means
// the remaining entry points are never driven by the game.
// ------------------------------------------------------------
PLUGIN_API bool DoesACPluginProvideSecureGameTransport();
PLUGIN_API void StartSignalling(const char* middlewareUserID, uint64_t goUserID);
PLUGIN_API void SendPacket(const char* middlewareUserID, uint64_t targetGoUserID, void* data, int numBytes, ENetworkChannels channel, EPacketReliability reliability);
PLUGIN_API int GetNextRecvPacketSize(uint8_t channelToReceiveOn);
PLUGIN_API bool RecvPacket(uint8_t** outData, uint8_t channelToReceiveOn);
PLUGIN_API void FreePacket(void* packetData);

PLUGIN_API int GetConnectionLatencyForUser(const char* middlewareUserID, uint32_t goUserID);

PLUGIN_API void DisconnectPlayer(const char* middlewareUserID, uint64_t goUserID);
PLUGIN_API void DisconnectAll();

// ------------------------------------------------------------
// Vars
// ------------------------------------------------------------
extern EOS_ProductUserId g_EOSUserID;
extern uint32_t g_goUserID;

extern ACIntegrityViolationCallbackFunc g_fnAnticheatIntegrityViolationOccurredCallback;
extern ACPlayerActionRequiredCallbackFunc g_fnAnticheatActionCallback;
extern SendMessageViaTransportFunc g_fnSendMessageViaTransport;
extern ConnectionStateChangedCallbackFunc g_fnConnectionStateChanged;
extern bool g_bEventsHooked;

// ------------------------------------------------------------
// Enums
// ------------------------------------------------------------
enum class EAnticheatActionType : int32_t
{
	NONE = 0,
	KICK = 1
};

enum class EAnticheatActionReason : int32_t
{
	Unknown = 0,
	InternalError = 1,
	InvalidMessage = 2,
	AuthFailure = 3,
	ACNotRunning = 4,
	HeartbeatTimedOut = 5,
	ClientViolation = 6,
	BackendViolation = 7,
	TempCooldown = 8,
	TempBanned = 9,
	PermaBanned = 10
};