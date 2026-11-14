#include "CursorMcpServer.h"
#include "CursorUnrealSettings.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

class FCursorUnrealMCPModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Commandlets (cooking, packaging, UAT) have no use for an agent endpoint.
		if (IsRunningCommandlet())
		{
			return;
		}

		if (GEngine)
		{
			OnEngineReady();
		}
		else
		{
			PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FCursorUnrealMCPModule::OnEngineReady);
		}
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		if (UObjectInitialized())
		{
			if (UCursorUnrealSettings* Settings = UCursorUnrealSettings::GetMutable())
			{
				Settings->OnSettingChanged().Remove(SettingsChangedHandle);
			}
		}
		FCursorMcpServer::Get().Stop();
	}

private:
	void OnEngineReady()
	{
		FCursorMcpServer::Get().ApplySettings();
		SettingsChangedHandle = UCursorUnrealSettings::GetMutable()->OnSettingChanged().AddLambda(
			[](UObject*, FPropertyChangedEvent&)
			{
				FCursorMcpServer::Get().ApplySettings();
			});
	}

	FDelegateHandle PostEngineInitHandle;
	FDelegateHandle SettingsChangedHandle;
};

IMPLEMENT_MODULE(FCursorUnrealMCPModule, CursorUnrealMCP);
