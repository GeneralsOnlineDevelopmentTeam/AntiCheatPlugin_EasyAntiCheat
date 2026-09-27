#include "plugin_eac.h"

// ------------------------------------------------------------
// Global definitions (declared extern in plugin_eac.h)
// ------------------------------------------------------------
std::atomic<LoggingFunc> g_fnLoggingFunc{ nullptr };
std::atomic<LoggingFunc> g_fnLobbyChatOutput{ nullptr };
EOS_HPlatform g_EOSPlatformHandle = nullptr;
std::recursive_mutex g_StateMutex;

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

void Tick()
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	if (g_EOSPlatformHandle != nullptr)
	{
		EOS_Platform_Tick(g_EOSPlatformHandle);
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

int Initialize(ConnectionStateChangedCallbackFunc connectionStateChangedCB)
{
    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);

    // The game hands this in on every Initialize() call, keep the latest one even
    // if we early out below so a re-init can't leave a stale pointer behind.
    g_fnConnectionStateChanged = connectionStateChangedCB;

    // Check if already initialized
    if (g_EOSPlatformHandle != nullptr)
    {
        PluginLog("[EAC] Already initialized - skipping re-initialization");
        return EInitializeResult_Success;
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
        PlatformOptions.EncryptionKey = "1111111111111111111111111111111111111111111111111111111"; // NOTE: unused
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
// This plugin runs anti-cheat only, it does not own a game transport. The game
// still resolves all of these exports at load time and unloads the plugin if any
// is missing, so they are exported as well-defined no-ops. Because
// DoesACPluginProvideSecureGameTransport() reports false, the game routes both
// game and anti-cheat traffic over its own mesh/WebSocket transport and never
// calls the remaining functions.
// ------------------------------------------------------------
PLUGIN_API bool DoesACPluginProvideSecureGameTransport()
{
	return false;
}

PLUGIN_API void StartSignalling(const char* middlewareUserID, uint64_t goUserID)
{
	(void)middlewareUserID;
	(void)goUserID;
}

PLUGIN_API void SendPacket(const char* middlewareUserID, uint64_t targetGoUserID, void* data, int numBytes, ENetworkChannels channel, EPacketReliability reliability)
{
	(void)middlewareUserID;
	(void)targetGoUserID;
	(void)data;
	(void)numBytes;
	(void)channel;
	(void)reliability;
}

PLUGIN_API int GetNextRecvPacketSize(uint8_t channelToReceiveOn)
{
	(void)channelToReceiveOn;
	return 0;
}

PLUGIN_API bool RecvPacket(uint8_t** outData, uint8_t channelToReceiveOn)
{
	(void)channelToReceiveOn;

	if (outData != nullptr)
	{
		*outData = nullptr;
	}

	return false;
}

PLUGIN_API void FreePacket(void* packetData)
{
	// Nothing is ever handed out by RecvPacket(), so there is nothing to release.
	(void)packetData;
}

PLUGIN_API int GetConnectionLatencyForUser(const char* middlewareUserID, uint32_t goUserID)
{
	// No plugin-owned connections, the game measures latency on its own transport.
	(void)middlewareUserID;
	(void)goUserID;
	return 0;
}

PLUGIN_API void DisconnectPlayer(const char* middlewareUserID, uint64_t goUserID)
{
	(void)middlewareUserID;
	(void)goUserID;
}

PLUGIN_API void DisconnectAll()
{
}

void HookupEvents()
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	
	if (g_bEventsHooked)
	{
		return;
	}

	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();
	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE NULL - Cannot hook events");
		return;
	}

	// Clean up any existing notification IDs before registering new ones (prevents memory leak on re-hook)
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

	g_bEventsHooked = true;

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
				if (sendMessageCallback != nullptr)
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

void BeginSession()
{
	PluginLog("[EAC] BEGIN SESSION");
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();

	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE IS NULL");
		return;
	}

	HookupEvents();

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

bool DeregisterPlayer(const char* szMiddlewareUserID, uint32_t goUserID)
{
	if (szMiddlewareUserID == nullptr)
	{
		PluginLog("[EAC] DeregisterPlayer: Invalid middleware user ID (null)");
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();

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
	if (szMiddlewareUserID == nullptr)
	{
		PluginLog("[EAC] RegisterPlayer: Invalid middleware user ID (null)");
		return false;
	}

	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	EOS_HAntiCheatClient acHandle = GetAntiCheatHandle();

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

	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE NULL 1");
		return;
	}

	g_bEventsHooked = false;

	// Remove all registered notification callbacks before ending the session
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