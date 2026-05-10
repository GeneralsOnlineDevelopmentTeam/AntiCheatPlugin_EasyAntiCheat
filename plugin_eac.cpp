#include "plugin_eac.h"

// Global notification IDs for callback cleanup
EOS_NotificationId g_NotifyClientIntegrityViolatedId = 0;
EOS_NotificationId g_NotifyMessageToPeerId = 0;
EOS_NotificationId g_NotifyPeerAuthStatusChangedId = 0;
EOS_NotificationId g_NotifyPeerActionRequiredId = 0;

typedef void (*LoginCallback)(bool bSuccess);
LoginCallback g_LoginCallback = nullptr;

BOOL APIENTRY DllMain(HMODULE /*hModule*/, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
	switch (ul_reason_for_call)
	{
		case DLL_PROCESS_ATTACH:
		{
			break;
		}

		case DLL_PROCESS_DETACH:
		{
			// Clean up resources on DLL unload
			std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
			if (g_EOSPlatformHandle != nullptr)
			{
				// Clear callback pointers to prevent use-after-free
				g_fnLoggingFunc = nullptr;
				g_fnLobbyChatOutput = nullptr;
				g_fnAnticheatIntegrityViolationOccurredCallback = nullptr;
				g_fnAnticheatActionCallback = nullptr;
				g_fnSendMessageViaTransport = nullptr;
				g_LoginCallback = nullptr;
			}
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

	char buffer[8192];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buffer, 8192, fmt, args);
	buffer[8192 - 1] = 0;
	va_end(args);

	// Lock before accessing global callback to prevent data race
	{
		std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
		if (g_fnLoggingFunc != nullptr)
		{
			g_fnLoggingFunc(buffer);
		}
	}
}

void SetLoggingFunction(LoggingFunc cb)
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	g_fnLoggingFunc = cb;
}

void SetLobbyChatOutputFunction(LoggingFunc cb)
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	g_fnLobbyChatOutput = cb;
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
	
	if (g_EOSPlatformHandle == nullptr)
	{
		PluginLog("[AC][EAC][REMOTE] ERROR: Platform handle is null");
		return;
	}

	EOS_HAntiCheatClient acHandle = EOS_Platform_GetAntiCheatClientInterface(g_EOSPlatformHandle);
	
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

enum class ENetworkChannels
{
	Game,
	Anticheat,
	Ping,
	Pong,
	InitialHolepunch
};

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
	
	EOS_HConnect ConnectHandle = EOS_Platform_GetConnectInterface(g_EOSPlatformHandle);

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

	if (epicToken->JsonWebToken == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: JsonWebToken is null!");
		return false;
	}

	const char* msg = epicToken->JsonWebToken;
	size_t len = strlen(msg) + 1;
	
	// Validate token length doesn't exceed a reasonable size
	if (len > 8192)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Token too large!");
		return false;
	}
	
	if (bufferSize < len)
	{
		PluginLog("[EAC] MIDDLEWARE BAD SIZE!");
		return false;
	}

	memcpy(buffer, msg, len);
	return true;
}

int Initialize()
{
    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
    
    // Check if already initialized
    if (g_EOSPlatformHandle != nullptr)
    {
        PluginLog("[EAC] Already initialized - skipping re-initialization");
        return 0;
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

    // TODO: Decrease logging once confirmed stable
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
            //EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Info);
			EOS_EResult SetLogLevelResult = EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_VeryVerbose);
			if (SetLogLevelResult != EOS_EResult::EOS_Success)
			{
				PluginLog("[EAC] Set Logging Level Failed!");
			}
#else
            //EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_Error);
			EOS_EResult SetLogLevelResult = EOS_Logging_SetLogLevel(EOS_ELogCategory::EOS_LC_ALL_CATEGORIES, EOS_ELogLevel::EOS_LOG_VeryVerbose);
			if (SetLogLevelResult != EOS_EResult::EOS_Success)
			{
				PluginLog("[EAC] Set Logging Level Failed!");
			}
#endif
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

        PlatformOptions.ProductId = "TODO";
        PlatformOptions.SandboxId = "TODO";
        PlatformOptions.EncryptionKey = "1111111111111111111111111111111111111111111111111111111"; // NOTE: unused
        PlatformOptions.DeploymentId = "TODO";

        PlatformOptions.ClientCredentials.ClientId = "TODO";
        PlatformOptions.ClientCredentials.ClientSecret = "TODO";

        double timeout = 5000.0;
        PlatformOptions.TaskNetworkTimeoutSeconds = &timeout;

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
            return 1;
        }
        
        std::string ExePath = std::string(buffer).substr(0, pos);

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
			return 1;
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
            return 2;
        }

        g_EOSPlatformHandle = EOS_Platform_Create(&PlatformOptions);
        
        if (g_EOSPlatformHandle == nullptr)
        {
            PluginLog("FATAL ERROR: EOS_Platform_Create failed - returned null handle");
            return 4;
        }
        // END PLATFORM OPTIONS

        return 0;
    }
    else
    {
        PluginLog("[EAC] INIT FAILED: %s", EOS_EResult_ToString(InitResult));
        return 3;
    }
}

PLUGIN_API void Shutdown()
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	if (g_EOSPlatformHandle != nullptr)
	{
		EOS_Platform_Release(g_EOSPlatformHandle);
		g_EOSPlatformHandle = nullptr;
	}

	EOS_Shutdown();
	
	// Reset global state
	g_EOSUserID = nullptr;
	g_goUserID = 0;
	g_fnLoggingFunc = nullptr;
	g_fnLobbyChatOutput = nullptr;
	g_fnAnticheatIntegrityViolationOccurredCallback = nullptr;
	g_fnAnticheatActionCallback = nullptr;
	g_fnSendMessageViaTransport = nullptr;
	g_LoginCallback = nullptr;
	g_bEventsHooked = false;
}

bool IsExternalProcessRunning()
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	if (g_EOSPlatformHandle == nullptr)
	{
		return false;
	}
	EOS_HAntiCheatClient acHandle = EOS_Platform_GetAntiCheatClientInterface(g_EOSPlatformHandle);
	return acHandle != nullptr;
}

PLUGIN_API int GetAnticheatIdentifier()
{
	return 9481;
}

void HookupEvents()
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	
	if (g_bEventsHooked)
	{
		return;
	}

	EOS_HAntiCheatClient acHandle = EOS_Platform_GetAntiCheatClientInterface(g_EOSPlatformHandle);
	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE NULL - Cannot hook events");
		return;
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

			{
				std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
				if (g_fnAnticheatIntegrityViolationOccurredCallback != nullptr)
				{
					g_fnAnticheatIntegrityViolationOccurredCallback(Data->ViolationMessage, (int)Data->ViolationType);
				}
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

			std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
			
			if (g_EOSPlatformHandle == nullptr)
			{
				return;
			}

			EOS_HAntiCheatClient acHandle = EOS_Platform_GetAntiCheatClientInterface(g_EOSPlatformHandle);
			
			if (acHandle == nullptr)
			{
				return;
			}

			if (Data->ClientHandle == nullptr || Data->MessageData == nullptr)
			{
				return;
			}

			// was it ourselves? just process immediately
			uint32_t targetUserID = (uint32_t)Data->ClientHandle;
			if (targetUserID == g_goUserID)
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
					PluginLog("[EAC][LOCAL] AC SEND MESSAGE TO PEER: %u bytes (User %u)", Data->MessageDataSizeBytes, (uint32_t)Data->ClientHandle);
				}
			}
			else // send via transport
			{
				PluginLog("[EAC][REMOTE] AC SEND MESSAGE TO PEER: %u bytes (User %u)", Data->MessageDataSizeBytes, targetUserID);
				if (g_fnSendMessageViaTransport != nullptr)
				{
					g_fnSendMessageViaTransport(targetUserID, Data->MessageData, Data->MessageDataSizeBytes);
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
			PluginLog("[EAC] AC PEER ACTION REQIRED: %s (%d - %d)", reasonStr, Data->ClientAction, Data->ActionReasonCode);

			{
				std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
				if (g_EOSPlatformHandle == nullptr)
				{
					return;
				}

				if (g_fnAnticheatActionCallback != nullptr)
				{
					uint32_t userID = (uint32_t)Data->ClientHandle;
					if (Data->ClientHandle == EOS_ANTICHEATCLIENT_PEER_SELF)
					{
						PluginLog("[EAC] AC PEER ACTION REQIRED: is self (%u)", userID);
					}
					else
					{
						PluginLog("[EAC] AC PEER ACTION REQIRED: is remote (%u)", userID);
					}
					g_fnAnticheatActionCallback(userID, Data->ActionReasonDetailsString, (int)(EAnticheatActionType)Data->ClientAction, (int)(EAnticheatActionReason)Data->ActionReasonCode);
				}
			}
		});
}

void BeginSession()
{
	PluginLog("[EAC] BEGIN SESSION");
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	EOS_HAntiCheatClient acHandle = EOS_Platform_GetAntiCheatClientInterface(g_EOSPlatformHandle);

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
	EOS_HAntiCheatClient acHandle = EOS_Platform_GetAntiCheatClientInterface(g_EOSPlatformHandle);

	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE NULL DeregisterPlayer");
		return false;
	}

	EOS_AntiCheatClient_UnregisterPeerOptions opts = {};
	opts.ApiVersion = EOS_ANTICHEATCLIENT_UNREGISTERPEER_API_LATEST;
	opts.PeerHandle = (void*)goUserID;
	EOS_EResult res = EOS_AntiCheatClient_UnregisterPeer(acHandle, &opts);

	PluginLog("[EAC] RegisterPlayer: Deregistering remote player %s - %d!", szMiddlewareUserID, goUserID);

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
	EOS_HAntiCheatClient acHandle = EOS_Platform_GetAntiCheatClientInterface(g_EOSPlatformHandle);

	if (acHandle == nullptr)
	{
		PluginLog("[EAC] AC HANDLE NULL RegisterPlayer");
		return false;
	}

	if (EOS_ProductUserId_FromString(szMiddlewareUserID) == g_EOSUserID)
	{
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
	opts.PeerProductUserId = EOS_ProductUserId_FromString(szMiddlewareUserID);
	EOS_EResult res = EOS_AntiCheatClient_RegisterPeer(acHandle, &opts);

	if (opts.PeerProductUserId == g_EOSUserID)
	{
		PluginLog("[EAC] RegisterPlayer: Registering local player %s - %d!", szMiddlewareUserID, goUserID);
		g_goUserID = goUserID;
	}
	else
	{
		PluginLog("[EAC] RegisterPlayer: Registering remote player %s - %d!", szMiddlewareUserID, goUserID);
	}
	
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
	EOS_HAntiCheatClient acHandle = EOS_Platform_GetAntiCheatClientInterface(g_EOSPlatformHandle);

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

	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	
	if (g_EOSPlatformHandle == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Platform not initialized!");
		if (cb != nullptr)
		{
			cb(false);
		}
		return;
	}

	EOS_HConnect ConnectHandle = EOS_Platform_GetConnectInterface(g_EOSPlatformHandle);
	
	if (ConnectHandle == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Connect handle is null!");
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

				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
					if (g_LoginCallback != nullptr)
					{
						g_LoginCallback(Data->ResultCode == EOS_EResult::EOS_Success);
					}
				}
			}
			else
			{
				PluginLog("[EAC] Token Refresh Failed: %p", Data);
				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
					if (g_LoginCallback != nullptr)
					{
						g_LoginCallback(Data->ResultCode == EOS_EResult::EOS_Success);
					}
				}
			}
		});
}

void Login(const char* szGameToken, LoginCallback cb)
{
	std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
	g_LoginCallback = cb;

	// Validate token pointer before logging to prevent format string vulnerabilities
	if (szGameToken != nullptr)
	{
		PluginLog("[EAC] Connect EOS: %s", szGameToken);
	}
	else
	{
		PluginLog("[EAC] Connect EOS: (null token)");
	}

	PluginLog("[EAC] A");
	PluginLog("[EAC] B");
	if (g_EOSPlatformHandle == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Platform not initialized!");
		if (cb != nullptr)
		{
			cb(false);
		}
		return;
	}
	PluginLog("[EAC] C");
	EOS_HConnect ConnectHandle = EOS_Platform_GetConnectInterface(g_EOSPlatformHandle);
	
	if (ConnectHandle == nullptr)
	{
		PluginLog("[EAC] MIDDLEWARE ERROR: Connect handle is null!");
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
	PluginLog("[EAC] D");
	// TODO: Start a timeout
	EOS_Connect_Login(ConnectHandle, &Options, nullptr, [](const EOS_Connect_LoginCallbackInfo* Data)
		{
			PluginLog("[EAC] done");
			if (Data == nullptr)
			{
				return;
			}

			PluginLog("[EAC] done2");

			// TODO: Clear timeout

			if (Data->ResultCode == EOS_EResult::EOS_Success)
			{
				PluginLog("[EAC] done 2a");
				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
					g_EOSUserID = Data->LocalUserId;
				}

				PluginLog("[EAC] done 3");
				char szBuffer[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = { 0 };
				int32_t outLen = sizeof(szBuffer);
				EOS_ProductUserId_ToString(Data->LocalUserId, szBuffer, &outLen);

				PluginLog("[EAC] Login Complete: %s", szBuffer);

				HookupEvents();

				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
					if (g_LoginCallback != nullptr)
					{
						g_LoginCallback(Data->ResultCode == EOS_EResult::EOS_Success);
					}
				}
			}
			else if (Data->ResultCode == EOS_EResult::EOS_InvalidUser)
			{
				PluginLog("[EAC] done 4");
				std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
				
				if (g_EOSPlatformHandle == nullptr)
				{
					PluginLog("[EAC] ERROR: Platform handle is null in login callback!");
					if (g_LoginCallback != nullptr)
					{
						g_LoginCallback(false);
					}
					return;
				}

				EOS_HConnect ConnectHandle = EOS_Platform_GetConnectInterface(g_EOSPlatformHandle);
				if (ConnectHandle == nullptr)
				{
					PluginLog("[EAC] ERROR: Connect handle is null!");
					if (g_LoginCallback != nullptr)
					{
						g_LoginCallback(false);
					}
					return;
				}

				EOS_Connect_CreateUserOptions Options = {};
				Options.ApiVersion = EOS_CONNECT_CREATEUSER_API_LATEST;

				if (Data->ContinuanceToken != NULL)
				{
					Options.ContinuanceToken = Data->ContinuanceToken;
				}

				// NOTE: We're not deleting the received context because we're passing it down to another SDK call
				EOS_Connect_CreateUser(ConnectHandle, &Options, nullptr,
					[](const EOS_Connect_CreateUserCallbackInfo* Data)
					{
						if (Data == nullptr)
						{
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

						{
							std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
							if (g_LoginCallback != nullptr)
							{
								g_LoginCallback(Data->ResultCode == EOS_EResult::EOS_Success);
							}
						}
					}
				);
			}
			else
			{
				PluginLog("[EAC] done 5");
				PluginLog("[EAC] Account Link Failed");
				{
					std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
					if (g_LoginCallback != nullptr)
					{
						g_LoginCallback(Data->ResultCode == EOS_EResult::EOS_Success);
					}
				}
			}
		});
}