#include "plugin_eac.h"
#include <unordered_map>
#include <unordered_set>
#include "EOS/Include/eos_p2p_types.h"

// ------------------------------------------------------------
// Global definitions (declared extern in plugin_eac.h)
// ------------------------------------------------------------
std::atomic<LoggingFunc> g_fnLoggingFunc{ nullptr };
std::atomic<LoggingFunc> g_fnLobbyChatOutput{ nullptr };
EOS_HPlatform g_EOSPlatformHandle = nullptr;
std::recursive_mutex g_StateMutex;

// Last reported EOS network connection type per middleware user ID, used by
// IsConnectionRelayed(). Written from EOS worker threads in
// ReportConnectionState(), read on the game thread, so it carries its own lock.
std::unordered_map<std::string, EOS_ENetworkConnectionType> g_ConnectionType;
std::mutex g_ConnectionTypeMutex;

EOS_ProductUserId g_EOSUserID = nullptr;
uint32_t g_goUserID = 0;

ACIntegrityViolationCallbackFunc g_fnAnticheatIntegrityViolationOccurredCallback = nullptr;
ACPlayerActionRequiredCallbackFunc g_fnAnticheatActionCallback = nullptr;
SendMessageViaTransportFunc g_fnSendMessageViaTransport = nullptr;
ConnectionStateChangedCallbackFunc g_fnConnectionStateChanged = nullptr;
bool g_bEventsHooked = false;

// Global notification IDs for callback cleanup
EOS_NotificationId g_NotifyClientIntegrityViolatedId = 0;
EOS_NotificationId g_NotifyMessageToPeerId = 0;
EOS_NotificationId g_NotifyPeerAuthStatusChangedId = 0;
EOS_NotificationId g_NotifyPeerActionRequiredId = 0;

typedef void (*LoginCallback)(bool bSuccess);
LoginCallback g_LoginCallback = nullptr;

// Maps a middleware (EOS ProductUserId) string to the game's user ID.
// Read from EOS worker threads inside the P2P notification handlers and written
// by RegisterPlayer()/DeregisterPlayer(), so it carries its own lock. If both
// locks are needed, g_StateMutex must always be acquired first.
std::unordered_map<std::string, uint32_t> g_UserMap;
static std::mutex g_UserMapMutex;

// Middleware user IDs whose incoming connection request arrived before the game
// had registered them. EOS never re-delivers a connection request, so remember
// it here and accept it as soon as RegisterPlayer() vouches for the peer.
// Guarded by g_UserMapMutex.
static std::unordered_set<std::string> g_PendingConnectionRequests;

// Round-trip latency per middleware user ID, in milliseconds. Populated by the
// ping/pong exchange driven from Tick(); read by GetConnectionLatencyForUser()
// on the game thread.
static std::unordered_map<std::string, uint32_t> g_LatencyMs;
static std::mutex g_LatencyMutex;

// ------------------------------------------------------------
// EOS deployment credentials
//
// These MUST be supplied by the builder, e.g.
//   cmake -DEAC_EOS_PRODUCT_ID=... (see README)
// or by defining them on the compiler command line. The "TODO" fallbacks exist
// only so the file compiles standalone; Initialize() refuses to run with them.
// ------------------------------------------------------------
#ifndef EAC_EOS_PRODUCT_ID
#define EAC_EOS_PRODUCT_ID "TODO"
#endif
#ifndef EAC_EOS_SANDBOX_ID
#define EAC_EOS_SANDBOX_ID "TODO"
#endif
#ifndef EAC_EOS_DEPLOYMENT_ID
#define EAC_EOS_DEPLOYMENT_ID "TODO"
#endif
#ifndef EAC_EOS_CLIENT_ID
#define EAC_EOS_CLIENT_ID "TODO"
#endif
#ifndef EAC_EOS_CLIENT_SECRET
#define EAC_EOS_CLIENT_SECRET "TODO"
#endif

// Return codes from Initialize()
enum EInitializeResult
{
	EInitializeResult_Success = 0,
	EInitializeResult_BadEnvironment = 1,
	EInitializeResult_ContainerFailed = 2,
	EInitializeResult_SDKInitFailed = 3,
	EInitializeResult_PlatformCreateFailed = 4,
	EInitializeResult_MissingCredentials = 5
};

// Forward declaration so DllMain can call Shutdown() defined later in this file.
PLUGIN_API void Shutdown();

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
	switch (ul_reason_for_call)
	{
		case DLL_PROCESS_ATTACH:
		{
			::DisableThreadLibraryCalls(hModule);
			break;
		}

		case DLL_PROCESS_DETACH:
		{
			// NOTE: Do NOT tear down the EOS SDK here. EOS_Platform_Release() and
			// EOS_Shutdown() join worker threads, unload dependent modules and
			// re-enter user code - all of which deadlock or fault when performed
			// under the Windows loader lock. Acquiring g_StateMutex here is unsafe
			// for the same reason.
			//
			// The host application is responsible for calling the exported
			// Shutdown() before unloading this module.
			break;
		}
	}

	return TRUE;
}

void PluginLog(const char* fmt, ...)
{
	if (fmt == nullptr)
	{
		return;
	}

	// This function is called from EOS SDK worker threads (via the EOS logging
	// callback) as well as from game threads. It deliberately takes no lock: the
	// sink is read atomically and invoked without holding g_StateMutex, so a
	// thread inside an EOS call cannot deadlock against an EOS logging thread.
	LoggingFunc sink = g_fnLoggingFunc.load(std::memory_order_acquire);
	if (sink == nullptr)
	{
		return;
	}

	char buffer[8192];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	buffer[sizeof(buffer) - 1] = 0;
	va_end(args);

	sink(buffer);
}

// Writes a line to the game's lobby chat window, if the host has installed a
// sink. Like PluginLog() this takes no lock so it is safe to call from EOS
// worker threads.
static void LobbyChatLog(const char* fmt, ...)
{
	if (fmt == nullptr)
	{
		return;
	}

	LoggingFunc sink = g_fnLobbyChatOutput.load(std::memory_order_acquire);
	if (sink == nullptr)
	{
		return;
	}

	char buffer[8192];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, sizeof(buffer), fmt, args);
	buffer[sizeof(buffer) - 1] = 0;
	va_end(args);

	sink(buffer);
}

// Returns the anti-cheat client interface, or nullptr if the platform has not
// been initialised. EOS_Platform_GetAntiCheatClientInterface() dereferences its
// argument, so the platform handle must never be passed through as null.
// Must be called with g_StateMutex held.
static EOS_HAntiCheatClient GetAntiCheatHandle()
{
	if (g_EOSPlatformHandle == nullptr)
	{
		return nullptr;
	}

	return EOS_Platform_GetAntiCheatClientInterface(g_EOSPlatformHandle);
}

// Returns the P2P interface, or nullptr if the platform has not been
// initialised (EOS_Platform_GetP2PInterface() dereferences its argument).
// Must be called with g_StateMutex held.
static EOS_HP2P GetP2PHandle()
{
	if (g_EOSPlatformHandle == nullptr)
	{
		return nullptr;
	}

	return EOS_Platform_GetP2PInterface(g_EOSPlatformHandle);
}

// EOS_ProductUserId_ToString() requires a buffer of EOS_PRODUCTUSERID_MAX_LENGTH + 1
// bytes; anything smaller fails with EOS_LimitExceeded and leaves the buffer
// untouched, so the result must always be checked before reading it.
static bool ProductUserIdToString(EOS_ProductUserId userId, std::string& outString)
{
	if (userId == nullptr)
	{
		return false;
	}

	char buffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
	int32_t outLength = (int32_t)sizeof(buffer);

	if (EOS_ProductUserId_ToString(userId, buffer, &outLength) != EOS_EResult::EOS_Success)
	{
		return false;
	}

	outString.assign(buffer);
	return true;
}

// Thread-safe lookup of the game user ID associated with a middleware user ID.
static bool TryGetGoUserID(const std::string& middlewareUserID, uint32_t& outGoUserID)
{
	std::lock_guard<std::mutex> lock(g_UserMapMutex);

	auto it = g_UserMap.find(middlewareUserID);
	if (it == g_UserMap.end())
	{
		return false;
	}

	outGoUserID = it->second;
	return true;
}

// Reverse of TryGetGoUserID(): finds the middleware user ID the game knows as
// goUserID. Used by the anti-cheat transport, which is handed game user IDs by
// the EOS anti-cheat interface but has to address peers by product user ID.
static bool TryGetMiddlewareUserID(uint32_t goUserID, std::string& outMiddlewareUserID)
{
	std::lock_guard<std::mutex> lock(g_UserMapMutex);

	for (const auto& entry : g_UserMap)
	{
		if (entry.second == goUserID)
		{
			outMiddlewareUserID = entry.first;
			return true;
		}
	}

	return false;
}

// Reports a peer's connection state change to the game. Shared by all four P2P
// notification handlers, which run on EOS worker threads: the user map needs its
// own lock and the host callbacks may be null.
static void ReportConnectionState(EOS_ProductUserId remoteUserId, const char* description, EConnectionState state, EOS_ENetworkConnectionType connectionType)
{
	if (state == EConnectionState::CONNECTED_DIRECT)
	{
		std::lock_guard<std::mutex> lock(g_ConnectionTypeMutex);

		char szBuffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = { 0 };
		int32_t outLen = sizeof(szBuffer);
		EOS_ProductUserId_ToString(remoteUserId, szBuffer, &outLen);
		g_ConnectionType[szBuffer] = connectionType;

		if (connectionType == EOS_ENetworkConnectionType::EOS_NCT_NoConnection)
		{
			PluginLog("[EAC] Connection type is NoConnection, invalid state");
		}
	}

	std::string middlewareUserID;
	if (!ProductUserIdToString(remoteUserId, middlewareUserID))
	{
		PluginLog("[EAC] %s: could not resolve remote product user ID", description);
		return;
	}

	uint32_t goUserID = 0;
	if (!TryGetGoUserID(middlewareUserID, goUserID))
	{
		// Not a player we know about - ignore rather than reporting a bogus user
		// ID of 0 to the game.
		PluginLog("[EAC] %s: unknown peer %s, ignoring", description, middlewareUserID.c_str());
		return;
	}

	auto fn = g_fnLobbyChatOutput.load();
	if (fn != nullptr)
	{
		std::string strMessage = std::format("{} {} [{}]", description, middlewareUserID, goUserID);
		fn(strMessage.c_str());
	}

	ConnectionStateChangedCallbackFunc stateCallback = nullptr;
	{
		std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
		stateCallback = g_fnConnectionStateChanged;
	}

	if (stateCallback != nullptr)
	{
		stateCallback(middlewareUserID.c_str(), goUserID, state);
	}
}

// ------------------------------------------------------------
// NAT type detection
// ------------------------------------------------------------

// Most recent NAT type reported by EOS. Written from an EOS worker thread in
// the query callback and read from game threads, so it is atomic.
static std::atomic<EOS_ENATType> g_LocalNATType{ EOS_ENATType::EOS_NAT_Unknown };

static const char* NATTypeToString(EOS_ENATType natType)
{
	switch (natType)
	{
	case EOS_ENATType::EOS_NAT_Open:
		return "Open - all peers can connect directly to you";
	case EOS_ENATType::EOS_NAT_Moderate:
		return "Moderate - you can connect directly to Open and Moderate peers";
	case EOS_ENATType::EOS_NAT_Strict:
		return "Strict - you can only connect directly to Open peers, other traffic is relayed";
	case EOS_ENATType::EOS_NAT_Unknown:
	default:
		return "Unknown - EOS could not determine your NAT type";
	}
}

// Asks EOS to determine how strict the local NAT is. The query is asynchronous;
// the answer is logged and echoed into lobby chat from the completion callback,
// which runs on an EOS worker thread during EOS_Platform_Tick().
static void QueryLocalNATType()
{
	EOS_HP2P p2pHandle = nullptr;

	{
		std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
		p2pHandle = GetP2PHandle();
	}

	if (p2pHandle == nullptr)
	{
		PluginLog("[EAC] NAT: P2P interface unavailable, cannot query NAT type");
		return;
	}

	// EOS caches the last known result, so report it immediately rather than
	// leaving the player with nothing while the (slow) query runs.
	EOS_P2P_GetNATTypeOptions getOptions = {};
	getOptions.ApiVersion = EOS_P2P_GETNATTYPE_API_LATEST;

	EOS_ENATType cachedNATType = EOS_ENATType::EOS_NAT_Unknown;
	EOS_EResult getResult = EOS_P2P_GetNATType(p2pHandle, &getOptions, &cachedNATType);

	if (getResult == EOS_EResult::EOS_Success && cachedNATType != EOS_ENATType::EOS_NAT_Unknown)
	{
		g_LocalNATType.store(cachedNATType, std::memory_order_release);
		PluginLog("[EAC] NAT: cached NAT type is %s", NATTypeToString(cachedNATType));
		LobbyChatLog("[NAT] Your NAT type: %s", NATTypeToString(cachedNATType));
	}

	EOS_P2P_QueryNATTypeOptions queryOptions = {};
	queryOptions.ApiVersion = EOS_P2P_QUERYNATTYPE_API_LATEST;

	PluginLog("[EAC] NAT: querying NAT type...");

	EOS_P2P_QueryNATType(p2pHandle, &queryOptions, nullptr,
		[](const EOS_P2P_OnQueryNATTypeCompleteInfo* Data)
		{
			if (Data == nullptr)
			{
				return;
			}

			if (Data->ResultCode != EOS_EResult::EOS_Success)
			{
				PluginLog("[EAC] NAT: query failed: %s", EOS_EResult_ToString(Data->ResultCode));
				LobbyChatLog("[NAT] Could not determine your NAT type (%s)", EOS_EResult_ToString(Data->ResultCode));
				return;
			}

			const EOS_ENATType previousNATType = g_LocalNATType.exchange(Data->NATType, std::memory_order_acq_rel);

			PluginLog("[EAC] NAT: local NAT type is %s", NATTypeToString(Data->NATType));

			// Only spam lobby chat when this actually tells the player something
			// new - QueryLocalNATType() already printed any cached value.
			if (previousNATType != Data->NATType)
			{
				LobbyChatLog("[NAT] Your NAT type: %s", NATTypeToString(Data->NATType));
			}

			if (Data->NATType == EOS_ENATType::EOS_NAT_Strict)
			{
				LobbyChatLog("[NAT] A strict NAT can cause connection problems. Enabling UPnP or forwarding ports on your router will help.");
			}
		});
}

// Atomically detaches the pending login callback and invokes it without holding
// g_StateMutex, so the host can safely re-enter the plugin and a stale callback
// can never be fired twice.
static void FireLoginCallback(bool bSuccess)
{
	LoginCallback localCallback = nullptr;

	{
		std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
		localCallback = g_LoginCallback;
		g_LoginCallback = nullptr;
	}

	if (localCallback != nullptr)
	{
		localCallback(bSuccess);
	}
}

void SetLoggingFunction(LoggingFunc cb)
{
	g_fnLoggingFunc.store(cb, std::memory_order_release);
}

void SetLobbyChatOutputFunction(LoggingFunc cb)
{
	g_fnLobbyChatOutput.store(cb, std::memory_order_release);
}

void SetACActionRequiredCallback(ACPlayerActionRequiredCallbackFunc cb)
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	g_fnAnticheatActionCallback = cb;
}

void SetACIntegrityViolationOccurredCallback(ACIntegrityViolationCallbackFunc cb)
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	g_fnAnticheatIntegrityViolationOccurredCallback = cb;
}

void SetSendMessageViaTransportCallback(SendMessageViaTransportFunc cb)
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	g_fnSendMessageViaTransport = cb;
}

void ACMessageArrivedViaTransport(uint32_t sourceUserID, void* data, uint32_t dataLen)
{
	if (data == nullptr || dataLen == 0)
	{
		PluginLog("[AC][EAC][REMOTE] ERROR: Invalid message data (null or zero length)");
		return;
	}

	// Exactly one transport carries anti-cheat traffic. When the plugin owns the
	// connection it drains its own anti-cheat channel in Tick(), so anything
	// arriving through the game is either a duplicate or a leftover from the
	// other transport and must not be replayed into the anti-cheat interface.
	if (DoesACPluginProvideSecureGameTransport())
	{
		PluginLog("[AC][EAC][REMOTE] Ignoring %u bytes from user %u delivered via the game transport - the plugin owns the anti-cheat transport",
			dataLen, sourceUserID);
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();

	if (acHandle == nullptr)
	{
		PluginLog("[AC][EAC][REMOTE] ERROR: AC handle is null");
		return;
	}

	EOS_AntiCheatClient_ReceiveMessageFromPeerOptions receiveOpts = {};
	receiveOpts.ApiVersion = EOS_ANTICHEATCLIENT_RECEIVEMESSAGEFROMPEER_API_LATEST;
	receiveOpts.PeerHandle = (void*)sourceUserID;
	receiveOpts.Data = data;
	receiveOpts.DataLengthBytes = dataLen;

	EOS_EResult receiveRes = EOS_AntiCheatClient_ReceiveMessageFromPeer(acHandle, &receiveOpts);
	if (receiveRes != EOS_EResult::EOS_Success)
	{
		PluginLog("[AC][EAC][REMOTE] MIDDLEWARE ERROR, EOS_AntiCheatClient_ReceiveMessageFromPeer: %s!", EOS_EResult_ToString(receiveRes));
	}
	else
	{
		PluginLog("[AC][EAC][REMOTE] AC RECEIVED MESSAGE FROM PEER: %u bytes (User %u)", dataLen, sourceUserID);
	}
}

// ------------------------------------------------------------
// Ping / pong latency measurement
//
// Every PING_INTERVAL the plugin sends a timestamped ping to each registered
// peer on ENetworkChannels::Ping. Peers echo the payload back byte-for-byte on
// ENetworkChannels::Pong, so the original sender can derive a round-trip time
// against its own clock - neither side needs a synchronised clock. The game
// never sees these channels; it only reads the result via
// GetConnectionLatencyForUser().
// ------------------------------------------------------------
#pragma pack(push, 1)
struct PingPayload
{
	uint32_t Magic;
	uint64_t SentMicroseconds;
};
#pragma pack(pop)

// 'GOPG' - guards against a stray packet on the ping channels being treated as
// a timestamp.
static constexpr uint32_t PING_PAYLOAD_MAGIC = 0x474F5047u;
static constexpr std::chrono::milliseconds PING_INTERVAL{ 1000 };

// Anything above this is treated as a bogus sample rather than real latency.
static constexpr uint64_t PING_MAX_PLAUSIBLE_RTT_MICROSECONDS = 30ull * 1000ull * 1000ull;

static std::chrono::steady_clock::time_point g_LastPingSendTime{};

// Defined below the transport section; driven from Tick().
static void ProcessIncomingPings();
static void ProcessIncomingPongs();
static void SendPingsIfDue();
static void ProcessIncomingACMessages();

static uint64_t NowMicroseconds()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

void Tick()
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	if (g_EOSPlatformHandle != nullptr)
	{
		EOS_Platform_Tick(g_EOSPlatformHandle);

		// Answer any pings we have been sent, then fold the replies to our own
		// pings into the latency table, then send the next round.
		ProcessIncomingPings();
		ProcessIncomingPongs();
		SendPingsIfDue();

		// When the plugin owns the transport it also carries anti-cheat
		// traffic, so drain that channel here rather than relying on the game.
		if (DoesACPluginProvideSecureGameTransport())
		{
			ProcessIncomingACMessages();
		}
	}
}

bool IsLoggedIn()
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	return g_EOSUserID != nullptr;
}

bool GetMiddlewareAuthToken(char* buffer, size_t bufferSize)
{
	// Validate caller-supplied buffer to prevent NULL dereference
	if (buffer == nullptr || bufferSize == 0)
	{
		PluginLog("[EAC] GetMiddlewareAuthToken: Invalid buffer (null or zero size)");
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	
	if (g_EOSPlatformHandle == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Platform not initialized!");
		return false;
	}

	if (g_EOSUserID == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Not logged in!");
		return false;
	}
	
	EOS_HConnect ConnectHandle = EOS_Platform_GetConnectInterface(g_EOSPlatformHandle);

	if (ConnectHandle == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Connect handle is null!");
		return false;
	}

	EOS_Connect_IdToken* epicToken = nullptr;

	EOS_Connect_CopyIdTokenOptions opts = {};
	opts.ApiVersion = EOS_CONNECT_COPYIDTOKEN_API_LATEST;
	opts.LocalUserId = g_EOSUserID;
	EOS_EResult res = EOS_Connect_CopyIdToken(ConnectHandle, &opts, &epicToken);

	if (res != EOS_EResult::EOS_Success)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: %s!", EOS_EResult_ToString(res));
		return false;
	}

	if (epicToken == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Token pointer is null!");
		return false;
	}

	// From here on the token is owned by us and must be released on every path.
	bool bSuccess = false;

	if (epicToken->JsonWebToken == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: JsonWebToken is null!");
	}
	else
	{
		const char* msg = epicToken->JsonWebToken;
		size_t len = strlen(msg) + 1;

		// Validate token length doesn't exceed a reasonable size
		if (len > 8192)
		{
			PluginLog("[EAC] MIDDLEWARE ERROR: Token too large!");
		}
		else if (bufferSize < len)
		{
			PluginLog("[EAC] MIDDLEWARE BAD SIZE!");
		}
		else
		{
			memcpy(buffer, msg, len);
			bSuccess = true;
		}
	}

	EOS_Connect_IdToken_Release(epicToken);

	return bSuccess;
}

EOS_NotificationId g_ConnectionRequestNotificationId = EOS_INVALID_NOTIFICATIONID;
EOS_NotificationId g_ConnectionEstablishedNotificationId = EOS_INVALID_NOTIFICATIONID;
EOS_NotificationId g_ConnectionInterruptedNotificationId = EOS_INVALID_NOTIFICATIONID;
EOS_NotificationId g_ConnectionClosedNotificationId = EOS_INVALID_NOTIFICATIONID;

EOS_P2P_SocketId g_SocketId = {};

// Removes every P2P notification we registered. Safe to call with a null handle
// or with nothing registered. Must be called with g_StateMutex held, and always
// before the platform that owns the notifications is released.
static void RemoveP2PNotifications(EOS_HP2P p2pHandle)
{
	if (p2pHandle != nullptr)
	{
		if (g_ConnectionRequestNotificationId != EOS_INVALID_NOTIFICATIONID)
		{
			EOS_P2P_RemoveNotifyPeerConnectionRequest(p2pHandle, g_ConnectionRequestNotificationId);
		}

		if (g_ConnectionEstablishedNotificationId != EOS_INVALID_NOTIFICATIONID)
		{
			EOS_P2P_RemoveNotifyPeerConnectionEstablished(p2pHandle, g_ConnectionEstablishedNotificationId);
		}

		if (g_ConnectionInterruptedNotificationId != EOS_INVALID_NOTIFICATIONID)
		{
			EOS_P2P_RemoveNotifyPeerConnectionInterrupted(p2pHandle, g_ConnectionInterruptedNotificationId);
		}

		if (g_ConnectionClosedNotificationId != EOS_INVALID_NOTIFICATIONID)
		{
			EOS_P2P_RemoveNotifyPeerConnectionClosed(p2pHandle, g_ConnectionClosedNotificationId);
		}
	}

	// Always reset the IDs: they belong to the platform that is going away and
	// must never be reused against a future one.
	g_ConnectionRequestNotificationId = EOS_INVALID_NOTIFICATIONID;
	g_ConnectionEstablishedNotificationId = EOS_INVALID_NOTIFICATIONID;
	g_ConnectionInterruptedNotificationId = EOS_INVALID_NOTIFICATIONID;
	g_ConnectionClosedNotificationId = EOS_INVALID_NOTIFICATIONID;
}

int Initialize(ConnectionStateChangedCallbackFunc connectionStateChangedCB)
{
    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

	g_SocketId.ApiVersion = EOS_P2P_SOCKETID_API_LATEST;
	strncpy_s(g_SocketId.SocketName, sizeof(g_SocketId.SocketName), "GO_SOCKET", _TRUNCATE);

    // The game hands this in on every Initialize() call, keep the latest one even
    // if we early out below so a re-init can't leave a stale pointer behind.
    g_fnConnectionStateChanged = connectionStateChangedCB;

    // Check if already initialized
    if (g_EOSPlatformHandle != nullptr)
    {
        PluginLog("[EAC] Already initialized - skipping re-initialization");
        return EInitializeResult_Success;
    }

    // NOTE: only wipe the player map on a genuine (re-)initialisation. Doing this
    // above the early-out would clear every registered player if the game called
    // Initialize() again mid-session.
    {
        std::lock_guard<std::mutex> userMapLock(g_UserMapMutex);
        g_UserMap.clear();
        g_PendingConnectionRequests.clear();
    }

    {
        std::lock_guard<std::mutex> latencyLock(g_LatencyMutex);
        g_LatencyMs.clear();
    }

    // Refuse to run with placeholder credentials: EOS_Platform_Create() would
    // return a null handle and every subsequent call would fail obscurely.
    {
        const char* credentials[] =
        {
            EAC_EOS_PRODUCT_ID,
            EAC_EOS_SANDBOX_ID,
            EAC_EOS_DEPLOYMENT_ID,
            EAC_EOS_CLIENT_ID,
            EAC_EOS_CLIENT_SECRET
        };

        for (const char* credential : credentials)
        {
            if (credential == nullptr || credential[0] == '\0' || strcmp(credential, "TODO") == 0)
            {
                PluginLog("[EAC] FATAL ERROR: EOS deployment credentials have not been configured.");
                PluginLog("[EAC] FATAL ERROR: Define EAC_EOS_PRODUCT_ID / EAC_EOS_SANDBOX_ID / EAC_EOS_DEPLOYMENT_ID / EAC_EOS_CLIENT_ID / EAC_EOS_CLIENT_SECRET at build time.");
                return EInitializeResult_MissingCredentials;
            }
        }
    }

	// Init EOS SDK
	EOS_InitializeOptions SDKOptions = {};
	SDKOptions.ApiVersion = EOS_INITIALIZE_API_LATEST;
	SDKOptions.AllocateMemoryFunction = nullptr;
	SDKOptions.ReallocateMemoryFunction = nullptr;
	SDKOptions.ReleaseMemoryFunction = nullptr;

	static char szBuffer[MAX_PATH] = { 0 };
	strcpy_s(szBuffer, sizeof(szBuffer), "GOClient");
	SDKOptions.ProductName = szBuffer;
    SDKOptions.ProductVersion = "1.0";
    SDKOptions.Reserved = nullptr;
    SDKOptions.SystemInitializeOptions = nullptr;
    SDKOptions.OverrideThreadAffinity = nullptr;

    EOS_EResult InitResult = EOS_Initialize(&SDKOptions);

    if (InitResult == EOS_EResult::EOS_Success)
    {
        // LOGGING
        EOS_EResult SetLogCallbackResult = EOS_Logging_SetCallback([](const EOS_LogMessage* Message)
            {
                if (Message == nullptr)
                {
                    return;
                }
                const char* category = Message->Category ? Message->Category : "UNKNOWN";
                const char* msg = Message->Message ? Message->Message : "(null)";
                PluginLog("[EOS - %s] %s", category, msg);
            });
        if (SetLogCallbackResult != EOS_EResult::EOS_Success)
        {
            PluginLog("[EAC] Set Logging Callback Failed!");
        }
        else
        {
            PluginLog("[EAC] Logging Callback Set");
#if _DEBUG
			const EOS_ELogLevel logLevel = EOS_ELogLevel::EOS_LOG_Verbose;
#else
			// Shipping builds only surface errors - VeryVerbose floods the game
			// log and costs measurable frame time.
			const EOS_ELogLevel logLevel = EOS_ELogLevel::EOS_LOG_Error;
#endif
			EOS_EResult SetLogLevelResult = EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, logLevel);
			if (SetLogLevelResult != EOS_EResult::EOS_Success)
			{
				PluginLog("[EAC] Set Logging Level Failed!");
			}
        }
		
        std::filesystem::path tempPath = std::filesystem::current_path();
        tempPath.append("cache");

        std::string strCachePath = tempPath.string();

        // PLATFORM OPTIONS
        EOS_Platform_Options PlatformOptions = {};
        PlatformOptions.ApiVersion = EOS_PLATFORM_OPTIONS_API_LATEST;
        PlatformOptions.bIsServer = EOS_FALSE;
        PlatformOptions.OverrideCountryCode = nullptr;
        PlatformOptions.OverrideLocaleCode = nullptr;
        PlatformOptions.Flags = EOS_PF_WINDOWS_ENABLE_OVERLAY_D3D9 | EOS_PF_WINDOWS_ENABLE_OVERLAY_D3D10;
        PlatformOptions.CacheDirectory = strCachePath.c_str();

        PlatformOptions.ProductId = EAC_EOS_PRODUCT_ID;
        PlatformOptions.SandboxId = EAC_EOS_SANDBOX_ID;

        // Only used by Player Data Storage / Title Storage, neither of which this
        // plugin touches. eos_types.h requires it to be null when unused, or
        // exactly EOS_PLATFORM_OPTIONS_ENCRYPTIONKEY_LENGTH (64) hex characters -
        // a shorter placeholder is rejected rather than ignored.
        PlatformOptions.EncryptionKey = nullptr;

        PlatformOptions.DeploymentId = EAC_EOS_DEPLOYMENT_ID;

        PlatformOptions.ClientCredentials.ClientId = EAC_EOS_CLIENT_ID;
        PlatformOptions.ClientCredentials.ClientSecret = EAC_EOS_CLIENT_SECRET;

        // Seconds, not milliseconds. Static so the pointer stays valid for the
        // lifetime of the platform regardless of when the SDK reads it.
        static double s_taskNetworkTimeoutSeconds = 5.0;
        PlatformOptions.TaskNetworkTimeoutSeconds = &s_taskNetworkTimeoutSeconds;

        EOS_Platform_RTCOptions RtcOptions = {};
        RtcOptions.ApiVersion = EOS_PLATFORM_RTCOPTIONS_API_LATEST;

#ifdef _WIN32
        // Get absolute path for xaudio2_9redist.dll file
        char CurDir[MAX_PATH + 1] = {};
        ::GetCurrentDirectoryA(MAX_PATH, CurDir);

        // get exe path
        char buffer[MAX_PATH] = {};
        GetModuleFileNameA(NULL, buffer, MAX_PATH - 1);
        buffer[MAX_PATH - 1] = '\0';  // Ensure null termination
        std::string::size_type pos = std::string(buffer).find_last_of("\\/");
        
        if (pos == std::string::npos)
        {
            PluginLog("FATAL ERROR: Failed to parse executable path");
            EOS_Shutdown();
            return EInitializeResult_BadEnvironment;
        }
        
        std::string ExePath = std::string(buffer).substr(0, pos);

        // Validate that the exe path contains only ASCII characters.
        // Non-ASCII characters (e.g. Cyrillic in a copied installation directory)
        // can cause EOS SDK path APIs to silently return null handles, leading to
        // an EXCEPTION_ACCESS_VIOLATION_WRITE at shutdown.
        for (unsigned char c : ExePath)
        {
            if (c > 127)
            {
                PluginLog("FATAL ERROR: Game installation path contains non-ASCII characters: %s", ExePath.c_str());
                PluginLog("FATAL ERROR: Please move the game to a directory with only ASCII characters (A-Z, 0-9, etc.).");
                EOS_Shutdown();
                return EInitializeResult_BadEnvironment;
            }
        }

        std::string XAudio29DllPath = ExePath;
        XAudio29DllPath.append("\\xaudio2_9redist.dll");

        PluginLog("Current Directory: %s", CurDir);
        PluginLog("EXE Directory: %s", ExePath.c_str());
        PluginLog("XAudio Path: %s", XAudio29DllPath.c_str());

		// does the DLL exist on disk?
		std::fstream fileStream;
		fileStream.open(XAudio29DllPath.c_str(), std::fstream::in | std::fstream::binary);
		if (!fileStream.good())
		{
			PluginLog("FATAL ERROR: Failed to locate XAudio DLL");
			EOS_Shutdown();
			return EInitializeResult_BadEnvironment;
		}
		else
		{
			PluginLog("XAudio DLL located successfully");
		}

		EOS_Windows_RTCOptions WindowsRtcOptions = { 0 };
		WindowsRtcOptions.ApiVersion = EOS_WINDOWS_RTCOPTIONS_API_LATEST;
		WindowsRtcOptions.XAudio29DllPath = XAudio29DllPath.c_str();
		RtcOptions.PlatformSpecificOptions = &WindowsRtcOptions;
#else
        RtcOptions.PlatformSpecificOptions = nullptr;
#endif // _WIN32

        PlatformOptions.RTCOptions = &RtcOptions;

#if ALLOW_RESERVED_PLATFORM_OPTIONS
        SetReservedPlatformOptions(PlatformOptions);
#else
        PlatformOptions.Reserved = NULL;
#endif // ALLOW_RESERVED_PLATFORM_OPTIONS

        // platform integration settings
        // Create the generic container.
        const EOS_IntegratedPlatform_CreateIntegratedPlatformOptionsContainerOptions CreateOptions =
        {
            EOS_INTEGRATEDPLATFORM_CREATEINTEGRATEDPLATFORMOPTIONSCONTAINER_API_LATEST
        };

        const EOS_EResult Result = EOS_IntegratedPlatform_CreateIntegratedPlatformOptionsContainer(&CreateOptions, &PlatformOptions.IntegratedPlatformOptionsContainerHandle);

        if (Result != EOS_EResult::EOS_Success)
        {
            PluginLog("EOS_IntegratedPlatform_CreateIntegratedPlatformOptionsContainer returned an error");
            EOS_Shutdown();
            return EInitializeResult_ContainerFailed;
        }

        g_EOSPlatformHandle = EOS_Platform_Create(&PlatformOptions);

        // The container is only read during EOS_Platform_Create; release it on
        // both paths so it is not leaked for the lifetime of the process.
        EOS_IntegratedPlatformOptionsContainer_Release(PlatformOptions.IntegratedPlatformOptionsContainerHandle);
        PlatformOptions.IntegratedPlatformOptionsContainerHandle = nullptr;

        if (g_EOSPlatformHandle == nullptr)
        {
            PluginLog("FATAL ERROR: EOS_Platform_Create failed - returned null handle");
            EOS_Shutdown();
            return EInitializeResult_PlatformCreateFailed;
        }
        // END PLATFORM OPTIONS

        return EInitializeResult_Success;
    }
    else
    {
        PluginLog("[EAC] INIT FAILED: %s", EOS_EResult_ToString(InitResult));
        return EInitializeResult_SDKInitFailed;
    }
}

PLUGIN_API void Shutdown()
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	if (g_EOSPlatformHandle == nullptr)
	{
		// Already shut down or never initialized; nothing to do.
		return;
	}

	// P2P notifications belong to the platform we are about to release, so they
	// must be unsubscribed first.
	RemoveP2PNotifications(GetP2PHandle());

	EOS_Platform_Release(g_EOSPlatformHandle);
	g_EOSPlatformHandle = nullptr;

	EOS_Shutdown();
	
	// Reset EOS-owned state. The notification IDs belong to the released
	// platform and must not be reused against a future one.
	g_EOSUserID = nullptr;
	g_goUserID = 0;
	g_LoginCallback = nullptr;
	g_bEventsHooked = false;
	g_NotifyClientIntegrityViolatedId = 0;
	g_NotifyMessageToPeerId = 0;
	g_NotifyPeerAuthStatusChangedId = 0;
	g_NotifyPeerActionRequiredId = 0;

	// NOTE: The host-supplied callbacks (logging, anti-cheat, transport) are
	// deliberately preserved. They are owned by the host, outlive the EOS
	// platform, and clearing them here would silently disable logging and all
	// anti-cheat notifications after a shutdown/re-initialise cycle.
}

bool IsExternalProcessRunning()
{
#if _DEBUG
	return true;
#else
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	return GetAntiCheatHandle() != nullptr;
#endif
}

PLUGIN_API int GetAnticheatIdentifier()
{
	return 9481;
}

// ------------------------------------------------------------
// Transport API
//
// This plugin owns the game transport: DoesACPluginProvideSecureGameTransport()
// reports true, so the game routes both game and anti-cheat traffic through the
// EOS P2P implementation below instead of its own mesh/WebSocket transport.
//
// Every entry point here can be called before Login() or after Shutdown(), so
// they all validate the platform handle and the local user ID before touching
// the SDK - EOS_Platform_GetP2PInterface(nullptr) would access-violate.
// ------------------------------------------------------------
PLUGIN_API bool DoesACPluginProvideSecureGameTransport()
{
	return true;
}

// Grabs everything needed to talk to the P2P interface. Returns false (and logs)
// if the transport is not usable yet.
static bool AcquireP2PContext(const char* context, EOS_HP2P& outHandle, EOS_ProductUserId& outLocalUser)
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

	outHandle = GetP2PHandle();
	outLocalUser = g_EOSUserID;

	if (outHandle == nullptr)
	{
		PluginLog("[EAC] %s: P2P interface unavailable (platform not initialised)", context);
		return false;
	}

	if (outLocalUser == nullptr)
	{
#if _DEBUG
		PluginLog("[EAC] %s: no local EOS user (not logged in)", context);
#endif
		return false;
	}

	return true;
}

// Parses a middleware user ID, logging and failing instead of handing a null
// (or a null-derived) product user ID to the SDK.
static bool ResolveRemoteUser(const char* context, const char* middlewareUserID, EOS_ProductUserId& outRemoteUser)
{
	if (middlewareUserID == nullptr || middlewareUserID[0] == '\0')
	{
		PluginLog("[EAC] %s: null/empty middleware user ID", context);
		return false;
	}

	outRemoteUser = EOS_ProductUserId_FromString(middlewareUserID);

	if (outRemoteUser == nullptr)
	{
		PluginLog("[EAC] %s: malformed middleware user ID '%s'", context, middlewareUserID);
		return false;
	}

	return true;
}

// Accepts an incoming P2P connection from a peer the game has vouched for.
// Callers must have confirmed the peer is registered; this only performs the
// EOS side of the handshake.
static bool AcceptP2PConnection(const char* context, EOS_ProductUserId remoteUser, const char* middlewareUserID)
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

	EOS_HP2P p2pHandle = GetP2PHandle();

	if (p2pHandle == nullptr || g_EOSUserID == nullptr || remoteUser == nullptr)
	{
		return false;
	}

	EOS_P2P_AcceptConnectionOptions acceptOptions = {};
	acceptOptions.ApiVersion = EOS_P2P_ACCEPTCONNECTION_API_LATEST;
	acceptOptions.LocalUserId = g_EOSUserID;
	acceptOptions.RemoteUserId = remoteUser;
	acceptOptions.SocketId = &g_SocketId;

	EOS_EResult acceptResult = EOS_P2P_AcceptConnection(p2pHandle, &acceptOptions);

	if (acceptResult != EOS_EResult::EOS_Success)
	{
		PluginLog("[EAC] %s: failed to accept connection from %s: %s",
			context, middlewareUserID, EOS_EResult_ToString(acceptResult));
		return false;
	}

	return true;
}

static void LogSendPacketResult(const char* context, const char* middlewareUserID, EOS_EResult result)
{
	if (result == EOS_EResult::EOS_Success)
	{
		return;
	}

	if (result == EOS_EResult::EOS_LimitExceeded)
	{
		PluginLog("[EAC] %s: packet to %s rejected - too large or outgoing queue full (%s)",
			context, middlewareUserID, EOS_EResult_ToString(result));
	}
	else
	{
		PluginLog("[EAC] %s: failed to send packet to %s: %s",
			context, middlewareUserID, EOS_EResult_ToString(result));
	}
}

PLUGIN_API void StartSignalling(const char* middlewareUserID, uint64_t goUserID)
{
	(void)goUserID;

	EOS_HP2P P2PHandle = nullptr;
	EOS_ProductUserId localUser = nullptr;
	EOS_ProductUserId targetPUID = nullptr;

	if (!AcquireP2PContext("StartSignalling", P2PHandle, localUser) ||
		!ResolveRemoteUser("StartSignalling", middlewareUserID, targetPUID))
	{
		return;
	}

	// NOTE: do NOT close an existing connection to this peer here. Both sides
	// signal each other, so closing on every signal means the second peer tears
	// down the connection the first one just established and the game sees a
	// disconnect. Connections left over from a previous session are purged once
	// per session in BeginSession()/EndSession() instead.

	// Set the options for sending the message.
	EOS_P2P_SendPacketOptions SendPacketOptions = {};
	SendPacketOptions.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;

	// Set the Product User ID of the local player sending a message.
	SendPacketOptions.LocalUserId = localUser;

	// Set the Product User ID of the remote player to send a message to.
	SendPacketOptions.RemoteUserId = targetPUID;

	// Set the socket ID for the P2P connection.
	SendPacketOptions.SocketId = &g_SocketId;

	// Set a boolean to specify whether to delay sending the message if the remote player is not currently connected.
	// If you set this to false and a connection is not established with the peer, this data will be dropped.
	SendPacketOptions.bAllowDelayedDelivery = EOS_TRUE;

	// Signalling has its own channel so it can never be confused with game or
	// anti-cheat traffic on the receiving side.
	SendPacketOptions.Channel = (uint8_t)ENetworkChannels::Signalling;

	// Set an enum to indicate the reliability and order of sent packets. 
	SendPacketOptions.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;

	// We are the initiator here and the target was chosen by the game, so the
	// outgoing connection is opened automatically. Inbound connection requests
	// are NOT automatic: they are vetted against g_UserMap and explicitly
	// accepted in the EOS_P2P_AddNotifyPeerConnectionRequest handler.
	SendPacketOptions.bDisableAutoAcceptConnection = EOS_FALSE;

	// Set the length in bytes of the message to send to the remote player.
	std::string Message = "HELLO";
	SendPacketOptions.DataLengthBytes = static_cast<uint32_t>(Message.length());

	// Set the message to send to the remote player.
	SendPacketOptions.Data = Message.data();

	// Call the EOS SDK to send the message to the remote player. 
	LogSendPacketResult("StartSignalling", middlewareUserID, EOS_P2P_SendPacket(P2PHandle, &SendPacketOptions));
}

PLUGIN_API void SendPacket(const char* middlewareUserID, uint64_t targetGoUserID, void* data, int numBytes, ENetworkChannels channel, EPacketReliability reliability)
{
	(void)targetGoUserID;

	if (data == nullptr || numBytes <= 0)
	{
		PluginLog("[EAC] SendPacket: refusing to send %d bytes from %p", numBytes, data);
		return;
	}

	if (numBytes > EOS_P2P_MAX_PACKET_SIZE)
	{
		PluginLog("[EAC] SendPacket: packet of %d bytes exceeds the EOS maximum of %d - dropping",
			numBytes, (int)EOS_P2P_MAX_PACKET_SIZE);
		return;
	}

	EOS_HP2P P2PHandle = nullptr;
	EOS_ProductUserId localUser = nullptr;
	EOS_ProductUserId targetPUID = nullptr;

	if (!AcquireP2PContext("SendPacket", P2PHandle, localUser) ||
		!ResolveRemoteUser("SendPacket", middlewareUserID, targetPUID))
	{
		return;
	}

	// Set the options for sending the message.
	EOS_P2P_SendPacketOptions SendPacketOptions = {};
	SendPacketOptions.ApiVersion = EOS_P2P_SENDPACKET_API_LATEST;

	// Set the Product User ID of the local player sending a message.
	SendPacketOptions.LocalUserId = localUser;

	// Set the Product User ID of the remote player to send a message to.
	SendPacketOptions.RemoteUserId = targetPUID;

	// Set the socket ID for the P2P connection.
	SendPacketOptions.SocketId = &g_SocketId;

	// Set a boolean to specify whether to delay sending the message if the remote player is not currently connected.
	// If you set this to false and a connection is not established with the peer, this data will be dropped.
	SendPacketOptions.bAllowDelayedDelivery = EOS_TRUE;

	// Send on the channel the game asked for. The game polls each channel
	// separately in RecvPacket(), so forcing everything onto one channel here
	// would make the traffic undeliverable.
	SendPacketOptions.Channel = (uint8_t)channel;

	// Set an enum to indicate the reliability and order of sent packets. 
	if (reliability == EPacketReliability::PACKET_RELIABILITY_UNRELIABLE_UNORDERED)
	{
		SendPacketOptions.Reliability = EOS_EPacketReliability::EOS_PR_UnreliableUnordered;
	}
	else if (reliability == EPacketReliability::PACKET_RELIABILITY_RELIABLE_UNORDERED)
	{
		SendPacketOptions.Reliability = EOS_EPacketReliability::EOS_PR_ReliableUnordered;
	}
	else if (reliability == EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED)
	{
		SendPacketOptions.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
	}
	else
	{
		PluginLog("[EAC] SendPacket: unknown reliability %d, defaulting to reliable-ordered", (int)reliability);
		SendPacketOptions.Reliability = EOS_EPacketReliability::EOS_PR_ReliableOrdered;
	}

	// See StartSignalling(): inbound connections are vetted in the connection
	// request handler, outbound ones are initiated deliberately by the game.
	SendPacketOptions.bDisableAutoAcceptConnection = EOS_FALSE;

	// Set the length in bytes of the message to send to the remote player.
	SendPacketOptions.DataLengthBytes = (uint32_t)numBytes;

	// Set the message to send to the remote player.
	SendPacketOptions.Data = data;

	// Call the EOS SDK to send the message to the remote player. 
	LogSendPacketResult("SendPacket", middlewareUserID, EOS_P2P_SendPacket(P2PHandle, &SendPacketOptions));
}

// Core receive used by both the exported RecvPacket() and the internal
// ping/pong handling. Fills a caller-owned buffer and reports which peer the
// packet came from.
static bool ReceivePacketCore(const char* context, uint8_t channelToReceiveOn, uint8_t* buffer, uint32_t bufferSize, uint32_t& outBytesWritten, EOS_ProductUserId& outSender)
{
	outBytesWritten = 0;
	outSender = nullptr;

	EOS_HP2P P2PHandle = nullptr;
	EOS_ProductUserId localUser = nullptr;

	if (!AcquireP2PContext(context, P2PHandle, localUser))
	{
		return false;
	}

	EOS_P2P_ReceivePacketOptions Options = {};
	Options.ApiVersion = EOS_P2P_RECEIVEPACKET_API_LATEST;
	Options.LocalUserId = localUser;
	Options.MaxDataSizeBytes = bufferSize;
	Options.RequestedChannel = &channelToReceiveOn;

	uint8_t channelReceivedOn = 0;

	// NOTE: this is an OUT parameter - EOS overwrites it with the socket the
	// packet arrived on, so it must be a local and never the shared g_SocketId.
	EOS_P2P_SocketId receivedSocketId = {};

	EOS_EResult Result = EOS_P2P_ReceivePacket(P2PHandle, &Options, &outSender, &receivedSocketId, &channelReceivedOn, buffer, &outBytesWritten);

	if (Result != EOS_EResult::EOS_Success)
	{
		// EOS_NotFound simply means the queue is empty, which is the common case
		// on every idle poll - anything else is a genuine error worth logging.
		if (Result != EOS_EResult::EOS_NotFound)
		{
			PluginLog("[EAC] %s: channel %u failed: %s",
				context, (unsigned)channelToReceiveOn, EOS_EResult_ToString(Result));
		}

		outBytesWritten = 0;
		outSender = nullptr;
		return false;
	}

	return true;
}

PLUGIN_API int GetNextRecvPacketSize(uint8_t channelToReceiveOn)
{
	EOS_HP2P P2PHandle = nullptr;
	EOS_ProductUserId localUser = nullptr;

	if (!AcquireP2PContext("GetNextRecvPacketSize", P2PHandle, localUser))
	{
		return 0;
	}

	uint32_t size = 0;

	EOS_P2P_GetNextReceivedPacketSizeOptions opts = {};
	opts.ApiVersion = EOS_P2P_GETNEXTRECEIVEDPACKETSIZE_API_LATEST;
	opts.LocalUserId = localUser;
	opts.RequestedChannel = &channelToReceiveOn;

	EOS_EResult result = EOS_P2P_GetNextReceivedPacketSize(P2PHandle, &opts, &size);

	if (result != EOS_EResult::EOS_Success)
	{
		// EOS_NotFound simply means the queue is empty, which is the common case
		// on every idle poll - anything else is a genuine error worth logging.
		if (result != EOS_EResult::EOS_NotFound)
		{
			PluginLog("[EAC] GetNextRecvPacketSize: channel %u failed: %s",
				(unsigned)channelToReceiveOn, EOS_EResult_ToString(result));
		}

		return 0;
	}

	return (int)size;
}

PLUGIN_API bool RecvPacket(uint8_t** outData, uint8_t channelToReceiveOn)
{
	if (outData == nullptr)
	{
		return false;
	}

	*outData = nullptr;

	int bufSize = GetNextRecvPacketSize(channelToReceiveOn);

	if (bufSize <= 0)
	{
		return false;
	}

	uint8_t* pOutData = (uint8_t*)malloc((size_t)bufSize);

	if (pOutData == nullptr)
	{
		PluginLog("[EAC] RecvPacket: out of memory allocating %d bytes", bufSize);
		return false;
	}

	uint32_t bytesWritten = 0;
	EOS_ProductUserId sender = nullptr;

	if (!ReceivePacketCore("RecvPacket", channelToReceiveOn, pOutData, (uint32_t)bufSize, bytesWritten, sender))
	{
		free(pOutData);
		return false;
	}

	// Hand ownership of the buffer to the game; it releases it via FreePacket().
	*outData = pOutData;
	return true;
}

PLUGIN_API void FreePacket(void* packetData)
{
	// Must match the malloc() in RecvPacket(). Using delete here would be both
	// an allocator mismatch and undefined behaviour on a void*.
	free(packetData);
}

PLUGIN_API bool IsConnectionRelayed(const char* middlewareUserID, uint32_t goUserID)
{
	(void)goUserID;

	if (middlewareUserID == nullptr || middlewareUserID[0] == '\0')
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(g_ConnectionTypeMutex);

	auto it = g_ConnectionType.find(middlewareUserID);
	if (it == g_ConnectionType.end())
	{
		return false;
	}

	return it->second == EOS_ENetworkConnectionType::EOS_NCT_RelayedConnection;
}

PLUGIN_API int GetConnectionLatencyForUser(const char* middlewareUserID, uint32_t goUserID)
{
	(void)goUserID;

	if (middlewareUserID == nullptr || middlewareUserID[0] == '\0')
	{
		return 0;
	}

	std::lock_guard<std::mutex> lock(g_LatencyMutex);

	auto it = g_LatencyMs.find(middlewareUserID);
	if (it == g_LatencyMs.end())
	{
		// No pong seen yet - the game treats 0 as "unknown".
		return 0;
	}

	return (int)it->second;
}

PLUGIN_API void DisconnectPlayer(const char* middlewareUserID, uint64_t goUserID)
{
	(void)goUserID;

	EOS_HP2P P2PHandle = nullptr;
	EOS_ProductUserId localUser = nullptr;
	EOS_ProductUserId targetPUID = nullptr;

	if (!AcquireP2PContext("DisconnectPlayer", P2PHandle, localUser) ||
		!ResolveRemoteUser("DisconnectPlayer", middlewareUserID, targetPUID))
	{
		return;
	}

	EOS_P2P_CloseConnectionOptions opts = {};
	opts.ApiVersion = EOS_P2P_CLOSECONNECTION_API_LATEST;
	opts.LocalUserId = localUser;
	opts.RemoteUserId = targetPUID;
	opts.SocketId = &g_SocketId;

	EOS_EResult result = EOS_P2P_CloseConnection(P2PHandle, &opts);

	if (result != EOS_EResult::EOS_Success)
	{
		PluginLog("[EAC] DisconnectPlayer: failed to close connection to %s: %s",
			middlewareUserID, EOS_EResult_ToString(result));
	}
}

PLUGIN_API void DisconnectAll()
{
	EOS_HP2P P2PHandle = nullptr;
	EOS_ProductUserId localUser = nullptr;

	if (!AcquireP2PContext("DisconnectAll", P2PHandle, localUser))
	{
		return;
	}

	EOS_P2P_CloseConnectionsOptions opts = {};
	opts.ApiVersion = EOS_P2P_CLOSECONNECTIONS_API_LATEST;
	opts.LocalUserId = localUser;
	opts.SocketId = &g_SocketId;

	EOS_EResult result = EOS_P2P_CloseConnections(P2PHandle, &opts);

	if (result != EOS_EResult::EOS_Success)
	{
		PluginLog("[EAC] DisconnectAll: failed to close connections: %s", EOS_EResult_ToString(result));
	}
}

// ------------------------------------------------------------
// Ping / pong implementation (declared above Tick())
// ------------------------------------------------------------

// Reads one ping/pong payload out of the given channel. Returns false when the
// queue is drained or the packet is not a well-formed payload from a peer we
// know about; *outMore tells the caller whether to keep draining.
static bool ReadPingPayload(const char* context, ENetworkChannels channel, PingPayload& outPayload, std::string& outSenderID, uint32_t& outSenderGoUserID, bool& outMore)
{
	uint8_t buffer[EOS_P2P_MAX_PACKET_SIZE] = {};
	uint32_t bytesWritten = 0;
	EOS_ProductUserId sender = nullptr;

	if (!ReceivePacketCore(context, (uint8_t)channel, buffer, (uint32_t)sizeof(buffer), bytesWritten, sender))
	{
		outMore = false;
		return false;
	}

	// A packet was consumed, so there may be more behind it even if this one is
	// rejected below.
	outMore = true;

	if (bytesWritten != sizeof(PingPayload))
	{
		return false;
	}

	memcpy(&outPayload, buffer, sizeof(PingPayload));

	if (outPayload.Magic != PING_PAYLOAD_MAGIC)
	{
		return false;
	}

	if (!ProductUserIdToString(sender, outSenderID))
	{
		return false;
	}

	// Only talk to players the game registered for this session.
	return TryGetGoUserID(outSenderID, outSenderGoUserID);
}

static void ProcessIncomingPings()
{
	bool bMore = true;

	while (bMore)
	{
		PingPayload payload = {};
		std::string senderID;
		uint32_t senderGoUserID = 0;

		if (!ReadPingPayload("Ping", ENetworkChannels::Ping, payload, senderID, senderGoUserID, bMore))
		{
			continue;
		}

		// Echo the payload back verbatim: SentMicroseconds belongs to the
		// sender's clock and must not be rewritten, otherwise the RTT it
		// computes would be meaningless.
		SendPacket(senderID.c_str(), senderGoUserID, &payload, (int)sizeof(payload),
			ENetworkChannels::Pong, EPacketReliability::PACKET_RELIABILITY_UNRELIABLE_UNORDERED);
	}
}

static void ProcessIncomingPongs()
{
	bool bMore = true;

	while (bMore)
	{
		PingPayload payload = {};
		std::string senderID;
		uint32_t senderGoUserID = 0;

		if (!ReadPingPayload("Pong", ENetworkChannels::Pong, payload, senderID, senderGoUserID, bMore))
		{
			continue;
		}

		const uint64_t nowMicroseconds = NowMicroseconds();

		// The timestamp is one we generated, so anything in the future or
		// implausibly old is a corrupt or replayed packet.
		if (payload.SentMicroseconds > nowMicroseconds)
		{
			continue;
		}

		const uint64_t rttMicroseconds = nowMicroseconds - payload.SentMicroseconds;

		if (rttMicroseconds > PING_MAX_PLAUSIBLE_RTT_MICROSECONDS)
		{
			continue;
		}

		const uint32_t rttMilliseconds = (uint32_t)((rttMicroseconds + 500ull) / 1000ull);

		std::lock_guard<std::mutex> lock(g_LatencyMutex);

		auto it = g_LatencyMs.find(senderID);
		if (it == g_LatencyMs.end())
		{
			g_LatencyMs[senderID] = rttMilliseconds;
		}
		else
		{
			// Light smoothing so a single delayed pong does not make the
			// displayed latency jump around.
			it->second = (uint32_t)(((uint64_t)it->second * 3ull + rttMilliseconds) / 4ull);
		}
	}
}

static void SendPingsIfDue()
{
	const auto now = std::chrono::steady_clock::now();

	if (g_LastPingSendTime.time_since_epoch().count() != 0 && (now - g_LastPingSendTime) < PING_INTERVAL)
	{
		return;
	}

	g_LastPingSendTime = now;

	// RegisterPlayer() also records the local player, who must never be pinged.
	std::string localUserID;
	ProductUserIdToString(g_EOSUserID, localUserID);

	std::vector<std::pair<std::string, uint32_t>> peers;
	{
		std::lock_guard<std::mutex> userMapLock(g_UserMapMutex);
		peers.assign(g_UserMap.begin(), g_UserMap.end());
	}

	PingPayload payload = {};
	payload.Magic = PING_PAYLOAD_MAGIC;
	payload.SentMicroseconds = NowMicroseconds();

	for (const auto& peer : peers)
	{
		if (!localUserID.empty() && peer.first == localUserID)
		{
			continue;
		}

		SendPacket(peer.first.c_str(), peer.second, &payload, (int)sizeof(payload),
			ENetworkChannels::Ping, EPacketReliability::PACKET_RELIABILITY_UNRELIABLE_UNORDERED);
	}
}

// ------------------------------------------------------------
// Anti-cheat message transport
//
// When DoesACPluginProvideSecureGameTransport() is true the plugin owns the
// network connection, so anti-cheat traffic is carried over the plugin's own
// EOS P2P connection on ENetworkChannels::Anticheat instead of being handed
// back to the game. EOS caps a packet at EOS_P2P_MAX_PACKET_SIZE but anti-cheat
// messages can be larger, so they are fragmented here and reassembled on the
// far side. The channel is reliable-ordered, which means fragments arrive in
// the order they were sent and reassembly is a simple append.
// ------------------------------------------------------------
#pragma pack(push, 1)
struct ACFragmentHeader
{
	uint32_t Magic;
	uint32_t TotalBytes;
	uint16_t FragmentIndex;
	uint16_t FragmentCount;
};
#pragma pack(pop)

// 'GOAC' - guards against a stray packet on the anti-cheat channel being fed
// into the anti-cheat interface as a message.
static constexpr uint32_t AC_FRAGMENT_MAGIC = 0x474F4143u;
static constexpr uint32_t AC_FRAGMENT_PAYLOAD_MAX = (uint32_t)EOS_P2P_MAX_PACKET_SIZE - (uint32_t)sizeof(ACFragmentHeader);

// An anti-cheat message larger than this is treated as corrupt rather than
// being allowed to allocate arbitrary memory from a remote peer's say-so.
static constexpr uint32_t AC_MESSAGE_MAX_BYTES = 1024u * 1024u;

// Partially received anti-cheat messages, keyed by sender middleware user ID.
// Only touched from Tick(), which holds g_StateMutex.
struct ACReassemblyBuffer
{
	std::vector<uint8_t> Data;
	uint32_t TotalBytes = 0;
	uint16_t FragmentCount = 0;
	uint16_t NextFragmentIndex = 0;
};

static std::unordered_map<std::string, ACReassemblyBuffer> g_ACReassembly;

// Sends an anti-cheat message to a peer over the plugin's own P2P connection,
// fragmenting it if it does not fit in a single EOS packet. Returns false if
// the peer could not be addressed, so the caller can fall back to the game.
static bool SendACMessageViaPluginTransport(uint32_t targetGoUserID, const void* data, uint32_t dataLen)
{
	if (data == nullptr || dataLen == 0)
	{
		return false;
	}

	if (dataLen > AC_MESSAGE_MAX_BYTES)
	{
		PluginLog("[EAC][REMOTE] AC message of %u bytes is implausibly large - dropping", dataLen);
		return false;
	}

	std::string middlewareUserID;
	if (!TryGetMiddlewareUserID(targetGoUserID, middlewareUserID))
	{
		PluginLog("[EAC][REMOTE] AC transport: no middleware user ID for game user %u", targetGoUserID);
		return false;
	}

	const uint32_t fragmentCount = (dataLen + AC_FRAGMENT_PAYLOAD_MAX - 1) / AC_FRAGMENT_PAYLOAD_MAX;

	if (fragmentCount > 0xFFFFu)
	{
		PluginLog("[EAC][REMOTE] AC message of %u bytes needs too many fragments - dropping", dataLen);
		return false;
	}

	const uint8_t* source = static_cast<const uint8_t*>(data);
	uint8_t packet[EOS_P2P_MAX_PACKET_SIZE] = {};

	for (uint32_t fragmentIndex = 0; fragmentIndex < fragmentCount; ++fragmentIndex)
	{
		const uint32_t offset = fragmentIndex * AC_FRAGMENT_PAYLOAD_MAX;
		const uint32_t chunk = (dataLen - offset) < AC_FRAGMENT_PAYLOAD_MAX ? (dataLen - offset) : AC_FRAGMENT_PAYLOAD_MAX;

		ACFragmentHeader header = {};
		header.Magic = AC_FRAGMENT_MAGIC;
		header.TotalBytes = dataLen;
		header.FragmentIndex = (uint16_t)fragmentIndex;
		header.FragmentCount = (uint16_t)fragmentCount;

		memcpy(packet, &header, sizeof(header));
		memcpy(packet + sizeof(header), source + offset, chunk);

		// Anti-cheat traffic must not be reordered or dropped, or the peer's
		// session fails to validate and the player is kicked.
		SendPacket(middlewareUserID.c_str(), targetGoUserID, packet, (int)(sizeof(header) + chunk),
			ENetworkChannels::AnticheatSecure, EPacketReliability::PACKET_RELIABILITY_RELIABLE_ORDERED);
	}

	return true;
}

// Hands a fully reassembled anti-cheat message to the EOS anti-cheat interface.
static void DeliverACMessage(uint32_t senderGoUserID, const uint8_t* data, uint32_t dataLen)
{
	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();

	if (acHandle == nullptr)
	{
		PluginLog("[AC][EAC][REMOTE] AC transport: AC handle is null, dropping %u bytes", dataLen);
		return;
	}

	EOS_AntiCheatClient_ReceiveMessageFromPeerOptions receiveOpts = {};
	receiveOpts.ApiVersion = EOS_ANTICHEATCLIENT_RECEIVEMESSAGEFROMPEER_API_LATEST;
	receiveOpts.PeerHandle = (void*)senderGoUserID;
	receiveOpts.Data = data;
	receiveOpts.DataLengthBytes = dataLen;

	EOS_EResult receiveRes = EOS_AntiCheatClient_ReceiveMessageFromPeer(acHandle, &receiveOpts);

	if (receiveRes != EOS_EResult::EOS_Success)
	{
		PluginLog("[AC][EAC][REMOTE] MIDDLEWARE ERROR, EOS_AntiCheatClient_ReceiveMessageFromPeer: %s!",
			EOS_EResult_ToString(receiveRes));
	}
	else
	{
		PluginLog("[AC][EAC][REMOTE] AC RECEIVED MESSAGE FROM PEER: %u bytes (User %u)", dataLen, senderGoUserID);
	}
}

// Drains the anti-cheat channel, reassembling fragmented messages and feeding
// each completed one into the anti-cheat interface.
static void ProcessIncomingACMessages()
{
	uint8_t buffer[EOS_P2P_MAX_PACKET_SIZE] = {};

	for (;;)
	{
		uint32_t bytesWritten = 0;
		EOS_ProductUserId sender = nullptr;

		if (!ReceivePacketCore("Anticheat", (uint8_t)ENetworkChannels::AnticheatSecure, buffer, (uint32_t)sizeof(buffer), bytesWritten, sender))
		{
			return;
		}

		if (bytesWritten < sizeof(ACFragmentHeader))
		{
			PluginLog("[EAC] AC transport: runt packet of %u bytes, ignoring", bytesWritten);
			continue;
		}

		ACFragmentHeader header = {};
		memcpy(&header, buffer, sizeof(header));

		if (header.Magic != AC_FRAGMENT_MAGIC)
		{
			PluginLog("[EAC] AC transport: packet with bad magic, ignoring");
			continue;
		}

		std::string senderID;
		uint32_t senderGoUserID = 0;

		// Only accept anti-cheat traffic from players the game registered.
		if (!ProductUserIdToString(sender, senderID) || !TryGetGoUserID(senderID, senderGoUserID))
		{
			PluginLog("[EAC] AC transport: message from unregistered peer, ignoring");
			continue;
		}

		const uint32_t payloadBytes = bytesWritten - (uint32_t)sizeof(header);

		if (header.FragmentCount == 0 || header.FragmentIndex >= header.FragmentCount ||
			header.TotalBytes == 0 || header.TotalBytes > AC_MESSAGE_MAX_BYTES)
		{
			PluginLog("[EAC] AC transport: malformed fragment header from %s, ignoring", senderID.c_str());
			g_ACReassembly.erase(senderID);
			continue;
		}

		// Single-fragment messages are the common case; deliver without
		// touching the reassembly table.
		if (header.FragmentCount == 1)
		{
			g_ACReassembly.erase(senderID);

			if (payloadBytes != header.TotalBytes)
			{
				PluginLog("[EAC] AC transport: fragment from %s is %u bytes, expected %u - ignoring",
					senderID.c_str(), payloadBytes, header.TotalBytes);
				continue;
			}

			DeliverACMessage(senderGoUserID, buffer + sizeof(header), payloadBytes);
			continue;
		}

		ACReassemblyBuffer& reassembly = g_ACReassembly[senderID];

		if (header.FragmentIndex == 0)
		{
			reassembly = ACReassemblyBuffer();
			reassembly.TotalBytes = header.TotalBytes;
			reassembly.FragmentCount = header.FragmentCount;
			reassembly.Data.reserve(header.TotalBytes);
		}
		else if (header.FragmentIndex != reassembly.NextFragmentIndex ||
			header.TotalBytes != reassembly.TotalBytes ||
			header.FragmentCount != reassembly.FragmentCount)
		{
			// The channel is reliable-ordered, so this means the peer restarted
			// mid-message or is sending garbage. Drop the partial message
			// rather than splicing unrelated bytes together.
			PluginLog("[EAC] AC transport: out-of-sequence fragment %u from %s, dropping partial message",
				(unsigned)header.FragmentIndex, senderID.c_str());
			g_ACReassembly.erase(senderID);
			continue;
		}

		if (reassembly.Data.size() + payloadBytes > reassembly.TotalBytes)
		{
			PluginLog("[EAC] AC transport: fragments from %s overflow the declared size, dropping", senderID.c_str());
			g_ACReassembly.erase(senderID);
			continue;
		}

		reassembly.Data.insert(reassembly.Data.end(), buffer + sizeof(header), buffer + bytesWritten);
		reassembly.NextFragmentIndex = (uint16_t)(header.FragmentIndex + 1);

		if (reassembly.NextFragmentIndex == reassembly.FragmentCount)
		{
			if (reassembly.Data.size() == reassembly.TotalBytes)
			{
				DeliverACMessage(senderGoUserID, reassembly.Data.data(), reassembly.TotalBytes);
			}
			else
			{
				PluginLog("[EAC] AC transport: reassembled %zu bytes from %s, expected %u - dropping",
					reassembly.Data.size(), senderID.c_str(), reassembly.TotalBytes);
			}

			g_ACReassembly.erase(senderID);
		}
	}
}

void HookupEvents()
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	
	if (g_bEventsHooked)
	{
		return;
	}

	EOS_HP2P P2PHandle = GetP2PHandle();

	// cleanup networking notifications
	RemoveP2PNotifications(P2PHandle);


	// Clean up any existing notification IDs before registering new ones (prevents memory leak on re-hook)
	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();
	if (acHandle != nullptr)
	{
		if (g_NotifyClientIntegrityViolatedId != 0)
		{
			EOS_AntiCheatClient_RemoveNotifyClientIntegrityViolated(acHandle, g_NotifyClientIntegrityViolatedId);
			g_NotifyClientIntegrityViolatedId = 0;
		}
		if (g_NotifyMessageToPeerId != 0)
		{
			EOS_AntiCheatClient_RemoveNotifyMessageToPeer(acHandle, g_NotifyMessageToPeerId);
			g_NotifyMessageToPeerId = 0;
		}
		if (g_NotifyPeerAuthStatusChangedId != 0)
		{
			EOS_AntiCheatClient_RemoveNotifyPeerAuthStatusChanged(acHandle, g_NotifyPeerAuthStatusChangedId);
			g_NotifyPeerAuthStatusChangedId = 0;
		}
		if (g_NotifyPeerActionRequiredId != 0)
		{
			EOS_AntiCheatClient_RemoveNotifyPeerActionRequired(acHandle, g_NotifyPeerActionRequiredId);
			g_NotifyPeerActionRequiredId = 0;
		}
	}

	g_bEventsHooked = true;

	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE NULL - Cannot hook events");
	}
	else
	{
		EOS_AntiCheatClient_AddNotifyClientIntegrityViolatedOptions opts = {};
		opts.ApiVersion = EOS_ANTICHEATCLIENT_ADDNOTIFYCLIENTINTEGRITYVIOLATED_API_LATEST;
		g_NotifyClientIntegrityViolatedId = EOS_AntiCheatClient_AddNotifyClientIntegrityViolated(acHandle, &opts, nullptr, [](const EOS_AntiCheatClient_OnClientIntegrityViolatedCallbackInfo* Data)
			{
				if (Data == nullptr)
				{
					return;
				}

				const char* violationMsg = Data->ViolationMessage ? Data->ViolationMessage : "(null)";
				PluginLog("[EAC] AC VIOLATION: %s (%d)", violationMsg, Data->ViolationType);

				ACIntegrityViolationCallbackFunc callback = nullptr;
				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
					callback = g_fnAnticheatIntegrityViolationOccurredCallback;
				}
				// Lock released before calling callback

				if (callback != nullptr)
				{
					callback(violationMsg, (int)Data->ViolationType);
				}
			});

		EOS_AntiCheatClient_AddNotifyMessageToPeerOptions AddNotifyMessageToPeerOpts = {};
		AddNotifyMessageToPeerOpts.ApiVersion = EOS_ANTICHEATCLIENT_ADDNOTIFYMESSAGETOPEER_API_LATEST;
		g_NotifyMessageToPeerId = EOS_AntiCheatClient_AddNotifyMessageToPeer(acHandle, &AddNotifyMessageToPeerOpts, nullptr, [](const EOS_AntiCheatCommon_OnMessageToClientCallbackInfo* Data)
			{
				if (Data == nullptr)
				{
					return;
				}

				uint32_t targetUserID = (uint32_t)Data->ClientHandle;
				EOS_HAntiCheatClient acHandle = nullptr;
				SendMessageViaTransportFunc sendMessageCallback = nullptr;
				uint32_t localUserID = 0;

				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

					acHandle = GetAntiCheatHandle();

					if (acHandle == nullptr)
					{
						return;
					}

					// NOTE: A null ClientHandle is EOS_ANTICHEATCLIENT_PEER_SELF, which
					// is a valid destination - only the payload may not be null.
					if (Data->MessageData == nullptr || Data->MessageDataSizeBytes == 0)
					{
						return;
					}

					sendMessageCallback = g_fnSendMessageViaTransport;
					localUserID = g_goUserID;
				}
				// Lock released before processing

				// was it ourselves? just process immediately.
				// localUserID is only meaningful once the local player has been
				// registered; until then 0 means "unknown", not "peer 0".
				const bool bIsSelf = (Data->ClientHandle == EOS_ANTICHEATCLIENT_PEER_SELF)
					|| (localUserID != 0 && targetUserID == localUserID);

				if (bIsSelf)
				{
					EOS_AntiCheatClient_ReceiveMessageFromPeerOptions receiveOpts = {};
					receiveOpts.ApiVersion = EOS_ANTICHEATCLIENT_RECEIVEMESSAGEFROMPEER_API_LATEST;
					receiveOpts.PeerHandle = Data->ClientHandle;
					receiveOpts.Data = Data->MessageData;
					receiveOpts.DataLengthBytes = Data->MessageDataSizeBytes;

					EOS_EResult receiveRes = EOS_AntiCheatClient_ReceiveMessageFromPeer(acHandle, &receiveOpts);
					if (receiveRes != EOS_EResult::EOS_Success)
					{
						PluginLog("[EAC][LOCAL] MIDDLEWARE ERROR, EOS_AntiCheatClient_ReceiveMessageFromPeer: %s!", EOS_EResult_ToString(receiveRes));
					}
					else
					{
						PluginLog("[EAC][LOCAL] AC SEND MESSAGE TO PEER: %u bytes (User %u)", Data->MessageDataSizeBytes, targetUserID);
					}
				}
				else // send via transport
				{
					PluginLog("[EAC][REMOTE] AC SEND MESSAGE TO PEER: %u bytes (User %u)", Data->MessageDataSizeBytes, targetUserID);

					// Exactly one transport carries anti-cheat traffic. When the
					// plugin owns the connection it sends the message itself and
					// the game never sees it; otherwise the game's transport is
					// the only path. Never both, or the peer would receive every
					// message twice and fail to validate.
					if (DoesACPluginProvideSecureGameTransport())
					{
						if (!SendACMessageViaPluginTransport(targetUserID, Data->MessageData, Data->MessageDataSizeBytes))
						{
							PluginLog("[EAC][REMOTE] ERROR: AC transport could not send to user %u", targetUserID);
						}
					}
					else if (sendMessageCallback != nullptr)
					{
						sendMessageCallback(targetUserID, Data->MessageData, Data->MessageDataSizeBytes);
					}
					else
					{
						PluginLog("[EAC][REMOTE] ERROR: Send message callback is null!");
					}
				}
			});

		EOS_AntiCheatClient_AddNotifyPeerAuthStatusChangedOptions authChangedOpts = {};
		authChangedOpts.ApiVersion = EOS_ANTICHEATCLIENT_ADDNOTIFYPEERAUTHSTATUSCHANGED_API_LATEST;
		g_NotifyPeerAuthStatusChangedId = EOS_AntiCheatClient_AddNotifyPeerAuthStatusChanged(acHandle, &authChangedOpts, nullptr, [](const EOS_AntiCheatCommon_OnClientAuthStatusChangedCallbackInfo* Data)
			{
				if (Data == nullptr)
				{
					return;
				}

				uint32_t userID = (uint32_t)Data->ClientHandle;
				PluginLog("[EAC] AC PEER AUTH STATUS CHANGED: %d (User %u)", Data->ClientAuthStatus, userID);
			});

		EOS_AntiCheatClient_AddNotifyPeerActionRequiredOptions actionRequiredOpts = {};
		actionRequiredOpts.ApiVersion = EOS_ANTICHEATCLIENT_ADDNOTIFYPEERACTIONREQUIRED_API_LATEST;
		g_NotifyPeerActionRequiredId = EOS_AntiCheatClient_AddNotifyPeerActionRequired(acHandle, &actionRequiredOpts, nullptr, [](const EOS_AntiCheatCommon_OnClientActionRequiredCallbackInfo* Data)
			{
				if (Data == nullptr)
				{
					return;
				}

				const char* reasonStr = Data->ActionReasonDetailsString ? Data->ActionReasonDetailsString : "(null)";
				PluginLog("[EAC] AC PEER ACTION REQUIRED: %s (%d - %d)", reasonStr, Data->ClientAction, Data->ActionReasonCode);

				ACPlayerActionRequiredCallbackFunc callback = nullptr;

				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
					callback = g_fnAnticheatActionCallback;
				}
				// Lock released before calling callback

				if (callback != nullptr)
				{
					uint32_t userID = (uint32_t)Data->ClientHandle;
					if (Data->ClientHandle == EOS_ANTICHEATCLIENT_PEER_SELF)
					{
						PluginLog("[EAC] AC PEER ACTION REQUIRED: is self (%u)", userID);
					}
					else
					{
						PluginLog("[EAC] AC PEER ACTION REQUIRED: is remote (%u)", userID);
					}
					callback(userID, reasonStr, (int)(EAnticheatActionType)Data->ClientAction, (int)(EAnticheatActionReason)Data->ActionReasonCode);
				}
			});
	}
	


	// hook up notifications for networking
	if (P2PHandle == nullptr)
	{
		PluginLog("[EAC] P2P HANDLE NULL - Cannot hook connection state events");
		return;
	}

	// Set the options for subscribing to connection request notifications.
	EOS_P2P_AddNotifyPeerConnectionRequestOptions ConnectionRequestNotificationOptions = {};
	ConnectionRequestNotificationOptions.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONREQUEST_API_LATEST;
	ConnectionRequestNotificationOptions.LocalUserId = g_EOSUserID;
	ConnectionRequestNotificationOptions.SocketId = &g_SocketId;
	g_ConnectionRequestNotificationId = EOS_P2P_AddNotifyPeerConnectionRequest(P2PHandle, &ConnectionRequestNotificationOptions, nullptr,
		[](const EOS_P2P_OnIncomingConnectionRequestInfo* Data)
		{
			if (Data == nullptr)
			{
				return;
			}

			std::string middlewareUserID;
			if (!ProductUserIdToString(Data->RemoteUserId, middlewareUserID))
			{
				PluginLog("[EAC] Incoming connection request from an unresolvable product user ID - rejecting");
				return;
			}

			// Only accept connections from players the game has registered for
			// this session; anyone else who knows our product user ID must not
			// be able to open a connection to us.
			uint32_t goUserID = 0;
			if (!TryGetGoUserID(middlewareUserID, goUserID))
			{
				// The game registers peers from a service message that can land
				// after the peer has already started signalling. EOS never
				// re-delivers a connection request, so park it and let
				// RegisterPlayer() accept it once the peer is vouched for.
				{
					std::lock_guard<std::mutex> userMapLock(g_UserMapMutex);
					g_PendingConnectionRequests.insert(middlewareUserID);
				}

				PluginLog("[EAC] Deferring connection request from not-yet-registered peer %s",
					middlewareUserID.c_str());
				return;
			}

			if (!AcceptP2PConnection("OnIncomingConnectionRequest", Data->RemoteUserId, middlewareUserID.c_str()))
			{
				return;
			}

			ReportConnectionState(Data->RemoteUserId, "Connecting to", EConnectionState::CONNECTING_DIRECT, EOS_ENetworkConnectionType::EOS_NCT_NoConnection);
		});

	// Set the options for subscribing to connection established notifications.
	EOS_P2P_AddNotifyPeerConnectionEstablishedOptions ConnectionEstablishedNotificationOptions = {};
	ConnectionEstablishedNotificationOptions.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONESTABLISHED_API_LATEST;
	ConnectionEstablishedNotificationOptions.LocalUserId = g_EOSUserID;
	ConnectionEstablishedNotificationOptions.SocketId = &g_SocketId;
	g_ConnectionEstablishedNotificationId = EOS_P2P_AddNotifyPeerConnectionEstablished(P2PHandle, &ConnectionEstablishedNotificationOptions, nullptr,
		[](const EOS_P2P_OnPeerConnectionEstablishedInfo* Data)
		{
			if (Data == nullptr)
			{
				return;
			}

			ReportConnectionState(Data->RemoteUserId, "Connected to", EConnectionState::CONNECTED_DIRECT, Data->NetworkType);
		});

	EOS_P2P_AddNotifyPeerConnectionInterruptedOptions ConnectionInterruptedNotificationOptions = {};
	ConnectionInterruptedNotificationOptions.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONINTERRUPTED_API_LATEST;
	ConnectionInterruptedNotificationOptions.LocalUserId = g_EOSUserID;
	ConnectionInterruptedNotificationOptions.SocketId = &g_SocketId;
	g_ConnectionInterruptedNotificationId = EOS_P2P_AddNotifyPeerConnectionInterrupted(P2PHandle, &ConnectionInterruptedNotificationOptions, nullptr,
		[](const EOS_P2P_OnPeerConnectionInterruptedInfo* Data)
		{
			if (Data == nullptr)
			{
				return;
			}

			ReportConnectionState(Data->RemoteUserId, "Disconnected from", EConnectionState::CONNECTION_DISCONNECTED, EOS_ENetworkConnectionType::EOS_NCT_NoConnection);
		});

	// Set the options for subscribing to connection closed notifications.
	EOS_P2P_AddNotifyPeerConnectionClosedOptions ConnectionClosedNotificationOptions = {};
	ConnectionClosedNotificationOptions.ApiVersion = EOS_P2P_ADDNOTIFYPEERCONNECTIONCLOSED_API_LATEST;
	ConnectionClosedNotificationOptions.LocalUserId = g_EOSUserID;
	ConnectionClosedNotificationOptions.SocketId = &g_SocketId;
	g_ConnectionClosedNotificationId = EOS_P2P_AddNotifyPeerConnectionClosed(P2PHandle, &ConnectionClosedNotificationOptions, nullptr,
		[](const EOS_P2P_OnRemoteConnectionClosedInfo* Data)
		{
			if (Data == nullptr)
			{
				return;
			}

			ReportConnectionState(Data->RemoteUserId, "Remote Disconnection by", EConnectionState::NOT_CONNECTED, EOS_ENetworkConnectionType::EOS_NCT_NoConnection);
		});

	// NOTE: these are unsubscribed in EndSession() and in Shutdown() (before the
	// platform that owns them is released) via RemoveP2PNotifications().
	// end networking notifications
}

void BeginSession()
{
	PluginLog("[EAC] BEGIN SESSION");
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();

	// Purge anything left over from a previous session before the game starts
	// signalling. EOS keeps P2P connections alive until they are explicitly
	// closed, and the connection notifications are edge-triggered: a surviving
	// connection raises no connection request or established event, so the game
	// would never observe that peer connecting. This is safe here because no
	// peer has been signalled yet for this session.
	DisconnectAll();

	{
		std::lock_guard<std::mutex> latencyLock(g_LatencyMutex);
		g_LatencyMs.clear();
	}

	{
		std::lock_guard<std::mutex> connectionTypeLock(g_ConnectionTypeMutex);
		g_ConnectionType.clear();
	}

	g_ACReassembly.clear();

	HookupEvents();

	// Refresh the NAT type for this session; the result is logged and shown in
	// lobby chat so players can see why their connections may be relayed.
	QueryLocalNATType();

	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE IS NULL");
	}
	else
	{
		EOS_AntiCheatClient_BeginSessionOptions beginSessionOpts = {};
		beginSessionOpts.ApiVersion = EOS_ANTICHEATCLIENT_BEGINSESSION_API_LATEST;
		beginSessionOpts.LocalUserId = g_EOSUserID;
		beginSessionOpts.Mode = EOS_EAntiCheatClientMode::EOS_ACCM_PeerToPeer;
		EOS_EResult result = EOS_AntiCheatClient_BeginSession(acHandle, &beginSessionOpts);
		if (result != EOS_EResult::EOS_Success)
		{
			PluginLog("[EAC] MIDDLEWARE ERROR, BEGIN SESSION: %s!", EOS_EResult_ToString(result));
		}
		else
		{
			PluginLog("[EAC] BEGIN SESSION SUCCEEDED");
		}
	}
}

bool DeregisterPlayer(const char* szMiddlewareUserID, uint32_t goUserID)
{
	if (szMiddlewareUserID == nullptr || szMiddlewareUserID[0] == '\0')
	{
		PluginLog("[EAC] DeregisterPlayer: Invalid middleware user ID (null/empty) for game user %u", goUserID);
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();

	std::string strUserKey(szMiddlewareUserID);
	{
		std::lock_guard<std::mutex> userMapLock(g_UserMapMutex);
		g_UserMap.erase(strUserKey);
		g_PendingConnectionRequests.erase(strUserKey);
	}

	g_ACReassembly.erase(strUserKey);

	{
		std::lock_guard<std::mutex> latencyLock(g_LatencyMutex);
		g_LatencyMs.erase(strUserKey);
	}

	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE NULL DeregisterPlayer");
		return false;
	}

	EOS_AntiCheatClient_UnregisterPeerOptions opts = {};
	opts.ApiVersion = EOS_ANTICHEATCLIENT_UNREGISTERPEER_API_LATEST;
	opts.PeerHandle = (void*)goUserID;
	EOS_EResult res = EOS_AntiCheatClient_UnregisterPeer(acHandle, &opts);

	PluginLog("[EAC] DeregisterPlayer: Deregistering remote player %s - %u!", szMiddlewareUserID, goUserID);

	if (res != EOS_EResult::EOS_Success)
	{
		PluginLog("[EAC] DeregisterPlayer ERROR: %s!", EOS_EResult_ToString(res));
		return false;
	}

	return true;
}

bool RegisterPlayer(const char* szMiddlewareUserID, uint32_t goUserID)
{
	if (szMiddlewareUserID == nullptr || szMiddlewareUserID[0] == '\0')
	{
		PluginLog("[EAC] RegisterPlayer: Invalid middleware user ID (null/empty) for game user %u", goUserID);
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();

	bool bHadPendingRequest = false;
	{
		std::lock_guard<std::mutex> userMapLock(g_UserMapMutex);
		g_UserMap[std::string(szMiddlewareUserID)] = goUserID;
		bHadPendingRequest = g_PendingConnectionRequests.erase(std::string(szMiddlewareUserID)) > 0;
	}

	// This peer tried to connect before the game vouched for it; complete the
	// handshake now rather than waiting for a request EOS will never resend.
	if (bHadPendingRequest)
	{
		PluginLog("[EAC] RegisterPlayer: accepting deferred connection request from %s", szMiddlewareUserID);

		EOS_ProductUserId pendingPeer = EOS_ProductUserId_FromString(szMiddlewareUserID);

		if (AcceptP2PConnection("RegisterPlayer", pendingPeer, szMiddlewareUserID))
		{
			ReportConnectionState(pendingPeer, "Connecting to", EConnectionState::CONNECTING_DIRECT,
				EOS_ENetworkConnectionType::EOS_NCT_NoConnection);
		}
	}

	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE NULL RegisterPlayer");
		return false;
	}

	EOS_ProductUserId peerUserId = EOS_ProductUserId_FromString(szMiddlewareUserID);

	if (peerUserId == nullptr)
	{
		PluginLog("[EAC] RegisterPlayer ERROR: Malformed middleware user ID '%s'", szMiddlewareUserID);
		return false;
	}

	// NOTE: both sides must be non-null before comparing - otherwise an
	// unparseable ID would compare equal to a not-yet-logged-in local user and
	// the peer would silently never be registered.
	if (g_EOSUserID != nullptr && peerUserId == g_EOSUserID)
	{
		PluginLog("[EAC] RegisterPlayer: Registering local player %s - %u!", szMiddlewareUserID, goUserID);
		g_goUserID = goUserID;
		return true;
	}

	EOS_AntiCheatClient_RegisterPeerOptions opts = {};
	opts.ApiVersion = EOS_ANTICHEATCLIENT_REGISTERPEER_API_LATEST;
	opts.PeerHandle = (void*)goUserID;
	opts.ClientType = EOS_EAntiCheatCommonClientType::EOS_ACCCT_ProtectedClient;
	opts.ClientPlatform = EOS_EAntiCheatCommonClientPlatform::EOS_ACCCP_Windows;
	opts.AuthenticationTimeout = EOS_ANTICHEATCLIENT_REGISTERPEER_MAX_AUTHENTICATIONTIMEOUT;
	opts.AccountId_DEPRECATED = nullptr;
	opts.IpAddress = nullptr;
	opts.PeerProductUserId = peerUserId;
	EOS_EResult res = EOS_AntiCheatClient_RegisterPeer(acHandle, &opts);

	PluginLog("[EAC] RegisterPlayer: Registering remote player %s - %u!", szMiddlewareUserID, goUserID);
	
	if (res != EOS_EResult::EOS_Success)
	{
		PluginLog("[EAC] RegisterPlayer ERROR: %s!", EOS_EResult_ToString(res));
		return false;
	}

	return true;
}

void EndSession()
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();

	g_bEventsHooked = false;

	// Tear down every P2P connection opened for this session.
	//
	// The game never calls DisconnectAll() itself, and EOS keeps connections
	// alive until they are explicitly closed. A connection that survives into the
	// next session is fatal: the connection notifications are edge-triggered, so
	// the next match raises no connection request or established event and the
	// game never observes the peer connecting.
	DisconnectAll();

	// Latency samples belong to the session that is ending; keeping them would
	// report stale round trip times for the first seconds of the next match.
	{
		std::lock_guard<std::mutex> latencyLock(g_LatencyMutex);
		g_LatencyMs.clear();
	}

	// Deferred connection requests are scoped to the session that raised them.
	{
		std::lock_guard<std::mutex> userMapLock(g_UserMapMutex);
		g_PendingConnectionRequests.clear();
	}

	// Cached connection types describe connections we just closed.
	{
		std::lock_guard<std::mutex> connectionTypeLock(g_ConnectionTypeMutex);
		g_ConnectionType.clear();
	}

	// Partially received anti-cheat messages cannot be completed once the
	// connections carrying them are gone.
	g_ACReassembly.clear();

	g_LastPingSendTime = std::chrono::steady_clock::time_point{};

	// remove networking events
	RemoveP2PNotifications(GetP2PHandle());

	// Remove all registered notification callbacks before ending the session
	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE NULL 1");
	}
	else
	{
		if (g_NotifyClientIntegrityViolatedId != 0)
		{
			EOS_AntiCheatClient_RemoveNotifyClientIntegrityViolated(acHandle, g_NotifyClientIntegrityViolatedId);
			g_NotifyClientIntegrityViolatedId = 0;
			PluginLog("[EAC] Removed ClientIntegrityViolated callback");
		}

		if (g_NotifyMessageToPeerId != 0)
		{
			EOS_AntiCheatClient_RemoveNotifyMessageToPeer(acHandle, g_NotifyMessageToPeerId);
			g_NotifyMessageToPeerId = 0;
			PluginLog("[EAC] Removed MessageToPeer callback");
		}

		if (g_NotifyPeerAuthStatusChangedId != 0)
		{
			EOS_AntiCheatClient_RemoveNotifyPeerAuthStatusChanged(acHandle, g_NotifyPeerAuthStatusChangedId);
			g_NotifyPeerAuthStatusChangedId = 0;
			PluginLog("[EAC] Removed PeerAuthStatusChanged callback");
		}

		if (g_NotifyPeerActionRequiredId != 0)
		{
			EOS_AntiCheatClient_RemoveNotifyPeerActionRequired(acHandle, g_NotifyPeerActionRequiredId);
			g_NotifyPeerActionRequiredId = 0;
			PluginLog("[EAC] Removed PeerActionRequired callback");
		}

		EOS_AntiCheatClient_EndSessionOptions endSessionOpts = {};
		endSessionOpts.ApiVersion = EOS_ANTICHEATCLIENT_ENDSESSION_API_LATEST;
		EOS_EResult result = EOS_AntiCheatClient_EndSession(acHandle, &endSessionOpts);
		if (result != EOS_EResult::EOS_Success)
		{
			PluginLog("[EAC] MIDDLEWARE ERROR, END SESSION: %s!", EOS_EResult_ToString(result));
		}
		else
		{
			PluginLog("[EAC] End session succeeded: %s!", EOS_EResult_ToString(result));
		}
	}
}


void RefreshToken(const char* szGameToken, LoginCallback cb)
{
	// Validate token pointer before logging to prevent format string vulnerabilities
	if (szGameToken != nullptr)
	{
		PluginLog("[EAC] Refresh Token: %s", szGameToken);
	}
	else
	{
		PluginLog("[EAC] Refresh Token: (null token)");
	}
	
	// refresh shouldnt need to create accounts etc, so should be fine to do less things
	{
		std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
		g_LoginCallback = cb;
	}

	if (szGameToken != nullptr)
	{
		PluginLog("[EAC] Connect EOS: %s", szGameToken);
	}
	else
	{
		PluginLog("[EAC] Connect EOS: (null token)");
	}

	EOS_HPlatform platformHandle = nullptr;
	
	{
		std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
		
		if (g_EOSPlatformHandle != nullptr)
		{
			platformHandle = g_EOSPlatformHandle;
		}
		else
		{
			g_LoginCallback = nullptr;
		}
	}
	// Lock released before invoking host callbacks or entering the SDK

	if (platformHandle == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Platform not initialized!");
		if (cb != nullptr)
		{
			cb(false);
		}
		return;
	}

	EOS_HConnect ConnectHandle = EOS_Platform_GetConnectInterface(platformHandle);
	
	if (ConnectHandle == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Connect handle is null!");

		{
			std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
			g_LoginCallback = nullptr;
		}

		if (cb != nullptr)
		{
			cb(false);
		}
		return;
	}

	EOS_Connect_Credentials Credentials = {};
	Credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
	Credentials.Token = szGameToken;
	Credentials.Type = EOS_EExternalCredentialType::EOS_ECT_OPENID_ACCESS_TOKEN;

	EOS_Connect_LoginOptions Options = {};
	Options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
	Options.Credentials = &Credentials;

	EOS_Connect_UserLoginInfo userLoginInfo = {};
	userLoginInfo.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
	userLoginInfo.DisplayName = nullptr; // not set for oauth, retrieved from token instead
	userLoginInfo.NsaIdToken = nullptr;
	Options.UserLoginInfo = &userLoginInfo;

	// TODO: Start a timeout
	EOS_Connect_Login(ConnectHandle, &Options, nullptr, [](const EOS_Connect_LoginCallbackInfo* Data)
		{
			if (Data == nullptr)
			{
				// Release the pending callback so a later refresh is not blocked.
				FireLoginCallback(false);
				return;
			}

			// TODO: Clear timeout
			if (Data->ResultCode == EOS_EResult::EOS_Success)
			{
				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
					g_EOSUserID = Data->LocalUserId;
				}

				char szBuffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = { 0 };
				int32_t outLen = sizeof(szBuffer);
				EOS_ProductUserId_ToString(Data->LocalUserId, szBuffer, &outLen);

				PluginLog("[EAC] Login Complete: %s", szBuffer);

				// NOTE: dont need to hook up events again

				FireLoginCallback(true);
			}
			else
			{
				PluginLog("[EAC] Token Refresh Failed: %s", EOS_EResult_ToString(Data->ResultCode));

				FireLoginCallback(false);
			}
		});
}

void Login(const char* szGameToken, LoginCallback cb)
{
	// Validate token pointer before logging to prevent format string vulnerabilities
	if (szGameToken != nullptr)
	{
		PluginLog("[EAC] Connect EOS: %s", szGameToken);
	}
	else
	{
		PluginLog("[EAC] Connect EOS: (null token)");
	}

	EOS_HPlatform platformHandle = nullptr;

	{
		std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
		g_LoginCallback = cb;

		if (g_EOSPlatformHandle == nullptr)
		{
			g_LoginCallback = nullptr;
		}
		else
		{
			platformHandle = g_EOSPlatformHandle;
		}
	}
	// Lock released before invoking host callbacks or entering the SDK

	if (platformHandle == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Platform not initialized!");
		if (cb != nullptr)
		{
			cb(false);
		}
		return;
	}

	EOS_HConnect ConnectHandle = EOS_Platform_GetConnectInterface(platformHandle);
	
	if (ConnectHandle == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Connect handle is null!");

		{
			std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
			g_LoginCallback = nullptr;
		}

		if (cb != nullptr)
		{
			cb(false);
		}
		return;
	}

	EOS_Connect_Credentials Credentials = {};
	Credentials.ApiVersion = EOS_CONNECT_CREDENTIALS_API_LATEST;
	Credentials.Token = szGameToken;
	Credentials.Type = EOS_EExternalCredentialType::EOS_ECT_OPENID_ACCESS_TOKEN;

	EOS_Connect_LoginOptions Options = {};
	Options.ApiVersion = EOS_CONNECT_LOGIN_API_LATEST;
	Options.Credentials = &Credentials;

	EOS_Connect_UserLoginInfo userLoginInfo = {};
	userLoginInfo.ApiVersion = EOS_CONNECT_USERLOGININFO_API_LATEST;
	userLoginInfo.DisplayName = nullptr; // not set for oauth, retrieved from token instead
	userLoginInfo.NsaIdToken = nullptr;
	Options.UserLoginInfo = &userLoginInfo;

	// TODO: Start a timeout
	EOS_Connect_Login(ConnectHandle, &Options, nullptr, [](const EOS_Connect_LoginCallbackInfo* Data)
		{
			if (Data == nullptr)
			{
				// The SDK gave us nothing to act on; release the pending callback
				// so a later login attempt is not blocked by stale state.
				FireLoginCallback(false);
				return;
			}

			// TODO: Clear timeout

			if (Data->ResultCode == EOS_EResult::EOS_Success)
			{
				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
					g_EOSUserID = Data->LocalUserId;
				}

				char szBuffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = { 0 };
				int32_t outLen = sizeof(szBuffer);
				EOS_ProductUserId_ToString(Data->LocalUserId, szBuffer, &outLen);

				PluginLog("[EAC] Login Complete: %s", szBuffer);

				HookupEvents();

				// Warm the NAT type up front so it is already known by the time
				// the player reaches a lobby.
				QueryLocalNATType();

				FireLoginCallback(true);
			}
			else if (Data->ResultCode == EOS_EResult::EOS_InvalidUser)
			{
				EOS_HConnect ConnectHandle = nullptr;
				EOS_ContinuanceToken ContinuanceToken = Data->ContinuanceToken;

				if (ContinuanceToken == nullptr)
				{
					// Without a continuance token there is nothing to create the
					// user from - calling CreateUser would just fail internally.
					PluginLog("[EAC] ERROR: Login returned InvalidUser without a continuance token!");
					FireLoginCallback(false);
					return;
				}

				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

					if (g_EOSPlatformHandle != nullptr)
					{
						ConnectHandle = EOS_Platform_GetConnectInterface(g_EOSPlatformHandle);
					}
				}
				// Lock released here before async operation

				if (ConnectHandle == nullptr)
				{
					PluginLog("[EAC] ERROR: Connect handle is null in login callback!");
					FireLoginCallback(false);
					return;
				}

				EOS_Connect_CreateUserOptions Options = {};
				Options.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;
				Options.ContinuanceToken = ContinuanceToken;

				// NOTE: We're not deleting the received context because we're passing it down to another SDK call
				EOS_Connect_CreateUser(ConnectHandle, &Options, nullptr,
					[](const EOS_Connect_CreateUserCallbackInfo* Data)
					{
						if (Data == nullptr)
						{
							FireLoginCallback(false);
							return;
						}

						if (Data->ResultCode == EOS_EResult::EOS_Success)
						{
							char szBuffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = { 0 };
							int32_t outLen = sizeof(szBuffer);
							EOS_ProductUserId_ToString(Data->LocalUserId, szBuffer, &outLen);

							PluginLog("[EAC] Account Link Complete: %s", szBuffer);

							{
								std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
								g_EOSUserID = Data->LocalUserId;
							}

							HookupEvents();
						}
						else
						{
							PluginLog("[EAC] Account Link Failed: %s", EOS_EResult_ToString(Data->ResultCode));
						}

						FireLoginCallback(Data->ResultCode == EOS_EResult::EOS_Success);
					}
				);
			}
			else
			{
				PluginLog("[EAC] Login Failed: %s", EOS_EResult_ToString(Data->ResultCode));

				FireLoginCallback(false);
			}
		});
}