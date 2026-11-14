#include "CursorToolPacks.h"

#include "Async/Async.h"
#include "Components/ActorComponent.h"
#include "Containers/Ticker.h"
#include "CursorJson.h"
#include "CursorLogCapture.h"
#include "CursorPackSupport.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "HAL/FileManager.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Regex.h"
#include "Interfaces/IProjectManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/MonitoredProcess.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "ProjectDescriptor.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#if PLATFORM_WINDOWS
#include "ILiveCodingModule.h"
#endif

/*
 * C++ workflow pack.
 *  - cpp_create_class: writes a .h/.cpp pair into an existing project module.
 *  - cpp_build: Live Coding when it is running in this editor session, otherwise UnrealBuildTool, with compiler errors parsed.
 *  - source_read / source_search / source_write: NativeAgentOnly. Cursor's own agents already read and write files, so MCP and
 *    the Cursor bridge do not need them; the in-editor agent does. Writes are confined to Source/ folders and backed up first.
 */

namespace
{
	const TCHAR* CppPack = TEXT("cpp");

	constexpr int32 MaxSourceBytes = 5 * 1024 * 1024;
	constexpr int32 MaxWriteChars = 1000000;

	/** Path shown to the model: relative to the project when inside it, otherwise to the engine's parent folder. */
	FString DisplayPath(const FString& Absolute)
	{
		FString Relative = Absolute;
		if (FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())) && !Relative.StartsWith(TEXT("..")))
		{
			return Relative;
		}
		Relative = Absolute;
		if (FPaths::MakePathRelativeTo(Relative, *FPaths::ConvertRelativePathToFull(FPaths::RootDir())) && !Relative.StartsWith(TEXT("..")))
		{
			return Relative;
		}
		return Absolute;
	}

	/** Makes pasted text use the file's own line endings so a one-line edit does not rewrite every line in version control. */
	FString MatchLineEndings(const FString& Text, bool bCrLf)
	{
		FString Out = Text.Replace(TEXT("\r\n"), TEXT("\n"));
		if (bCrLf)
		{
			Out.ReplaceInline(TEXT("\n"), TEXT("\r\n"));
		}
		return Out;
	}

	TSharedRef<FJsonObject> MessageToJson(const CursorSource::FCompilerMessage& Message)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (!Message.File.IsEmpty())
		{
			Out->SetStringField(TEXT("file"), Message.File);
			Out->SetNumberField(TEXT("line"), Message.Line);
			if (Message.Column > 0)
			{
				Out->SetNumberField(TEXT("column"), Message.Column);
			}
		}
		if (!Message.Code.IsEmpty())
		{
			Out->SetStringField(TEXT("code"), Message.Code);
		}
		Out->SetStringField(TEXT("severity"), Message.Severity);
		Out->SetStringField(TEXT("text"), Message.Text);
		return Out;
	}

	/** Splits parsed diagnostics into the two arrays a tool result carries, capped so a build with hundreds of errors stays readable. */
	void AddDiagnostics(const TArray<CursorSource::FCompilerMessage>& Messages, const TSharedRef<FJsonObject>& Out, int32& OutErrors, int32& OutWarnings)
	{
		constexpr int32 MaxErrors = 40;
		constexpr int32 MaxWarnings = 10;
		TArray<TSharedPtr<FJsonValue>> Errors;
		TArray<TSharedPtr<FJsonValue>> Warnings;
		OutErrors = 0;
		OutWarnings = 0;
		for (const CursorSource::FCompilerMessage& Message : Messages)
		{
			if (Message.Severity == TEXT("error"))
			{
				++OutErrors;
				if (Errors.Num() < MaxErrors)
				{
					Errors.Add(MakeShared<FJsonValueObject>(MessageToJson(Message)));
				}
			}
			else
			{
				++OutWarnings;
				if (Warnings.Num() < MaxWarnings)
				{
					Warnings.Add(MakeShared<FJsonValueObject>(MessageToJson(Message)));
				}
			}
		}
		Out->SetNumberField(TEXT("error_count"), OutErrors);
		Out->SetNumberField(TEXT("warning_count"), OutWarnings);
		Out->SetArrayField(TEXT("errors"), Errors);
		Out->SetArrayField(TEXT("warnings"), Warnings);
	}

	FString TailLines(const FString& Text, int32 Count)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
		const int32 Start = FMath::Max(0, Lines.Num() - Count);
		TArray<FString> Tail;
		for (int32 i = Start; i < Lines.Num(); ++i)
		{
			Tail.Add(Lines[i]);
		}
		return FString::Join(Tail, TEXT("\n"));
	}

	// ---- cpp_create_class helpers --------------------------------------------------------------------------------------------

	/** Header an agent writes to include a native class: UE's own IncludePath metadata, falling back to ModuleRelativePath. */
	FString IncludeForClass(const UClass* Class)
	{
		FString Include = Class->GetMetaData(TEXT("IncludePath"));
		if (Include.IsEmpty())
		{
			Include = Class->GetMetaData(TEXT("ModuleRelativePath"));
			for (const TCHAR* Prefix : { TEXT("Public/"), TEXT("Classes/") })
			{
				if (Include.StartsWith(Prefix))
				{
					Include.RightChopInline(FCString::Strlen(Prefix));
					break;
				}
			}
		}
		return Include;
	}

	/** Name of the project module that matches the argument (case-insensitively), or the project's first module when none is given. */
	bool ResolveProjectModule(const FString& Requested, FString& OutModule, FString& OutError)
	{
		const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
		if (!Project || Project->Modules.Num() == 0)
		{
			OutError = TEXT("This project has no C++ modules. Add C++ to the project first (File > New C++ Class in the editor creates the first module).");
			return false;
		}
		TArray<FString> Names;
		for (const FModuleDescriptor& Module : Project->Modules)
		{
			Names.Add(Module.Name.ToString());
		}
		if (Requested.IsEmpty())
		{
			OutModule = Names[0];
			return true;
		}
		const int32 Index = CursorPackSupport::IndexOfName(Names, Requested);
		if (Index == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("'%s' is not a module of this project. Modules: %s"), *Requested, *FString::Join(Names, TEXT(", ")));
			return false;
		}
		OutModule = Names[Index];
		return true;
	}

	/** Subfolder inside Public/Private: letters, digits, underscores and slashes, no leading slash, no "..". */
	bool ValidateSubfolder(FString& Subfolder, FString& OutError)
	{
		Subfolder.ReplaceInline(TEXT("\\"), TEXT("/"));
		Subfolder.TrimStartAndEndInline();
		while (Subfolder.StartsWith(TEXT("/")))
		{
			Subfolder.RightChopInline(1);
		}
		while (Subfolder.EndsWith(TEXT("/")))
		{
			Subfolder.LeftChopInline(1);
		}
		for (const TCHAR Ch : Subfolder)
		{
			if (!FChar::IsAlnum(Ch) && Ch != TEXT('_') && Ch != TEXT('/'))
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid subfolder: use letters, digits, underscores and slashes."), *Subfolder);
				return false;
			}
		}
		if (Subfolder.Contains(TEXT("//")))
		{
			OutError = TEXT("The subfolder contains an empty path segment.");
			return false;
		}
		return true;
	}

	// ---- cpp_build helpers ---------------------------------------------------------------------------------------------------

	/** One running UnrealBuildTool process. Lives until it finishes or times out; the completion delegate is called once on the game thread. */
	struct FBuildRun : public TSharedFromThis<FBuildRun>
	{
		TSharedPtr<FMonitoredProcess> Process;
		FCriticalSection OutputLock;
		FString Output;
		FCursorToolCompletion Done;
		FString Command;
		FTSTicker::FDelegateHandle TimeoutHandle;
		double StartSeconds = 0.0;
		bool bFinished = false;
		bool bTimedOut = false;

		FString SnapshotOutput()
		{
			FScopeLock Lock(&OutputLock);
			return Output;
		}

		/** Game thread only. */
		void Finish(int32 ExitCode)
		{
			if (bFinished)
			{
				return;
			}
			bFinished = true;
			FTSTicker::GetCoreTicker().RemoveTicker(TimeoutHandle);

			const FString Text = SnapshotOutput();
			TArray<CursorSource::FCompilerMessage> Messages;
			CursorSource::ParseCompilerOutput(Text, Messages);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			int32 Errors = 0;
			int32 Warnings = 0;
			AddDiagnostics(Messages, Out, Errors, Warnings);
			const bool bSucceeded = ExitCode == 0 && !bTimedOut;
			Out->SetBoolField(TEXT("succeeded"), bSucceeded);
			Out->SetStringField(TEXT("method"), TEXT("ubt"));
			Out->SetNumberField(TEXT("exit_code"), ExitCode);
			Out->SetNumberField(TEXT("duration_seconds"), FPlatformTime::Seconds() - StartSeconds);
			if (bTimedOut)
			{
				Out->SetStringField(TEXT("note"), TEXT("The build ran past timeout_seconds and was stopped."));
			}
			if (!bSucceeded)
			{
				if (Text.Contains(TEXT("being used by another process")) || Text.Contains(TEXT("LNK1104")) || Text.Contains(TEXT("LNK1168")) || Text.Contains(TEXT("Live Coding")))
				{
					Out->SetStringField(TEXT("hint"), TEXT("The running editor is holding the build's output files. Enable Live Coding and use method=live_coding, or close the editor and build from a terminal."));
				}
				if (Errors == 0)
				{
					// UBT failed in a way the parser does not recognise: show the end of its output.
					Out->SetStringField(TEXT("output_tail"), TailLines(Text, 40));
				}
			}

			FCursorToolResult Result = FCursorToolResult::OkJson(Out);
			Result.bIsError = !bSucceeded;
			FCursorToolCompletion Callback = MoveTemp(Done);
			Done = nullptr;
			// Releasing the process here (game thread) also breaks the reference loop through its delegates.
			Process.Reset();
			if (Callback)
			{
				Callback(Result);
			}
		}
	};

	TWeakPtr<FBuildRun> ActiveBuild;

	/** Path of the build script that wraps UnrealBuildTool for this platform. */
	FString BuildScriptPath()
	{
#if PLATFORM_WINDOWS
		return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Build/BatchFiles/Build.bat"));
#elif PLATFORM_MAC
		return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Build/BatchFiles/Mac/Build.sh"));
#else
		return FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Build/BatchFiles/Linux/Build.sh"));
#endif
	}

	/** Program and arguments that run UBT for a target. */
	void BuildCommand(const FString& Target, const FString& Configuration, FString& OutProgram, FString& OutParams)
	{
		const FString Script = BuildScriptPath();
		const FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
		const FString Platform = FPlatformMisc::GetUBTPlatform();
#if PLATFORM_WINDOWS
		// cmd /c strips the outermost quotes, so the whole command line is wrapped in one more pair.
		OutProgram = TEXT("cmd.exe");
		OutParams = FString::Printf(TEXT("/c \"\"%s\" %s %s %s -Project=\"%s\" -WaitMutex -NoHotReloadFromIDE\""), *Script, *Target, *Platform, *Configuration, *Project);
#else
		OutProgram = TEXT("/bin/sh");
		OutParams = FString::Printf(TEXT("\"%s\" %s %s %s -Project=\"%s\" -WaitMutex -NoHotReloadFromIDE"), *Script, *Target, *Platform, *Configuration, *Project);
#endif
	}

	bool IsSafeToken(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Ch : Text)
		{
			if (!FChar::IsAlnum(Ch) && Ch != TEXT('_') && Ch != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}

#if PLATFORM_WINDOWS
	ILiveCodingModule* GetLiveCodingIfRunning()
	{
		ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME);
		return (LiveCoding && LiveCoding->IsEnabledForSession()) ? LiveCoding : nullptr;
	}
#endif

	bool IsLiveCodingRunning()
	{
#if PLATFORM_WINDOWS
		return GetLiveCodingIfRunning() != nullptr;
#else
		return false;
#endif
	}

	// ---- source_* helpers ----------------------------------------------------------------------------------------------------

	bool ReadSourceFile(const FString& Absolute, FString& OutText, FString& OutError)
	{
		const int64 Size = IFileManager::Get().FileSize(*Absolute);
		if (Size < 0)
		{
			OutError = FString::Printf(TEXT("No such file: %s"), *DisplayPath(Absolute));
			return false;
		}
		if (Size > MaxSourceBytes)
		{
			OutError = FString::Printf(TEXT("%s is %lld bytes, over the %d byte limit."), *DisplayPath(Absolute), Size, MaxSourceBytes);
			return false;
		}
		if (!FFileHelper::LoadFileToString(OutText, *Absolute))
		{
			OutError = FString::Printf(TEXT("Could not read %s."), *DisplayPath(Absolute));
			return false;
		}
		return true;
	}
}

void CursorToolPacks::RegisterCppPack()
{
	FCursorToolBuilder(TEXT("cpp_create_class"), CppPack)
		.Describe(TEXT("Create a new C++ class (a .h and .cpp pair) in one of the project's modules. Give the class name without the A/U prefix: parent Actor and name Door make ADoor in Door.h. "
			"Files go in the module's Public/ and Private/ folders (or next to each other in a module without them). Never overwrites. "
			"The class is not usable until it is compiled: call cpp_build next. Use dry_run to see the files without writing them."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("name"), TEXT("Class name without the A/U prefix, for example Door."), true)
			.Str(TEXT("parent_class"), TEXT("Native parent class: Actor, Pawn, Character, ActorComponent, SceneComponent, GameModeBase, Object... Default Actor."))
			.Str(TEXT("module"), TEXT("Project module to put it in. Default: the project's first module."))
			.Str(TEXT("subfolder"), TEXT("Folder inside Public/ and Private/, for example Gameplay or Gameplay/Doors."))
			.Bool(TEXT("blueprintable"), TEXT("Allow Blueprints to derive from it. Default true."))
			.Bool(TEXT("dry_run"), TEXT("Return the generated files and paths without writing anything.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			const FString Name = Args.Str(TEXT("name")).TrimStartAndEnd();
			if (!CursorSource::IsValidClassName(Name, Error))
			{
				return FCursorToolResult::Error(Error);
			}

			UClass* Parent = CursorToolHelpers::ResolveClass(Args.Str(TEXT("parent_class"), TEXT("Actor")), nullptr, Error);
			if (!Parent)
			{
				return FCursorToolResult::Error(Error);
			}
			if (!Parent->IsNative())
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("%s is a Blueprint class. A C++ class must derive from a native class; use blueprint_create to derive from a Blueprint."), *Parent->GetName()));
			}
			if (Parent->HasAnyClassFlags(CLASS_Final))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("%s is final and cannot be derived from."), *Parent->GetName()));
			}
			const FString ParentInclude = IncludeForClass(Parent);
			if (ParentInclude.IsEmpty())
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Could not work out which header declares %s."), *Parent->GetName()));
			}
			if (FindFirstObject<UClass>(*Name, EFindFirstObjectOptions::NativeFirst | EFindFirstObjectOptions::EnsureIfAmbiguous))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("A class named '%s' already exists. Pick another name."), *Name));
			}

			FString Module;
			FString Subfolder = Args.Str(TEXT("subfolder"));
			if (!ResolveProjectModule(Args.Str(TEXT("module")), Module, Error) || !ValidateSubfolder(Subfolder, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			const FString ModuleDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source") / Module);
			if (!IFileManager::Get().DirectoryExists(*ModuleDir))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Module '%s' has no folder at %s (modules from plugins are not supported by this tool)."), *Module, *DisplayPath(ModuleDir)));
			}

			// Public/ + Private/ when the module uses them, otherwise header and source sit together.
			const bool bSplit = IFileManager::Get().DirectoryExists(*(ModuleDir / TEXT("Public"))) && IFileManager::Get().DirectoryExists(*(ModuleDir / TEXT("Private")));
			const FString HeaderDir = (bSplit ? ModuleDir / TEXT("Public") : ModuleDir) / Subfolder;
			const FString SourceDir = (bSplit ? ModuleDir / TEXT("Private") : ModuleDir) / Subfolder;
			FString HeaderPath;
			FString SourcePath;
			if (!CursorSource::ResolveForWrite(HeaderDir / (Name + TEXT(".h")), HeaderPath, Error) || !CursorSource::ResolveForWrite(SourceDir / (Name + TEXT(".cpp")), SourcePath, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			for (const FString& Path : { HeaderPath, SourcePath })
			{
				if (IFileManager::Get().FileExists(*Path))
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("%s already exists; nothing was written."), *DisplayPath(Path)));
				}
			}

			CursorSource::FNewClassSpec Spec;
			Spec.Name = Name;
			Spec.ModuleName = Module;
			Spec.Prefix = Parent->GetPrefixCPP();
			Spec.ParentCppName = Spec.Prefix + Parent->GetName();
			Spec.ParentInclude = ParentInclude;
			Spec.OwnIncludePath = (Subfolder.IsEmpty() ? FString() : Subfolder + TEXT("/")) + Name + TEXT(".h");
			Spec.bIsActor = Parent->IsChildOf(AActor::StaticClass());
			Spec.bIsActorComponent = Parent->IsChildOf(UActorComponent::StaticClass());
			Spec.bBlueprintable = Args.Bool(TEXT("blueprintable"), true);
			const FString Header = CursorSource::GenerateHeader(Spec);
			const FString Source = CursorSource::GenerateSource(Spec);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("class"), Spec.Prefix + Name);
			Out->SetStringField(TEXT("module"), Module);
			Out->SetStringField(TEXT("header"), DisplayPath(HeaderPath));
			Out->SetStringField(TEXT("source"), DisplayPath(SourcePath));

			// A parent from another engine module needs that module in Build.cs, or the first build fails to link.
			FString BuildCs;
			const FString ParentModule = FPackageName::GetShortName(Parent->GetOutermost()->GetName());
			if (FFileHelper::LoadFileToString(BuildCs, *(ModuleDir / (Module + TEXT(".Build.cs"))))
				&& ParentModule != TEXT("CoreUObject") && ParentModule != TEXT("Core") && !BuildCs.Contains(FString::Printf(TEXT("\"%s\""), *ParentModule)))
			{
				Out->SetStringField(TEXT("warning"), FString::Printf(TEXT("%s.Build.cs does not mention the module '%s' that declares %s. Add it to PublicDependencyModuleNames if the build cannot find the header."),
					*Module, *ParentModule, *Parent->GetName()));
			}

			if (Args.Bool(TEXT("dry_run")))
			{
				Out->SetBoolField(TEXT("dry_run"), true);
				Out->SetStringField(TEXT("header_contents"), Header);
				Out->SetStringField(TEXT("source_contents"), Source);
				return FCursorToolResult::OkJson(Out);
			}

			const bool bWroteHeader = FFileHelper::SaveStringToFile(Header, *HeaderPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			const bool bWroteSource = bWroteHeader && FFileHelper::SaveStringToFile(Source, *SourcePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
			if (!bWroteSource)
			{
				// Do not leave a lone header that would fail the next build.
				IFileManager::Get().Delete(*HeaderPath);
				return FCursorToolResult::Error(TEXT("Could not write the files (read-only folder or source control lock). Nothing was created."));
			}
			Out->SetStringField(TEXT("next"), TEXT("Call cpp_build to compile it. New classes usually need a full build (or an editor restart) before they appear; Live Coding only patches existing code unless new-class support is on."));
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("cpp_build"), CppPack)
		.Describe(TEXT("Compile the project's C++ and return the compiler errors and warnings with file, line and message. "
			"method=auto uses Live Coding when it is running in this editor (patches the running editor, fast) and otherwise UnrealBuildTool. "
			"UnrealBuildTool cannot replace files the running editor has loaded: with the editor open, enable Live Coding (Ctrl+Alt+F11) first or expect a locked-file error. "
			"Builds can take minutes. Use dry_run to see which method and command would run."))
		.Risk(ECursorToolRisk::Dangerous)
		.Schema(FCursorSchema()
			.Enum(TEXT("method"), TEXT("auto (default), live_coding, or ubt."), { TEXT("auto"), TEXT("live_coding"), TEXT("ubt") })
			.Str(TEXT("target"), TEXT("UBT target. Default: <ProjectName>Editor."))
			.Enum(TEXT("configuration"), TEXT("Default Development."), { TEXT("Development"), TEXT("DebugGame") })
			.Int(TEXT("timeout_seconds"), TEXT("Stop a UBT build after this long. Default 1800."))
			.Bool(TEXT("dry_run"), TEXT("Report the method and command without building.")))
		.RunAsync([](const FCursorArgs& Args, const FCursorToolContext&, FCursorToolCompletion Done)
		{
			const FString Method = Args.Str(TEXT("method"), TEXT("auto")).ToLower();
			if (Method != TEXT("auto") && Method != TEXT("live_coding") && Method != TEXT("ubt"))
			{
				Done(FCursorToolResult::Error(FString::Printf(TEXT("Unknown method '%s'. Use auto, live_coding or ubt."), *Method)));
				return;
			}
			const bool bLiveCodingRunning = IsLiveCodingRunning();
			const bool bUseLiveCoding = Method == TEXT("live_coding") || (Method == TEXT("auto") && bLiveCodingRunning);
			if (bUseLiveCoding && !bLiveCodingRunning)
			{
				// Switching it on starts an external console process and changes the user's session, so the agent is not allowed to do it silently.
				Done(FCursorToolResult::Error(TEXT("Live Coding is not enabled in this editor session. Turn it on (Ctrl+Alt+F11, or Editor Preferences > Live Coding) and try again, or use method=ubt.")));
				return;
			}

			const FString Target = Args.Str(TEXT("target"), FString(FApp::GetProjectName()) + TEXT("Editor"));
			const FString Configuration = Args.Str(TEXT("configuration"), TEXT("Development"));
			if (!IsSafeToken(Target) || (Configuration != TEXT("Development") && Configuration != TEXT("DebugGame")))
			{
				Done(FCursorToolResult::Error(TEXT("'target' may only contain letters, digits, '_' and '-', and 'configuration' must be Development or DebugGame.")));
				return;
			}

			FString Program;
			FString Params;
			BuildCommand(Target, Configuration, Program, Params);

			if (Args.Bool(TEXT("dry_run")))
			{
				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				Out->SetBoolField(TEXT("dry_run"), true);
				Out->SetStringField(TEXT("method"), bUseLiveCoding ? TEXT("live_coding") : TEXT("ubt"));
				Out->SetBoolField(TEXT("live_coding_running"), bLiveCodingRunning);
				Out->SetStringField(TEXT("command"), Program + TEXT(" ") + Params);
				Done(FCursorToolResult::OkJson(Out));
				return;
			}

#if PLATFORM_WINDOWS
			if (bUseLiveCoding)
			{
				ILiveCodingModule* LiveCoding = GetLiveCodingIfRunning();
				if (LiveCoding->IsCompiling())
				{
					Done(FCursorToolResult::Error(TEXT("Live Coding is already compiling. Wait for it to finish.")));
					return;
				}
				const int64 LogMark = FCursorLogCapture::Get().GetLatestSequence();
				ELiveCodingCompileResult Result = ELiveCodingCompileResult::NotStarted;
				// Blocks the editor until the patch is built, the same as pressing Ctrl+Alt+F11 and waiting.
				LiveCoding->Compile(ELiveCodingCompileFlags::WaitForCompletion, &Result);

				FString Text;
				for (const FCursorLogLine& Line : FCursorLogCapture::Get().GetLines(LogMark, 2000, ELogVerbosity::All, TEXT("LiveCoding")))
				{
					Text += Line.Message + TEXT("\n");
				}
				TArray<CursorSource::FCompilerMessage> Messages;
				CursorSource::ParseCompilerOutput(Text, Messages);

				const TCHAR* ResultName = TEXT("failure");
				switch (Result)
				{
				case ELiveCodingCompileResult::Success: ResultName = TEXT("success"); break;
				case ELiveCodingCompileResult::NoChanges: ResultName = TEXT("no_changes"); break;
				case ELiveCodingCompileResult::InProgress: ResultName = TEXT("in_progress"); break;
				case ELiveCodingCompileResult::CompileStillActive: ResultName = TEXT("compile_still_active"); break;
				case ELiveCodingCompileResult::NotStarted: ResultName = TEXT("not_started"); break;
				case ELiveCodingCompileResult::Cancelled: ResultName = TEXT("cancelled"); break;
				default: break;
				}
				const bool bOk = Result == ELiveCodingCompileResult::Success || Result == ELiveCodingCompileResult::NoChanges;

				TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
				int32 Errors = 0;
				int32 Warnings = 0;
				AddDiagnostics(Messages, Out, Errors, Warnings);
				Out->SetBoolField(TEXT("succeeded"), bOk);
				Out->SetStringField(TEXT("method"), TEXT("live_coding"));
				Out->SetStringField(TEXT("result"), ResultName);
				if (!bOk && Errors == 0)
				{
					Out->SetStringField(TEXT("hint"), TEXT("Live Coding reports full compiler output in its own console window; it was not captured in the editor log."));
				}
				FCursorToolResult Reply = FCursorToolResult::OkJson(Out);
				Reply.bIsError = !bOk;
				Done(Reply);
				return;
			}
#endif

			if (const TSharedPtr<FBuildRun> Existing = ActiveBuild.Pin(); Existing.IsValid() && !Existing->bFinished)
			{
				Done(FCursorToolResult::Error(TEXT("A build is already running. Wait for it to finish.")));
				return;
			}
			if (!IFileManager::Get().FileExists(*BuildScriptPath()))
			{
				Done(FCursorToolResult::Error(FString::Printf(TEXT("Build script not found: %s"), *BuildScriptPath())));
				return;
			}
			if (FApp::IsProjectNameEmpty() || !FPaths::FileExists(FPaths::GetProjectFilePath()))
			{
				Done(FCursorToolResult::Error(TEXT("No project file is loaded, so there is nothing to build.")));
				return;
			}

			const TSharedRef<FBuildRun> Run = MakeShared<FBuildRun>();
			Run->Done = MoveTemp(Done);
			Run->Command = Program + TEXT(" ") + Params;
			Run->StartSeconds = FPlatformTime::Seconds();
			Run->Process = MakeShared<FMonitoredProcess>(Program, Params, /*bInHidden*/ true, /*bInCreatePipes*/ true);
			// Both delegates fire on the process's own thread; hop to the game thread before touching tool state.
			Run->Process->OnOutput().BindLambda([Run](const FString& Line)
			{
				FScopeLock Lock(&Run->OutputLock);
				Run->Output += Line + TEXT("\n");
			});
			Run->Process->OnCompleted().BindLambda([Run](int32 ExitCode)
			{
				AsyncTask(ENamedThreads::GameThread, [Run, ExitCode]() { Run->Finish(ExitCode); });
			});
			Run->Process->OnCanceled().BindLambda([Run]()
			{
				AsyncTask(ENamedThreads::GameThread, [Run]() { Run->Finish(-1); });
			});

			const double TimeoutSeconds = FMath::Clamp(Args.Int(TEXT("timeout_seconds"), 1800), 30, 7200);
			Run->TimeoutHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([WeakRun = TWeakPtr<FBuildRun>(Run)](float)
			{
				if (const TSharedPtr<FBuildRun> Pinned = WeakRun.Pin(); Pinned.IsValid() && !Pinned->bFinished)
				{
					Pinned->bTimedOut = true;
					if (Pinned->Process.IsValid())
					{
						Pinned->Process->Cancel(/*bKillTree*/ true);
					}
				}
				return false;
			}), static_cast<float>(TimeoutSeconds));

			ActiveBuild = Run;
			if (!Run->Process->Launch())
			{
				FCursorToolCompletion Callback = MoveTemp(Run->Done);
				Run->bFinished = true;
				FTSTicker::GetCoreTicker().RemoveTicker(Run->TimeoutHandle);
				Run->Process.Reset();
				Callback(FCursorToolResult::Error(TEXT("Could not start the build process.")));
			}
		})
		.Register();

	FCursorToolBuilder(TEXT("source_read"), CppPack)
		.Describe(TEXT("Read a C++ source file (.h .hpp .inl .cpp .cc .c .cs) with line numbers. Paths are relative to the project, for example Source/MyGame/Public/Door.h, "
			"or relative to the engine install, for example Engine/Source/Runtime/Engine/Classes/GameFramework/Actor.h. Reading engine headers is the quickest way to check an API."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Audience(ECursorToolAudience::NativeAgentOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("File path."), true)
			.Int(TEXT("start_line"), TEXT("First line to return, 1-based. Default 1."))
			.Int(TEXT("max_lines"), TEXT("Lines to return. Default 400, maximum 2000.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			FString Absolute;
			if (!CursorSource::ResolveForRead(Args.Str(TEXT("path")), Absolute, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			if (!CursorSource::IsSourceExtension(Absolute))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("'%s' is not a source file (.h .hpp .inl .cpp .cc .c .cs)."), *Args.Str(TEXT("path"))));
			}
			FString Text;
			if (!ReadSourceFile(Absolute, Text, Error))
			{
				return FCursorToolResult::Error(Error);
			}

			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines, /*bCullEmpty*/ false);
			const int32 Total = Lines.Num();
			const int32 Start = FMath::Max(1, Args.Int(TEXT("start_line"), 1));
			const int32 Max = FMath::Clamp(Args.Int(TEXT("max_lines"), 400), 1, 2000);
			if (Start > Total && Total > 0)
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("%s has only %d lines."), *DisplayPath(Absolute), Total));
			}
			const int32 End = FMath::Min(Total, Start + Max - 1);

			FString Body;
			for (int32 i = Start; i <= End; ++i)
			{
				Body += FString::Printf(TEXT("%6d|%s\n"), i, *Lines[i - 1]);
			}

			TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
			Info->SetStringField(TEXT("path"), DisplayPath(Absolute));
			Info->SetNumberField(TEXT("total_lines"), Total);
			Info->SetNumberField(TEXT("start_line"), Start);
			Info->SetNumberField(TEXT("end_line"), End);
			Info->SetBoolField(TEXT("truncated"), End < Total);

			FCursorToolResult Result = FCursorToolResult::Ok(
				FString::Printf(TEXT("%s (lines %d-%d of %d)\n%s"), *DisplayPath(Absolute), Start, End, Total, *Body));
			Result.Structured = Info;
			return Result;
		})
		.Register();

	FCursorToolBuilder(TEXT("source_search"), CppPack)
		.Describe(TEXT("Search C++ source for text or a regular expression, like grep. Defaults to the project's Source/ folder. "
			"Pass 'path' to search a plugin or engine folder, for example Engine/Source/Runtime/Engine/Classes (engine searches need a folder, not the whole engine). Returns file, line and the matching line."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Audience(ECursorToolAudience::NativeAgentOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("pattern"), TEXT("Text to find."), true)
			.Str(TEXT("path"), TEXT("Folder or file to search. Default: the project's Source/ folder."))
			.Bool(TEXT("regex"), TEXT("Treat the pattern as a regular expression. Default false."))
			.Bool(TEXT("case_sensitive"), TEXT("Default false."))
			.StrArray(TEXT("extensions"), TEXT("File extensions to include, without the dot. Default: h hpp inl cpp cc c cs."))
			.Int(TEXT("max_results"), TEXT("Default 50, maximum 300.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			const FString Pattern = Args.Str(TEXT("pattern"));
			if (Pattern.IsEmpty() || Pattern.Len() > 500)
			{
				return FCursorToolResult::Error(TEXT("'pattern' is required and may be at most 500 characters."));
			}

			FString Root;
			if (Args.Has(TEXT("path")))
			{
				if (!CursorSource::ResolveForRead(Args.Str(TEXT("path")), Root, Error))
				{
					return FCursorToolResult::Error(Error);
				}
			}
			else
			{
				Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Source"));
				FPaths::NormalizeDirectoryName(Root);
			}
			const bool bIsFile = IFileManager::Get().FileExists(*Root);
			if (!bIsFile && !IFileManager::Get().DirectoryExists(*Root))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Nothing at '%s'."), *DisplayPath(Root)));
			}
			// An unbounded walk of the engine would take minutes. Ask for a folder.
			const FString EngineSource = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Source"));
			const FString EnginePlugins = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Plugins"));
			FString NormalisedRoot = Root;
			FPaths::NormalizeDirectoryName(NormalisedRoot);
			if (NormalisedRoot.Equals(EngineSource, ESearchCase::IgnoreCase) || NormalisedRoot.Equals(EnginePlugins, ESearchCase::IgnoreCase))
			{
				return FCursorToolResult::Error(TEXT("Searching all of the engine is too slow. Pass a folder, for example Engine/Source/Runtime/Engine/Classes."));
			}

			TArray<FString> Extensions;
			for (const FString& Ext : Args.StrArray(TEXT("extensions")))
			{
				Extensions.Add(Ext.Replace(TEXT("."), TEXT("")).ToLower());
			}
			auto WantsFile = [&](const FString& Path)
			{
				return Extensions.Num() > 0 ? Extensions.Contains(FPaths::GetExtension(Path).ToLower()) : CursorSource::IsSourceExtension(Path);
			};

			TArray<FString> Files;
			if (bIsFile)
			{
				Files.Add(Root);
			}
			else
			{
				IFileManager::Get().IterateDirectoryRecursively(*Root, [&](const TCHAR* Path, bool bIsDirectory)
				{
					if (!bIsDirectory && WantsFile(Path))
					{
						Files.Add(FString(Path));
					}
					return Files.Num() < 100000;
				});
				Files.Sort();
			}

			const bool bRegex = Args.Bool(TEXT("regex"));
			const bool bCaseSensitive = Args.Bool(TEXT("case_sensitive"));
			TUniquePtr<FRegexPattern> RegexPattern;
			if (bRegex)
			{
				RegexPattern = MakeUnique<FRegexPattern>(Pattern, bCaseSensitive ? ERegexPatternFlags::None : ERegexPatternFlags::CaseInsensitive);
			}
			const int32 MaxResults = FMath::Clamp(Args.Int(TEXT("max_results"), 50), 1, 300);

			TArray<TSharedPtr<FJsonValue>> Matches;
			bool bTruncated = false;
			bool bTimedOut = false;
			int32 Searched = 0;
			const double Deadline = FPlatformTime::Seconds() + 15.0;
			for (const FString& File : Files)
			{
				if (FPlatformTime::Seconds() > Deadline)
				{
					bTimedOut = true;
					break;
				}
				if (IFileManager::Get().FileSize(*File) > MaxSourceBytes)
				{
					continue;
				}
				TArray<FString> Lines;
				if (!FFileHelper::LoadFileToStringArray(Lines, *File))
				{
					continue;
				}
				++Searched;
				for (int32 i = 0; i < Lines.Num(); ++i)
				{
					bool bHit = false;
					if (bRegex)
					{
						FRegexMatcher Matcher(*RegexPattern, Lines[i]);
						bHit = Matcher.FindNext();
					}
					else
					{
						bHit = Lines[i].Contains(Pattern, bCaseSensitive ? ESearchCase::CaseSensitive : ESearchCase::IgnoreCase);
					}
					if (!bHit)
					{
						continue;
					}
					if (Matches.Num() >= MaxResults)
					{
						bTruncated = true;
						break;
					}
					TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
					Item->SetStringField(TEXT("file"), DisplayPath(File));
					Item->SetNumberField(TEXT("line"), i + 1);
					Item->SetStringField(TEXT("text"), Lines[i].TrimStartAndEnd().Left(300));
					Matches.Add(MakeShared<FJsonValueObject>(Item));
				}
				if (bTruncated)
				{
					break;
				}
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetNumberField(TEXT("files_searched"), Searched);
			Out->SetArrayField(TEXT("matches"), Matches);
			if (bTruncated)
			{
				Out->SetStringField(TEXT("note"), TEXT("More matches exist. Narrow the pattern or path, or raise max_results."));
			}
			if (bTimedOut)
			{
				Out->SetStringField(TEXT("note"), TEXT("The search hit its 15 second limit and is incomplete. Search a smaller folder."));
			}
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("source_write"), CppPack)
		.Describe(TEXT("Create or edit a C++ source file (.h .hpp .inl .cpp .cc .c .cs). Only the project's Source/ folder and the Source/ folder of its own plugins can be written. "
			"Either pass 'content' to write the whole file (creating it if new), or 'old_text' and 'new_text' to replace a snippet that must appear exactly once. "
			"An existing file is copied to Saved/CursorUnreal/Backups first. Asks the user for approval. Compile with cpp_build afterwards."))
		.Risk(ECursorToolRisk::Dangerous)
		.Audience(ECursorToolAudience::NativeAgentOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("File path under Source/, for example Source/MyGame/Public/Door.h."), true)
			.Str(TEXT("content"), TEXT("Full new contents of the file."))
			.Str(TEXT("old_text"), TEXT("Snippet to replace. Must match exactly once, including whitespace."))
			.Str(TEXT("new_text"), TEXT("Replacement for old_text."))
			.Bool(TEXT("replace_all"), TEXT("Replace every occurrence of old_text instead of requiring exactly one.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			FString Error;
			FString Absolute;
			if (!CursorSource::ResolveForWrite(Args.Str(TEXT("path")), Absolute, Error))
			{
				return FCursorToolResult::Error(Error);
			}

			const bool bFull = Args.Has(TEXT("content"));
			const bool bEdit = Args.Has(TEXT("old_text"));
			if (bFull == bEdit)
			{
				return FCursorToolResult::Error(TEXT("Pass either 'content' (whole file) or 'old_text' with 'new_text' (one edit), not both and not neither."));
			}

			const bool bExists = IFileManager::Get().FileExists(*Absolute);
			FString Existing;
			if (bExists && !ReadSourceFile(Absolute, Existing, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			const bool bCrLf = Existing.Contains(TEXT("\r\n"));

			FString NewText;
			int32 Replacements = 0;
			if (bFull)
			{
				NewText = Args.Str(TEXT("content"));
				if (NewText.Len() > MaxWriteChars)
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("'content' is over %d characters."), MaxWriteChars));
				}
				NewText = bExists ? MatchLineEndings(NewText, bCrLf) : NewText;
			}
			else
			{
				if (!bExists)
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("%s does not exist, so there is nothing to edit. Pass 'content' to create it."), *DisplayPath(Absolute)));
				}
				const FString Old = MatchLineEndings(Args.Str(TEXT("old_text")), bCrLf);
				const FString Replacement = MatchLineEndings(Args.Str(TEXT("new_text")), bCrLf);
				if (Old.IsEmpty())
				{
					return FCursorToolResult::Error(TEXT("'old_text' is empty."));
				}
				int32 Pos = 0;
				while ((Pos = Existing.Find(Old, ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos)) != INDEX_NONE)
				{
					++Replacements;
					Pos += Old.Len();
				}
				if (Replacements == 0)
				{
					return FCursorToolResult::Error(TEXT("'old_text' was not found. Read the file again with source_read; it must match exactly, including indentation."));
				}
				if (Replacements > 1 && !Args.Bool(TEXT("replace_all")))
				{
					return FCursorToolResult::Error(FString::Printf(TEXT("'old_text' appears %d times. Add surrounding lines to make it unique, or set replace_all."), Replacements));
				}
				NewText = Existing.Replace(*Old, *Replacement, ESearchCase::CaseSensitive);
				if (NewText.Len() > MaxWriteChars * 5)
				{
					return FCursorToolResult::Error(TEXT("The edited file would be too large."));
				}
			}

			// The backup comes first: if it fails, the original is untouched.
			FString Backup = CursorSource::BackupFile(Absolute, FPaths::ProjectDir(), CursorSource::DefaultBackupRoot(), Error);
			if (bExists && Backup.IsEmpty())
			{
				return FCursorToolResult::Error(Error);
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(Absolute), /*Tree*/ true);
			if (!FFileHelper::SaveStringToFile(NewText, *Absolute, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("Could not write %s (read-only, or locked by source control or an IDE)."), *DisplayPath(Absolute)));
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), DisplayPath(Absolute));
			Out->SetBoolField(TEXT("created"), !bExists);
			Out->SetNumberField(TEXT("chars"), NewText.Len());
			if (bEdit)
			{
				Out->SetNumberField(TEXT("replacements"), Replacements);
			}
			if (!Backup.IsEmpty())
			{
				Out->SetStringField(TEXT("backup"), DisplayPath(Backup));
			}
			return FCursorToolResult::OkJson(Out);
		})
		.Register();
}

void CursorToolPacks::UnregisterCppPack()
{
	FCursorToolRegistry::Get().UnregisterPack(CppPack);
}
