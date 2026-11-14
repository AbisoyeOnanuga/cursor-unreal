#if WITH_DEV_AUTOMATION_TESTS

#include "AgentBackend.h"
#include "CursorBridgeBackend.h"
#include "CursorBridgeInstall.h"
#include "CursorUnrealSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"

// Runs the real Node bridge against the stand-in SDK. Nothing is billed and no Cursor account is used.

namespace CursorBridgeTests
{
	FString FakeSdkPath()
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CursorUnreal"));
		check(Plugin.IsValid());
		return FPaths::ConvertRelativePathToFull(
			Plugin->GetBaseDir() / TEXT("Bridge") / TEXT("test") / TEXT("helpers") / TEXT("fake-sdk.mjs"));
	}

	struct FTurnState
	{
		TSharedPtr<FCursorBridgeBackend> Backend;
		TArray<FAgentEvent> Events;
		bool bTimedOut = false;
	};

	struct FScopedCursorSettings
	{
		FString Model;
		ECursorBuiltinTools Builtin;

		FScopedCursorSettings()
		{
			UCursorUnrealSettings* S = UCursorUnrealSettings::GetMutable();
			Model = S->CursorModel;
			Builtin = S->CursorBuiltinTools;
			S->CursorModel = TEXT("stand-in-large");
			S->CursorBuiltinTools = ECursorBuiltinTools::None;
		}

		~FScopedCursorSettings()
		{
			UCursorUnrealSettings* S = UCursorUnrealSettings::GetMutable();
			S->CursorModel = Model;
			S->CursorBuiltinTools = Builtin;
		}
	};
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FCursorBridgeWaitIdle,
	TSharedRef<CursorBridgeTests::FTurnState>, State,
	double, DeadlineSeconds);

bool FCursorBridgeWaitIdle::Update()
{
	if (!State->Backend.IsValid() || !State->Backend->IsBusy())
	{
		return true;
	}
	if (FPlatformTime::Seconds() > DeadlineSeconds)
	{
		State->bTimedOut = true;
		State->Backend->Cancel();
		return true;
	}
	return false;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(
	FCursorBridgeAssertTurn,
	FAutomationTestBase*, Test,
	TSharedRef<CursorBridgeTests::FTurnState>, State);

bool FCursorBridgeAssertTurn::Update()
{
	Test->TestFalse(TEXT("The bridge turn finished in time"), State->bTimedOut);
	Test->TestFalse(TEXT("The backend is idle"), State->Backend.IsValid() && State->Backend->IsBusy());

	bool bSawText = false;
	bool bSawDone = false;
	FString Combined;
	for (const FAgentEvent& Event : State->Events)
	{
		if (Event.Type == EAgentEventType::TextDelta)
		{
			bSawText = true;
			Combined += Event.Text;
		}
		if (Event.Type == EAgentEventType::Done)
		{
			bSawDone = true;
			Test->TestEqual(TEXT("Outcome"), Event.Outcome, FString(TEXT("done")));
		}
		if (Event.Type == EAgentEventType::Error)
		{
			Test->AddError(FString::Printf(TEXT("Bridge error: %s"), *Event.Text));
		}
	}
	Test->TestTrue(TEXT("Received text"), bSawText);
	Test->TestTrue(TEXT("Received done"), bSawDone);
	Test->TestEqual(TEXT("Stand-in reply"), Combined, FString(TEXT("Hello from the stand-in.")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCursorBridgeInstallTest,
	"CursorUnreal.Agent.BridgeInstall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCursorBridgeInstallTest::RunTest(const FString& Parameters)
{
	const FCursorBridgeInstall Install = CursorBridge::ProbeInstall();
	TestTrue(TEXT("Node is on PATH for developers of this plugin"), !CursorBridge::FindNodeExecutable().IsEmpty());
	TestTrue(TEXT("bridge.mjs is present"), FPaths::FileExists(CursorBridge::GetBridgeScriptPath()));
	TestTrue(TEXT("The stand-in SDK module is present for tests"), FPaths::FileExists(CursorBridgeTests::FakeSdkPath()));
	if (Install.bOk)
	{
		TestFalse(TEXT("SDK version is reported when installed"), Install.SdkVersion.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCursorBridgeTurnTest,
	"CursorUnreal.Agent.BridgeTurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCursorBridgeTurnTest::RunTest(const FString& Parameters)
{
	using namespace CursorBridgeTests;
	if (!FPaths::FileExists(FakeSdkPath()) || CursorBridge::FindNodeExecutable().IsEmpty())
	{
		AddInfo(TEXT("Skipping: Node or the stand-in SDK is missing."));
		return true;
	}

	// Kept alive for the latent commands; settings are restored when this frame's locals die, which
	// is before the latent commands run — so pin the model on the backend path via Options only.
	// CursorModel is still read from settings inside GetModelId(), so set it and leave it for the
	// duration of the latent commands by heap-allocating the scope.
	const TSharedRef<FScopedCursorSettings> Settings = MakeShared<FScopedCursorSettings>();
	const TSharedRef<FTurnState> State = MakeShared<FTurnState>();

	FCursorBridgeBackendOptions Options;
	Options.KeyProvider = []() { return FString(TEXT("cursor_test_key")); };
	Options.SdkModuleOverride = FakeSdkPath();
	Options.bSkipInstallProbe = true;

	State->Backend = MakeShared<FCursorBridgeBackend>(MoveTemp(Options));
	State->Backend->SendTurn(
		FAgentTurnRequest{ TEXT("Say hello"), {} },
		[State](const FAgentEvent& Event) { State->Events.Add(Event); });

	AddCommand(new FCursorBridgeWaitIdle(State, FPlatformTime::Seconds() + 15.0));
	AddCommand(new FCursorBridgeAssertTurn(this, State));
	AddCommand(new FDelayedFunctionLatentCommand([Settings, State]()
	{
		// Drop the backend (and its Node process) only after assertions have run.
		if (State->Backend.IsValid())
		{
			State->Backend.Reset();
		}
		(void)Settings;
	}));
	return true;
}

namespace CursorBridgeTests
{
	struct FModelsState
	{
		TSharedPtr<FCursorBridgeBackend> Backend;
		/** What the key store holds. The stand-in SDK refuses "bad-key". */
		FString Key = TEXT("bad-key");
		int32 Answers = 0;
		bool bLastOk = false;
		FString LastError;
		TArray<FAgentModelInfo> LastModels;
		double Deadline = 0.0;

		void Ask()
		{
			Deadline = FPlatformTime::Seconds() + 15.0;
			Backend->RefreshModels([this](bool bOk, const TArray<FAgentModelInfo>& Models, const FString& Error)
			{
				++Answers;
				bLastOk = bOk;
				LastModels = Models;
				LastError = Error;
			});
		}

		bool Waited(int32 Expected) const
		{
			return Answers >= Expected || FPlatformTime::Seconds() > Deadline;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCursorBridgeModelsTest,
	"CursorUnreal.Agent.BridgeModels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCursorBridgeModelsTest::RunTest(const FString& Parameters)
{
	using namespace CursorBridgeTests;
	if (!FPaths::FileExists(FakeSdkPath()) || CursorBridge::FindNodeExecutable().IsEmpty())
	{
		AddInfo(TEXT("Skipping: Node or the stand-in SDK is missing."));
		return true;
	}

	// "auto" is what older settings files hold; the list Cursor returns calls the same thing "default".
	{
		UCursorUnrealSettings* S = UCursorUnrealSettings::GetMutable();
		const FString Saved = S->CursorModel;
		const TSharedRef<FCursorBridgeBackend> Probe = MakeShared<FCursorBridgeBackend>();
		S->CursorModel = TEXT("auto");
		TestEqual(TEXT("'auto' is sent as 'default'"), Probe->GetModelId(), FString(TEXT("default")));
		S->CursorModel = TEXT("composer-2");
		TestEqual(TEXT("other ids are sent as they are"), Probe->GetModelId(), FString(TEXT("composer-2")));
		S->CursorModel = Saved;
	}

	const TSharedRef<FModelsState> State = MakeShared<FModelsState>();
	const TWeakPtr<FModelsState> WeakState = State;

	FCursorBridgeBackendOptions Options;
	Options.KeyProvider = [WeakState]()
	{
		const TSharedPtr<FModelsState> Pinned = WeakState.Pin();
		return Pinned.IsValid() ? Pinned->Key : FString();
	};
	Options.SdkModuleOverride = FakeSdkPath();
	Options.bSkipInstallProbe = true;
	// The real list under Saved/ belongs to the user's account. Leave it alone.
	Options.bPersistModels = false;
	State->Backend = MakeShared<FCursorBridgeBackend>(MoveTemp(Options));
	TestEqual(TEXT("no list before asking"), State->Backend->GetCachedModels().Num(), 0);

	// A key the provider refuses: the failure is reported, not swallowed.
	State->Ask();
	AddCommand(new FFunctionLatentCommand([State]() { return State->Waited(1); }));
	AddCommand(new FFunctionLatentCommand([this, State]()
	{
		TestEqual(TEXT("the refused key was answered"), State->Answers, 1);
		TestFalse(TEXT("the refused key fails"), State->bLastOk);
		TestFalse(TEXT("the failure says why"), State->LastError.IsEmpty());
		TestEqual(TEXT("still no list"), State->Backend->GetCachedModels().Num(), 0);

		// The user saves a working key while the bridge is still running with the old one. Two parts of
		// the panel ask at once (the Setup tab and the Model menu); both must be answered.
		State->Key = TEXT("cursor_test_key");
		State->Ask();
		State->Ask();
		return true;
	}));
	AddCommand(new FFunctionLatentCommand([State]() { return State->Waited(3); }));
	AddCommand(new FFunctionLatentCommand([this, State]()
	{
		TestEqual(TEXT("both askers were answered"), State->Answers, 3);
		TestTrue(FString::Printf(TEXT("the new key is used without restarting the editor (%s)"), *State->LastError), State->bLastOk);
		TestEqual(TEXT("the stand-in catalogue arrived"), State->LastModels.Num(), 2);
		if (State->LastModels.Num() > 0)
		{
			TestEqual(TEXT("first model id"), State->LastModels[0].Id, FString(TEXT("stand-in-large")));
			TestEqual(TEXT("first model name"), State->LastModels[0].DisplayName, FString(TEXT("Stand-in Large")));
		}
		TestEqual(TEXT("the list is kept for the Model menu"), State->Backend->GetCachedModels().Num(), 2);
		// Drop the backend, and its Node process, now that everything has been checked.
		State->Backend.Reset();
		return true;
	}));
	return true;
}

#endif
