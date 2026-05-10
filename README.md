# AntiCheatPlugin_EasyAntiCheat
AntiCheat Plugin for EasyAntiCheat

# External Dependencies

To successfully build this project, you will need to place an x86 Windows EOS (Epic Online Services) SDK in the "EOS" folder.

This can be obtained from Epic Games / Easy AntiCheat (https://onlineservices.epicgames.com/) and requires acceptance of Epic Games license, which is NOT GPL compatible (but is LGPL compatible).

# Configuration

You will also need to configure an App/Product on the Epic Games Dev Portal and set the following variables in code:

PlatformOptions.ProductId = "TODO";
PlatformOptions.SandboxId = "TODO";
PlatformOptions.DeploymentId = "TODO";
PlatformOptions.ClientCredentials.ClientId = "TODO";
PlatformOptions.ClientCredentials.ClientSecret = "TODO";