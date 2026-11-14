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

	/** A small picture for tests. Different seeds give different pictures. bPhoto makes it a JPEG, otherwise a PNG. */
	inline FCursorImage MakeImage(int32 Seed, int32 Width = 8, int32 Height = 6, bool bPhoto = false, const TCHAR* Name = TEXT("test picture"))
	{
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Width * Height);
		for (int32 Index = 0; Index < Pixels.Num(); ++Index)
		{
			Pixels[Index] = FColor(static_cast<uint8>(Index * 37 + Seed * 53), static_cast<uint8>((Index / Width) * 19 + Seed * 101), static_cast<uint8>(Seed * 29), 255);
		}
		FCursorImage Image;
		FString Error;
		CursorImages::FromPixels(Pixels, Width, Height, Name, bPhoto, Image, Error);
		return Image;
	}

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
