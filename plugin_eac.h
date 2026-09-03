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

typedef void (*ConnectionStateChangedCallbackFunc)(
	uint32_t userId,
	int connectionState
	);

extern LoggingFunc g_fnLoggingFunc;
extern LoggingFunc g_fnLobbyChatOutput;

extern EOS_HPlatform g_EOSPlatformHandle;

typedef void (*LoginCallback)(bool bSuccess);

// Thread synchronization for global state
extern std::recursive_mutex g_StateMutex;

// ------------------------------------------------------------
// Enums (mirroring plugin.cpp)
// ------------------------------------------------------------

enum EPluginConnectionState
{
	EConnectionState_Connecting = 0,
	EConnectionState_Connected = 1,
	EConnectionState_ConnectionClosed = 2
};

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

PLUGIN_API int Initialize();
PLUGIN_API void Shutdown();
PLUGIN_API void BeginSession();
PLUGIN_API void EndSession();

PLUGIN_API bool RegisterPlayer(const char* middlewareUserID, uint32_t goUserID);
PLUGIN_API bool DeregisterPlayer(const char* middlewareUserID, uint32_t goUserID);

PLUGIN_API void Login(const char* gameToken, LoginCallback cb);
PLUGIN_API void RefreshToken(const char* gameToken, LoginCallback cb);

// ------------------------------------------------------------
// Vars
// ------------------------------------------------------------
extern EOS_ProductUserId g_EOSUserID;
extern uint32_t g_goUserID;

extern ACIntegrityViolationCallbackFunc g_fnAnticheatIntegrityViolationOccurredCallback;
extern ACPlayerActionRequiredCallbackFunc g_fnAnticheatActionCallback;
extern SendMessageViaTransportFunc g_fnSendMessageViaTransport;
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