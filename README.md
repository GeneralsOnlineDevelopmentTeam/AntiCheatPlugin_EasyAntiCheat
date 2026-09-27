# AntiCheatPlugin_EasyAntiCheat
AntiCheat Plugin for EasyAntiCheat

# External Dependencies

To successfully build this project, you will need to place an x86 Windows EOS (Epic Online Services) SDK in the "EOS" folder.

This can be obtained from Epic Games / Easy AntiCheat (https://onlineservices.epicgames.com/) and requires acceptance of Epic Games license, which is NOT GPL compatible (but is LGPL compatible).

# Configuration

You will also need to configure an App/Product on the Epic Games Dev Portal and supply its
credentials at build time. They are compile-time definitions, not source edits:

```
cmake -A Win32 -B build ^
  -DEAC_EOS_PRODUCT_ID=... ^
  -DEAC_EOS_SANDBOX_ID=... ^
  -DEAC_EOS_DEPLOYMENT_ID=... ^
  -DEAC_EOS_CLIENT_ID=... ^
  -DEAC_EOS_CLIENT_SECRET=...
```

If any of these are left unset, `Initialize()` logs a fatal error and returns `5`
(`EInitializeResult_MissingCredentials`) instead of creating a platform that can never work.

# Lifetime

The host application **must** call the exported `Shutdown()` before unloading the DLL.
Teardown is deliberately *not* performed in `DllMain(DLL_PROCESS_DETACH)`: releasing the EOS
platform there runs under the Windows loader lock and deadlocks or faults.
