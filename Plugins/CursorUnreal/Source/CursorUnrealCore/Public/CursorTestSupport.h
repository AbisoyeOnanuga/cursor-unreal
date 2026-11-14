#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CursorApprovalGate.h"
#include "CursorJson.h"
#include "CursorToolRegistry.h"
#include "CursorUnrealSettings.h"
#include "Misc/AutomationTest.h"

/** Shared by the automation tests of every module in the plugin. */
namespace CursorTest
{
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/**
	 * Gives a test a known approval policy and no approval UI, then puts the user's settings and
	 * the real approval handler back, so running tests in an open editor leaves it as it was.
	 */
	struct FScopedPolicy
	{
		ECursorApprovalPolicy SavedPolicy;
		TArray<FString> SavedAllowed;
		TArray<FName> SavedDisabledPacks;
		FCursorApprovalHandler SavedHandler;
		FName PackToRemove;

		explicit FScopedPolicy(FName InPackToRemove = NAME_None, ECursorApprovalPolicy Policy = ECursorApprovalPolicy::AskForDangerous)
			: PackToRemove(InPackToRemove)
		{
			UCursorUnrealSettings* Settings = UCursorUnrealSettings::GetMutable();
			SavedPolicy = Settings->ApprovalPolicy;
			SavedAllowed = Settings->AlwaysAllowedTools;
			SavedDisabledPacks = Settings->DisabledToolPacks;
			SavedHandler = FCursorApprovalGate::Get().GetHandler();
			Settings->ApprovalPolicy = Policy;
			Settings->AlwaysAllowedTools.Reset();
			Settings->DisabledToolPacks.Reset();
			FCursorApprovalGate::Get().ClearHandler();
			FCursorApprovalGate::Get().ResetSessionAllowances();
		}

		~FScopedPolicy()
		{
			UCursorUnrealSettings* Settings = UCursorUnrealSettings::GetMutable();
			Settings->ApprovalPolicy = SavedPolicy;
			Settings->AlwaysAllowedTools = SavedAllowed;
			Settings->DisabledToolPacks = SavedDisabledPacks;
			FCursorApprovalGate::Get().ResetSessionAllowances();
			if (SavedHandler.IsBound())
			{
				FCursorApprovalGate::Get().SetHandler(SavedHandler);
			}
			else
			{
				FCursorApprovalGate::Get().ClearHandler();
			}
			if (!PackToRemove.IsNone())
			{
				FCursorToolRegistry::Get().UnregisterPack(PackToRemove);
			}
		}
	};

	/** Calls a tool as the in-editor agent and returns its result. Only for tools that finish before returning. */
	inline FCursorToolResult Call(const TCHAR* Tool, const TCHAR* ArgsJson = TEXT("{}"), const FString& Caller = FCursorToolRegistry::CallerNative)
	{
		FCursorToolContext Ctx;
		Ctx.Caller = Caller;
		Ctx.SessionId = TEXT("test");
		FCursorToolResult Out = FCursorToolResult::Error(TEXT("tool did not complete synchronously"));
		FCursorToolRegistry::Get().Invoke(FName(Tool), CursorJson::Parse(ArgsJson), Ctx, [&Out](const FCursorToolResult& R) { Out = R; });
		return Out;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
