#include "CursorActivity.h"
#include "CursorCoreTools.h"
#include "CursorLogCapture.h"
#include "CursorSkillLibrary.h"
#include "CursorUnrealLog.h"
#include "Editor.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogCursorUnreal);

class FCursorUnrealCoreModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FCursorLogCapture::Get().Start();
		CursorCoreTools::Register();

		// GEditor does not exist yet when editor modules start, so anything that needs it waits.
		// If the module is loaded late (after engine init), there is nothing to wait for.
		if (GEditor != nullptr)
		{
			OnEngineReady();
		}
		else
		{
			PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddStatic(&FCursorUnrealCoreModule::OnEngineReady);
		}
	}

	static void OnEngineReady()
	{
		FCursorActivity::Get().InstallThrottleOverride();
		FCursorSkillLibrary::Get().StartWatching();
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		FCursorSkillLibrary::Get().StopWatching();
		FCursorActivity::Get().RemoveThrottleOverride();
		CursorCoreTools::Unregister();
		FCursorLogCapture::Get().Stop();
	}

private:
	FDelegateHandle PostEngineInitHandle;
};

IMPLEMENT_MODULE(FCursorUnrealCoreModule, CursorUnrealCore);
