#if WITH_DEV_AUTOMATION_TESTS

#include "CursorJson.h"
#include "CursorTestSupport.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Editor.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorNiagaraToolsTest, "CursorUnreal.Tools.Niagara", CursorTest::Flags)
bool FCursorNiagaraToolsTest::RunTest(const FString&)
{
	if (!FModuleManager::Get().IsModuleLoaded(TEXT("CursorUnrealToolsNiagara"))
		&& !FModuleManager::Get().LoadModule(TEXT("CursorUnrealToolsNiagara")))
	{
		AddWarning(TEXT("CursorUnrealToolsNiagara is not loaded; skipping Niagara tools test."));
		return true;
	}

	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);

	const FString Path = FString::Printf(TEXT("/Game/CursorUnrealTests/NS_CUTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
	auto Cleanup = [Path]()
	{
		if (UEditorAssetSubsystem* Assets = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr)
		{
			if (Assets->DoesAssetExist(Path))
			{
				Assets->DeleteAsset(Path);
			}
		}
		FString File;
		if (FPackageName::TryConvertLongPackageNameToFilename(Path, File, FPackageName::GetAssetPackageExtension()))
		{
			IFileManager::Get().Delete(*File, false, true);
		}
	};

	FCursorToolResult R = CursorTest::Call(TEXT("niagara_create_system"),
		*FString::Printf(TEXT("{\"path\":\"%s\"}"), *Path));
	TestFalse(TEXT("niagara_create_system succeeds: ") + R.Text, R.bIsError);
	if (R.bIsError)
	{
		Cleanup();
		return true;
	}

	R = CursorTest::Call(TEXT("niagara_create_system"),
		*FString::Printf(TEXT("{\"path\":\"%s\"}"), *Path));
	TestTrue(TEXT("duplicate create is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("niagara_get_info"),
		*FString::Printf(TEXT("{\"system\":\"%s\"}"), *Path));
	TestFalse(TEXT("niagara_get_info succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("niagara_list_parameters"),
		*FString::Printf(TEXT("{\"system\":\"%s\"}"), *Path));
	TestFalse(TEXT("niagara_list_parameters succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("niagara_compile"),
		*FString::Printf(TEXT("{\"system\":\"%s\"}"), *Path));
	TestFalse(TEXT("niagara_compile succeeds: ") + R.Text, R.bIsError);

	Cleanup();
	return true;
}

#endif
