#if WITH_DEV_AUTOMATION_TESTS

#include "CursorPackSupport.h"
#include "CursorPackTestSupport.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

using namespace CursorPackTest;

namespace
{
	FString ReadFile(const FString& Path)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		return Text;
	}

	/** A folder under Saved that the test owns and removes. */
	struct FScopedTempDir
	{
		FString Path;
		FScopedTempDir() : Path(FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CursorUnreal") / TEXT("Tests") / FGuid::NewGuid().ToString(EGuidFormats::Short)))
		{
			IFileManager::Get().MakeDirectory(*Path, true);
		}
		~FScopedTempDir() { IFileManager::Get().DeleteDirectory(*Path, false, true); }
	};

	FString ProjectSourceDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
	}
}

// Pure helpers: no editor state, no project files.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorCppSupportTest, "CursorUnreal.Tools.Cpp.Support", CursorTest::Flags)
bool FCursorCppSupportTest::RunTest(const FString&)
{
	// ---- compiler output ------------------------------------------------------------------------------------------------
	const FString Output = TEXT(
		"Building 3 actions with 8 processes...\n"
		"D:\\Proj\\Source\\Game\\Door.cpp(12,5): error C2065: 'Foo': undeclared identifier\n"
		"D:\\Proj\\Source\\Game\\Door.cpp(12,5): error C2065: 'Foo': undeclared identifier\n"
		"D:\\Proj\\Source\\Game\\Door.h(7): warning C4100: 'Delta': unreferenced parameter\n"
		"D:\\Proj\\Source\\Game\\Gate.cpp(3): fatal error C1083: Cannot open include file: 'Nope.h': No such file or directory\n"
		"/proj/Source/Game/Latch.cpp:44:10: error: use of undeclared identifier 'Bar'\n"
		"/proj/Source/Game/Latch.cpp:50:1: warning: unused variable 'x'\n"
		"LINK : fatal error LNK1104: cannot open file 'UnrealEditor-Game.dll'\n"
		"ERROR: Unable to instantiate module 'Broken': Could not find definition for module\n"
		"Total execution time: 4.2 seconds\n");
	TArray<CursorSource::FCompilerMessage> Messages;
	CursorSource::ParseCompilerOutput(Output, Messages);
	TestEqual(TEXT("duplicate diagnostics are reported once"), Messages.Num(), 7);
	if (Messages.Num() == 7)
	{
		TestEqual(TEXT("MSVC file"), Messages[0].File, FString(TEXT("D:\\Proj\\Source\\Game\\Door.cpp")));
		TestEqual(TEXT("MSVC line"), Messages[0].Line, 12);
		TestEqual(TEXT("MSVC column"), Messages[0].Column, 5);
		TestEqual(TEXT("MSVC code"), Messages[0].Code, FString(TEXT("C2065")));
		TestEqual(TEXT("MSVC severity"), Messages[0].Severity, FString(TEXT("error")));
		TestTrue(TEXT("MSVC text"), Messages[0].Text.Contains(TEXT("undeclared identifier")));
		TestEqual(TEXT("MSVC warning severity"), Messages[1].Severity, FString(TEXT("warning")));
		TestEqual(TEXT("MSVC line without a column"), Messages[1].Line, 7);
		TestEqual(TEXT("fatal errors count as errors"), Messages[2].Severity, FString(TEXT("error")));
		TestEqual(TEXT("clang file keeps its path"), Messages[3].File, FString(TEXT("/proj/Source/Game/Latch.cpp")));
		TestEqual(TEXT("clang line"), Messages[3].Line, 44);
		TestEqual(TEXT("clang column"), Messages[3].Column, 10);
		TestEqual(TEXT("clang warning"), Messages[4].Severity, FString(TEXT("warning")));
		TestEqual(TEXT("linker error code"), Messages[5].Code, FString(TEXT("LNK1104")));
		TestTrue(TEXT("linker errors have no file"), Messages[5].File.IsEmpty());
		TestTrue(TEXT("UBT's own errors are kept"), Messages[6].Text.Contains(TEXT("Unable to instantiate module")));
	}
	Messages.Reset();
	CursorSource::ParseCompilerOutput(TEXT("Total execution time: 4.2 seconds\nResult: Succeeded\n"), Messages);
	TestEqual(TEXT("a clean build has no diagnostics"), Messages.Num(), 0);

	// ---- path confinement -----------------------------------------------------------------------------------------------
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CursorUnreal") / TEXT("FakeProject") / TEXT("Source"));
	const FString Base = FPaths::GetPath(Root);
	const TArray<FString> Roots = { Root };
	FString Absolute;
	FString Error;
	TestTrue(TEXT("a relative path inside the root resolves"), CursorSource::ResolveWithinRoots(TEXT("Source/Game/Door.h"), Base, Roots, Absolute, Error));
	TestTrue(TEXT("the result is absolute and normalised"), Absolute.StartsWith(Root) && !Absolute.Contains(TEXT("\\")));
	TestTrue(TEXT("an absolute path inside the root resolves"), CursorSource::ResolveWithinRoots(Root / TEXT("A/B.cpp"), Base, Roots, Absolute, Error));
	TestFalse(TEXT("'..' cannot climb out of the root"), CursorSource::ResolveWithinRoots(TEXT("Source/../Config/DefaultEngine.ini"), Base, Roots, Absolute, Error));
	TestFalse(TEXT("'..' hidden deeper cannot climb out either"), CursorSource::ResolveWithinRoots(TEXT("Source/Game/../../../Secret.h"), Base, Roots, Absolute, Error));
	TestFalse(TEXT("a sibling folder with the same prefix is outside"), CursorSource::ResolveWithinRoots(Root + TEXT("Evil/Door.h"), Base, Roots, Absolute, Error));
	TestFalse(TEXT("the root itself is not a file"), CursorSource::ResolveWithinRoots(Root, Base, Roots, Absolute, Error));
	TestFalse(TEXT("an empty path is rejected"), CursorSource::ResolveWithinRoots(TEXT("  "), Base, Roots, Absolute, Error));
	TestFalse(TEXT("an absolute path elsewhere is rejected"), CursorSource::ResolveWithinRoots(FPaths::EngineDir() / TEXT("Source/Runtime/Core/Public/CoreMinimal.h"), Base, Roots, Absolute, Error));

	// The real roots: project writes are limited to Source trees and never reach into the engine.
	TestFalse(TEXT("engine source is not writable"), CursorSource::ResolveForWrite(TEXT("Engine/Source/Runtime/Core/Public/CoreMinimal.h"), Absolute, Error));
	TestFalse(TEXT("an absolute engine path is not writable"), CursorSource::ResolveForWrite(FPaths::EngineDir() / TEXT("Source/Runtime/Core/Public/CoreMinimal.h"), Absolute, Error));
	TestFalse(TEXT("config files are not writable"), CursorSource::ResolveForWrite(TEXT("Config/DefaultEngine.ini"), Absolute, Error));
	TestFalse(TEXT("the project file is not writable"), CursorSource::ResolveForWrite(TEXT("CursorUnrealDev.uproject"), Absolute, Error));
	TestFalse(TEXT("'..' out of Source is not writable"), CursorSource::ResolveForWrite(TEXT("Source/../Config/DefaultEngine.ini"), Absolute, Error));
	TestFalse(TEXT("non-source files under Source are not writable: ") + Error, CursorSource::ResolveForWrite(TEXT("Source/Game/notes.txt"), Absolute, Error));
	TestFalse(TEXT("this plugin's own source is not writable"), CursorSource::ResolveForWrite(TEXT("Plugins/CursorUnreal/Source/CursorUnrealTools/Private/EditorTools.cpp"), Absolute, Error));
	TestTrue(TEXT("project source is writable"), CursorSource::ResolveForWrite(TEXT("Source/Game/Door.h"), Absolute, Error));
	TestTrue(TEXT("this plugin's source is readable"), CursorSource::ResolveForRead(TEXT("Plugins/CursorUnreal/Source/CursorUnrealTools/Private/EditorTools.cpp"), Absolute, Error));
	TestFalse(TEXT("project config is not readable by the source tools"), CursorSource::ResolveForRead(TEXT("Config/DefaultEngine.ini"), Absolute, Error));
	TestTrue(TEXT("engine headers are readable via an engine-relative path"), CursorSource::ResolveForRead(TEXT("Engine/Source/Runtime/Engine/Classes/GameFramework/Actor.h"), Absolute, Error));
	TestTrue(TEXT("source extensions are recognised"), CursorSource::IsSourceExtension(TEXT("A/B.CPP")) && CursorSource::IsSourceExtension(TEXT("X.Build.cs")) && CursorSource::IsSourceExtension(TEXT("a.inl")));
	TestFalse(TEXT("other extensions are not"), CursorSource::IsSourceExtension(TEXT("a.uasset")) || CursorSource::IsSourceExtension(TEXT("a.ini")) || CursorSource::IsSourceExtension(TEXT("noext")));

	// ---- backups --------------------------------------------------------------------------------------------------------
	{
		FScopedTempDir Temp;
		const FString File = Temp.Path / TEXT("Source/Game/Door.h");
		FFileHelper::SaveStringToFile(TEXT("original contents"), *File);
		const FString BackupRoot = Temp.Path / TEXT("Backups");
		FString BackupError;
		const FString Backup = CursorSource::BackupFile(File, Temp.Path, BackupRoot, BackupError);
		TestFalse(TEXT("a backup path is returned: ") + BackupError, Backup.IsEmpty());
		TestTrue(TEXT("the backup is under the backup root"), Backup.StartsWith(BackupRoot));
		TestTrue(TEXT("the backup keeps the relative path"), Backup.EndsWith(TEXT("Source/Game/Door.h")));
		TestEqual(TEXT("the backup holds the original"), ReadFile(Backup), FString(TEXT("original contents")));
		TestTrue(TEXT("nothing to back up for a new file"), CursorSource::BackupFile(Temp.Path / TEXT("missing.h"), Temp.Path, BackupRoot, BackupError).IsEmpty());
		// Two backups in quick succession must not overwrite each other.
		FFileHelper::SaveStringToFile(TEXT("second"), *File);
		FPlatformProcess::Sleep(0.01f);
		const FString Second = CursorSource::BackupFile(File, Temp.Path, BackupRoot, BackupError);
		TestTrue(TEXT("a second backup is separate"), Second != Backup && ReadFile(Backup) == TEXT("original contents") && ReadFile(Second) == TEXT("second"));
	}

	// ---- generated code -------------------------------------------------------------------------------------------------
	CursorSource::FNewClassSpec Spec;
	Spec.Name = TEXT("Door");
	Spec.ModuleName = TEXT("MyGame");
	Spec.Prefix = TEXT("A");
	Spec.ParentCppName = TEXT("AActor");
	Spec.ParentInclude = TEXT("GameFramework/Actor.h");
	Spec.OwnIncludePath = TEXT("Gameplay/Door.h");
	Spec.bIsActor = true;
	const FString Header = CursorSource::GenerateHeader(Spec);
	TestTrue(TEXT("header has pragma once"), Header.Contains(TEXT("#pragma once")));
	TestTrue(TEXT("header includes the parent"), Header.Contains(TEXT("#include \"GameFramework/Actor.h\"")));
	TestTrue(TEXT("generated.h is last include"), Header.Find(TEXT("Door.generated.h")) > Header.Find(TEXT("GameFramework/Actor.h")));
	TestTrue(TEXT("class uses the module's API macro"), Header.Contains(TEXT("class MYGAME_API ADoor : public AActor")));
	TestTrue(TEXT("class is Blueprintable by default"), Header.Contains(TEXT("UCLASS(Blueprintable)")));
	TestTrue(TEXT("GENERATED_BODY is present"), Header.Contains(TEXT("GENERATED_BODY()")));
	const FString Source = CursorSource::GenerateSource(Spec);
	TestTrue(TEXT("source includes its own header by the Public-relative path"), Source.StartsWith(TEXT("#include \"Gameplay/Door.h\"")));
	TestTrue(TEXT("actor constructor enables ticking"), Source.Contains(TEXT("ADoor::ADoor()")) && Source.Contains(TEXT("PrimaryActorTick.bCanEverTick = true;")));
	Spec.bIsActor = false;
	Spec.bIsActorComponent = true;
	Spec.bBlueprintable = false;
	TestTrue(TEXT("component constructor enables component ticking"), CursorSource::GenerateSource(Spec).Contains(TEXT("PrimaryComponentTick.bCanEverTick")));
	TestTrue(TEXT("non-Blueprintable class says so"), CursorSource::GenerateHeader(Spec).Contains(TEXT("UCLASS()")));
	Spec.bIsActorComponent = false;
	TestFalse(TEXT("a plain object constructor is empty"), CursorSource::GenerateSource(Spec).Contains(TEXT("Tick")));

	// ---- names ----------------------------------------------------------------------------------------------------------
	TestTrue(TEXT("a normal name is valid"), CursorSource::IsValidClassName(TEXT("Door_Mk2"), Error));
	TestFalse(TEXT("a leading digit is invalid"), CursorSource::IsValidClassName(TEXT("2Door"), Error));
	TestFalse(TEXT("spaces are invalid"), CursorSource::IsValidClassName(TEXT("My Door"), Error));
	TestFalse(TEXT("path characters are invalid"), CursorSource::IsValidClassName(TEXT("../Door"), Error));
	TestFalse(TEXT("an empty name is invalid"), CursorSource::IsValidClassName(TEXT(""), Error));
	TestFalse(TEXT("a double underscore is reserved"), CursorSource::IsValidClassName(TEXT("Do__or"), Error));
	return true;
}

// The tools themselves, against the real project.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorCppToolsTest, "CursorUnreal.Tools.Cpp.Tools", CursorTest::Flags)
bool FCursorCppToolsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedFiles Cleanup;

	// ---- registration: risk and audience --------------------------------------------------------------------------------
	FCursorToolRegistry& Registry = FCursorToolRegistry::Get();
	for (const TCHAR* Name : { TEXT("source_read"), TEXT("source_search"), TEXT("source_write") })
	{
		const TSharedPtr<ICursorTool> Tool = Registry.Find(FName(Name));
		TestTrue(FString(Name) + TEXT(" is registered"), Tool.IsValid());
		if (Tool.IsValid())
		{
			TestTrue(FString(Name) + TEXT(" is NativeAgentOnly"), Tool->GetAudience() == ECursorToolAudience::NativeAgentOnly);
			TestTrue(FString(Name) + TEXT(" is in the cpp pack"), Tool->GetPack() == FName(TEXT("cpp")));
		}
		const auto HasTool = [&](const FString& Caller)
		{
			return Registry.GetTools(Caller).ContainsByPredicate([&](const TSharedRef<ICursorTool>& T) { return T->GetName() == FName(Name); });
		};
		TestTrue(FString(Name) + TEXT(" is offered to the in-editor agent"), HasTool(FCursorToolRegistry::CallerNative));
		TestFalse(FString(Name) + TEXT(" is hidden from MCP"), HasTool(FCursorToolRegistry::CallerMcp));
		TestFalse(FString(Name) + TEXT(" is hidden from the Cursor SDK bridge"), HasTool(FCursorToolRegistry::CallerCursorSdk));
	}
	TestTrue(TEXT("source_read is read-only"), Registry.Find(TEXT("source_read"))->GetRisk() == ECursorToolRisk::ReadOnly);
	TestTrue(TEXT("source_search is read-only"), Registry.Find(TEXT("source_search"))->GetRisk() == ECursorToolRisk::ReadOnly);
	TestTrue(TEXT("source_write is dangerous"), Registry.Find(TEXT("source_write"))->GetRisk() == ECursorToolRisk::Dangerous);
	TestTrue(TEXT("cpp_build is dangerous"), Registry.Find(TEXT("cpp_build"))->GetRisk() == ECursorToolRisk::Dangerous);
	TestTrue(TEXT("cpp_create_class is offered to every caller"), Registry.Find(TEXT("cpp_create_class"))->GetAudience() == ECursorToolAudience::All);
	TestTrue(TEXT("cpp_build is offered to every caller"), Registry.Find(TEXT("cpp_build"))->GetAudience() == ECursorToolAudience::All);

	// ---- cpp_build: nothing here may start a real build -----------------------------------------------------------------
	FCursorToolResult R = CursorTest::Call(TEXT("cpp_build"), TEXT("{\"method\":\"warp\"}"));
	TestTrue(TEXT("an unknown build method is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("cpp_build"), TEXT("{\"dry_run\":true,\"target\":\"Bad Target; calc\"}"));
	TestTrue(TEXT("a target that is not a plain name is refused"), R.bIsError);
	R = CursorTest::Call(TEXT("cpp_build"), TEXT("{\"dry_run\":true,\"configuration\":\"Shipping\"}"));
	TestTrue(TEXT("only Development and DebugGame are accepted"), R.bIsError);
	R = CursorTest::Call(TEXT("cpp_build"), TEXT("{\"method\":\"ubt\",\"dry_run\":true}"));
	TestFalse(TEXT("a UBT dry run succeeds: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		TestTrue(TEXT("the dry run reports UBT"), R.Structured->GetStringField(TEXT("method")) == TEXT("ubt"));
		TestTrue(TEXT("the command names the editor target and the project"), R.Structured->GetStringField(TEXT("command")).Contains(TEXT("Editor")) && R.Structured->GetStringField(TEXT("command")).Contains(TEXT(".uproject")));
		TestTrue(TEXT("the command waits for the UBT mutex"), R.Structured->GetStringField(TEXT("command")).Contains(TEXT("-WaitMutex")));
	}
	R = CursorTest::Call(TEXT("cpp_build"), TEXT("{\"method\":\"live_coding\",\"dry_run\":true}"));
	if (R.bIsError)
	{
		TestTrue(TEXT("live_coding without Live Coding running explains how to enable it: ") + R.Text, R.Text.Contains(TEXT("Live Coding is not enabled")));
	}
	else
	{
		AddInfo(TEXT("Live Coding is running in this editor, so the live_coding dry run was accepted."));
	}
	{
		CursorTest::FScopedPolicy NoApproval; // default policy, nobody to ask
		R = CursorTest::Call(TEXT("cpp_build"), TEXT("{\"dry_run\":true}"));
		TestTrue(TEXT("cpp_build is refused without approval, even as a dry run: ") + R.Text, R.bIsError);
		R = CursorTest::Call(TEXT("source_write"), TEXT("{\"path\":\"Source/Anything.h\",\"content\":\"x\"}"));
		TestTrue(TEXT("source_write is refused without approval"), R.bIsError);
	}

	// ---- source tools without touching the project ----------------------------------------------------------------------
	R = CursorTest::Call(TEXT("source_read"), TEXT("{\"path\":\"Engine/Source/Runtime/Engine/Classes/GameFramework/Actor.h\",\"start_line\":1,\"max_lines\":30}"));
	TestFalse(TEXT("an engine header can be read: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		TestTrue(TEXT("lines are numbered"), R.Text.Contains(TEXT("     1|")));
		TestTrue(TEXT("the range is reported"), R.Structured->GetIntegerField(TEXT("end_line")) == 30 && R.Structured->GetBoolField(TEXT("truncated")));
	}
	R = CursorTest::Call(TEXT("source_read"), TEXT("{\"path\":\"Engine/Source/Runtime/Engine/Classes/GameFramework/Actor.h\",\"start_line\":99999999}"));
	TestTrue(TEXT("reading past the end is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("source_read"), TEXT("{\"path\":\"Config/DefaultEngine.ini\"}"));
	TestTrue(TEXT("the source tools do not read config files"), R.bIsError);
	R = CursorTest::Call(TEXT("source_read"), TEXT("{\"path\":\"Source/../Config/DefaultEngine.ini\"}"));
	TestTrue(TEXT("'..' cannot be used to leave Source"), R.bIsError);
	R = CursorTest::Call(TEXT("source_write"), TEXT("{\"path\":\"Engine/Source/Runtime/Core/Public/CursorUnrealWasHere.h\",\"content\":\"x\"}"));
	TestTrue(TEXT("engine source cannot be written"), R.bIsError);
	R = CursorTest::Call(TEXT("source_write"), TEXT("{\"path\":\"Source/Game/Door.h\"}"));
	TestTrue(TEXT("source_write needs content or an edit"), R.bIsError);
	R = CursorTest::Call(TEXT("source_write"), TEXT("{\"path\":\"Source/Game/Door.h\",\"content\":\"x\",\"old_text\":\"a\",\"new_text\":\"b\"}"));
	TestTrue(TEXT("source_write refuses content and an edit together"), R.bIsError);
	R = CursorTest::Call(TEXT("source_write"), TEXT("{\"path\":\"Source/NoSuchFolderAnywhere/Nothing.h\",\"old_text\":\"a\",\"new_text\":\"b\"}"));
	TestTrue(TEXT("editing a file that does not exist is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("source_search"), TEXT("{\"pattern\":\"x\",\"path\":\"Engine/Source\"}"));
	TestTrue(TEXT("searching all of the engine is refused: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("source_search"), TEXT("{\"pattern\":\"class AActor\",\"path\":\"Engine/Source/Runtime/Engine/Classes/GameFramework\",\"extensions\":[\"h\"],\"max_results\":5}"));
	TestTrue(TEXT("an engine folder can be searched: ") + R.Text, !R.bIsError && R.Structured->GetArrayField(TEXT("matches")).Num() >= 1);

	// ---- project files: skipped when the host project has no Source folder ----------------------------------------------
	if (!IFileManager::Get().DirectoryExists(*ProjectSourceDir()))
	{
		AddInfo(TEXT("The project has no Source folder; skipping the tests that write C++ files."));
		return true;
	}

	// Write, read, edit, back up. The file is a header no module includes, removed at the end.
	const FString Marker = UniqueName(TEXT("CursorUnrealTestMarker"));
	const FString RelPath = FString::Printf(TEXT("Source/%s.inl"), *Marker);
	const FString AbsPath = ProjectSourceDir() / (Marker + TEXT(".inl"));
	Cleanup.Files.Add(AbsPath);

	R = CursorTest::Call(TEXT("source_write"), *Args(TEXT("{\"path\":\"{0}\",\"content\":\"// {1}\\nint Alpha = 1;\\nint Beta = 2;\\nint Alpha2 = 1;\\n\"}"), RelPath, Marker));
	TestFalse(TEXT("create a source file: ") + R.Text, R.bIsError);
	TestTrue(TEXT("it reports creation and no backup"), !R.bIsError && R.Structured->GetBoolField(TEXT("created")) && !R.Structured->HasField(TEXT("backup")));
	TestTrue(TEXT("the file is on disk"), IFileManager::Get().FileExists(*AbsPath));

	R = CursorTest::Call(TEXT("source_read"), *Args(TEXT("{\"path\":\"{0}\"}"), RelPath));
	TestTrue(TEXT("the file reads back with line numbers: ") + R.Text, !R.bIsError && R.Text.Contains(TEXT("     2|int Alpha = 1;")) && R.Structured->GetIntegerField(TEXT("total_lines")) == 4);
	R = CursorTest::Call(TEXT("source_read"), *Args(TEXT("{\"path\":\"{0}\",\"start_line\":3,\"max_lines\":1}"), AbsPath));
	TestTrue(TEXT("an absolute path and a line range work: ") + R.Text, !R.bIsError && R.Text.Contains(TEXT("     3|int Beta = 2;")) && !R.Text.Contains(TEXT("Alpha =")));

	R = CursorTest::Call(TEXT("source_search"), *Args(TEXT("{\"pattern\":\"{0}\"}"), Marker));
	TestTrue(TEXT("source_search finds the marker in the project: ") + R.Text, !R.bIsError && R.Structured->GetArrayField(TEXT("matches")).Num() == 1);
	R = CursorTest::Call(TEXT("source_search"), *Args(TEXT("{\"pattern\":\"int Alpha\\\\d* = 1\",\"regex\":true,\"path\":\"{0}\"}"), RelPath));
	TestTrue(TEXT("a regex search finds both lines: ") + R.Text, !R.bIsError && R.Structured->GetArrayField(TEXT("matches")).Num() == 2);
	R = CursorTest::Call(TEXT("source_search"), *Args(TEXT("{\"pattern\":\"int alpha\",\"case_sensitive\":true,\"path\":\"{0}\"}"), RelPath));
	TestTrue(TEXT("case-sensitive search respects case"), !R.bIsError && R.Structured->GetArrayField(TEXT("matches")).Num() == 0);
	R = CursorTest::Call(TEXT("source_search"), *Args(TEXT("{\"pattern\":\"int alpha\",\"path\":\"{0}\"}"), RelPath));
	TestTrue(TEXT("default search ignores case"), !R.bIsError && R.Structured->GetArrayField(TEXT("matches")).Num() == 2);

	// An ambiguous edit changes nothing.
	R = CursorTest::Call(TEXT("source_write"), *Args(TEXT("{\"path\":\"{0}\",\"old_text\":\"= 1;\",\"new_text\":\"= 9;\"}"), RelPath));
	TestTrue(TEXT("an edit matching twice is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("2 times")));
	TestTrue(TEXT("and leaves the file alone"), ReadFile(AbsPath).Contains(TEXT("Alpha = 1")));
	R = CursorTest::Call(TEXT("source_write"), *Args(TEXT("{\"path\":\"{0}\",\"old_text\":\"not in the file\",\"new_text\":\"x\"}"), RelPath));
	TestTrue(TEXT("an edit that matches nothing is refused"), R.bIsError);

	R = CursorTest::Call(TEXT("source_write"), *Args(TEXT("{\"path\":\"{0}\",\"old_text\":\"int Beta = 2;\",\"new_text\":\"int Beta = 22;\"}"), RelPath));
	TestFalse(TEXT("a unique edit succeeds: ") + R.Text, R.bIsError);
	TestTrue(TEXT("the edit is applied"), ReadFile(AbsPath).Contains(TEXT("int Beta = 22;")));
	FString BackupPath;
	if (!R.bIsError && R.Structured->HasField(TEXT("backup")))
	{
		BackupPath = FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), R.Structured->GetStringField(TEXT("backup")));
		Cleanup.AddBackup(BackupPath);
		TestTrue(TEXT("the backup is under Saved/CursorUnreal/Backups"), R.Structured->GetStringField(TEXT("backup")).StartsWith(TEXT("Saved/CursorUnreal/Backups/")));
		TestTrue(TEXT("the backup holds the text from before the edit"), ReadFile(BackupPath).Contains(TEXT("int Beta = 2;")) && !ReadFile(BackupPath).Contains(TEXT("22")));
	}
	else
	{
		AddError(TEXT("An edit to an existing file did not report a backup."));
	}

	R = CursorTest::Call(TEXT("source_write"), *Args(TEXT("{\"path\":\"{0}\",\"old_text\":\"= 1;\",\"new_text\":\"= 7;\",\"replace_all\":true}"), RelPath));
	TestTrue(TEXT("replace_all changes every match: ") + R.Text, !R.bIsError && R.Structured->GetIntegerField(TEXT("replacements")) == 2 && !ReadFile(AbsPath).Contains(TEXT("= 1;")));
	if (!R.bIsError && R.Structured->HasField(TEXT("backup")))
	{
		Cleanup.AddBackup(FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), R.Structured->GetStringField(TEXT("backup"))));
	}

	R = CursorTest::Call(TEXT("source_write"), *Args(TEXT("{\"path\":\"{0}\",\"content\":\"// replaced\\n\"}"), RelPath));
	TestTrue(TEXT("overwriting a file is allowed and backed up: ") + R.Text, !R.bIsError && R.Structured->HasField(TEXT("backup")));
	if (!R.bIsError && R.Structured->HasField(TEXT("backup")))
	{
		Cleanup.AddBackup(FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), R.Structured->GetStringField(TEXT("backup"))));
	}
	TestEqual(TEXT("the overwrite replaced the contents"), ReadFile(AbsPath), FString(TEXT("// replaced\n")));

	R = CursorTest::Call(TEXT("source_write"), *Args(TEXT("{\"path\":\"Source/{0}.txt\",\"content\":\"x\"}"), Marker));
	TestTrue(TEXT("non-source files under Source are refused"), R.bIsError);
	R = CursorTest::Call(TEXT("source_write"), *Args(TEXT("{\"path\":\"Source/../{0}.h\",\"content\":\"x\"}"), Marker));
	TestTrue(TEXT("a '..' escape from Source is refused"), R.bIsError);
	TestFalse(TEXT("and nothing was created outside"), IFileManager::Get().FileExists(*(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()) / (Marker + TEXT(".h")))));

	// ---- cpp_create_class -----------------------------------------------------------------------------------------------
	const FString ClassName = UniqueName(TEXT("CUTestGen")).Replace(TEXT("-"), TEXT(""));
	R = CursorTest::Call(TEXT("cpp_create_class"), *Args(TEXT("{\"name\":\"{0}\",\"parent_class\":\"Actor\",\"dry_run\":true}"), ClassName));
	if (R.bIsError && R.Text.Contains(TEXT("no C++ modules")))
	{
		AddInfo(TEXT("The project has no C++ module; cpp_create_class was not exercised."));
		return true;
	}
	TestFalse(TEXT("a dry run succeeds: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		TestTrue(TEXT("the dry run returns the header"), R.Structured->GetStringField(TEXT("header_contents")).Contains(TEXT("A") + ClassName));
		TestTrue(TEXT("the dry run returns the source"), R.Structured->GetStringField(TEXT("source_contents")).Contains(TEXT("PrimaryActorTick")));
		TestFalse(TEXT("a dry run writes nothing"), IFileManager::Get().FileExists(*FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), R.Structured->GetStringField(TEXT("header")))));
	}

	R = CursorTest::Call(TEXT("cpp_create_class"), *Args(TEXT("{\"name\":\"{0}\",\"parent_class\":\"Actor\"}"), ClassName));
	TestFalse(TEXT("create a class: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		const FString Header = FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), R.Structured->GetStringField(TEXT("header")));
		const FString SourceFile = FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), R.Structured->GetStringField(TEXT("source")));
		Cleanup.Files.Add(Header);
		Cleanup.Files.Add(SourceFile);
		TestTrue(TEXT("the header exists"), IFileManager::Get().FileExists(*Header));
		TestTrue(TEXT("the source exists"), IFileManager::Get().FileExists(*SourceFile));
		TestTrue(TEXT("the header declares the class"), ReadFile(Header).Contains(FString::Printf(TEXT("class %s_API A%s : public AActor"), *R.Structured->GetStringField(TEXT("module")).ToUpper(), *ClassName)));
		TestTrue(TEXT("the source defines the constructor"), ReadFile(SourceFile).Contains(FString::Printf(TEXT("A%s::A%s()"), *ClassName, *ClassName)));
		TestTrue(TEXT("the files are inside the project's Source folder"), Header.StartsWith(ProjectSourceDir()) && SourceFile.StartsWith(ProjectSourceDir()));

		R = CursorTest::Call(TEXT("cpp_create_class"), *Args(TEXT("{\"name\":\"{0}\"}"), ClassName));
		TestTrue(TEXT("it never overwrites: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("already exists")));
	}

	R = CursorTest::Call(TEXT("cpp_create_class"), TEXT("{\"name\":\"1Bad\"}"));
	TestTrue(TEXT("an invalid class name is refused"), R.bIsError);
	R = CursorTest::Call(TEXT("cpp_create_class"), *Args(TEXT("{\"name\":\"{0}X\",\"module\":\"NoSuchModule\"}"), ClassName));
	TestTrue(TEXT("an unknown module is refused and the modules are listed: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("Modules:")));
	R = CursorTest::Call(TEXT("cpp_create_class"), *Args(TEXT("{\"name\":\"{0}X\",\"parent_class\":\"NoSuchParentAnywhere\"}"), ClassName));
	TestTrue(TEXT("an unknown parent is refused"), R.bIsError);
	R = CursorTest::Call(TEXT("cpp_create_class"), *Args(TEXT("{\"name\":\"{0}X\",\"subfolder\":\"../Escape\"}"), ClassName));
	TestTrue(TEXT("a subfolder cannot escape: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("cpp_create_class"), TEXT("{\"name\":\"Actor\"}"));
	TestTrue(TEXT("a name that is already a class is refused"), R.bIsError);
	R = CursorTest::Call(TEXT("cpp_create_class"), *Args(TEXT("{\"name\":\"{0}C\",\"parent_class\":\"ActorComponent\",\"dry_run\":true,\"subfolder\":\"Gameplay/Parts\"}"), ClassName));
	TestTrue(TEXT("a component parent and subfolder work: ") + R.Text, !R.bIsError && R.Structured->GetStringField(TEXT("source_contents")).Contains(TEXT("PrimaryComponentTick"))
		&& R.Structured->GetStringField(TEXT("header_contents")).Contains(TEXT("UActorComponent")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
