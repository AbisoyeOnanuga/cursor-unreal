#include "CursorPackSupport.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CursorToolHelpers.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Misc/DateTime.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace CursorPackSupport
{
	bool IsWritableContentPath(const FString& PackagePath, FString& OutError)
	{
		if (PackagePath.StartsWith(TEXT("/Engine/")) || PackagePath.StartsWith(TEXT("/Script/")) || PackagePath.StartsWith(TEXT("/Temp/")))
		{
			OutError = FString::Printf(TEXT("'%s' is not project content and will not be changed."), *PackagePath);
			return false;
		}
		FText Reason;
		if (!FPackageName::IsValidLongPackageName(PackagePath, false, &Reason))
		{
			OutError = FString::Printf(TEXT("'%s' is not a valid asset path: %s"), *PackagePath, *Reason.ToString());
			return false;
		}
		return true;
	}

	bool AssetExists(const FString& PackagePath)
	{
		TArray<FAssetData> InPackage;
		IAssetRegistry::GetChecked().GetAssetsByPackageName(FName(*PackagePath), InPackage);
		if (InPackage.Num() > 0 || FPackageName::DoesPackageExist(PackagePath))
		{
			return true;
		}
		// Created earlier in this session and not saved yet.
		if (const UPackage* Loaded = FindPackage(nullptr, *PackagePath))
		{
			return FindObject<UObject>(const_cast<UPackage*>(Loaded), *FPackageName::GetShortName(PackagePath)) != nullptr;
		}
		return false;
	}

	bool PrepareNewAsset(const FString& AssetPath, UPackage*& OutPackage, FString& OutAssetName, FString& OutError)
	{
		const FString PackagePath = CursorToolHelpers::ToPackagePath(AssetPath);
		if (!IsWritableContentPath(PackagePath, OutError))
		{
			return false;
		}
		if (AssetExists(PackagePath))
		{
			OutError = FString::Printf(TEXT("'%s' already exists. Pick another name, or edit the existing asset."), *PackagePath);
			return false;
		}
		OutAssetName = FPackageName::GetShortName(PackagePath);
		OutPackage = CreatePackage(*PackagePath);
		if (!OutPackage)
		{
			OutError = FString::Printf(TEXT("Could not create a package for '%s'."), *PackagePath);
			return false;
		}
		OutPackage->FullyLoad();
		return true;
	}

	void FinishNewAsset(UObject* Asset)
	{
		if (!Asset)
		{
			return;
		}
		FAssetRegistryModule::AssetCreated(Asset);
		Asset->MarkPackageDirty();
	}

	int32 IndexOfName(const TArray<FString>& Names, const FString& Wanted)
	{
		return Names.IndexOfByPredicate([&](const FString& Name) { return Name.Equals(Wanted, ESearchCase::IgnoreCase); });
	}
}

// Public forwards, so tool packs in other modules (the Niagara pack) create assets under the same rules.
namespace CursorToolHelpers
{
	bool IsWritableContentPath(const FString& PackagePath, FString& OutError)
	{
		return CursorPackSupport::IsWritableContentPath(PackagePath, OutError);
	}

	bool PrepareNewAsset(const FString& AssetPath, UPackage*& OutPackage, FString& OutAssetName, FString& OutError)
	{
		return CursorPackSupport::PrepareNewAsset(AssetPath, OutPackage, OutAssetName, OutError);
	}

	void FinishNewAsset(UObject* Asset)
	{
		CursorPackSupport::FinishNewAsset(Asset);
	}
}

namespace CursorSource
{
	namespace
	{
		FString NormalisedDir(const FString& Dir)
		{
			FString Out = FPaths::ConvertRelativePathToFull(Dir);
			FPaths::NormalizeDirectoryName(Out);
			FPaths::CollapseRelativeDirectories(Out);
			return Out;
		}

		/** Name of this plugin. source_write refuses to edit it so an agent cannot rewrite its own tools mid-session. */
		const TCHAR* OwnPluginName = TEXT("CursorUnreal");

		void AddProjectPluginRoots(TArray<FString>& Roots, bool bIncludeOwnPlugin, bool bSourceOnly)
		{
			for (const TSharedRef<IPlugin>& Plugin : IPluginManager::Get().GetEnabledPlugins())
			{
				if (Plugin->GetLoadedFrom() != EPluginLoadedFrom::Project)
				{
					continue;
				}
				if (!bIncludeOwnPlugin && Plugin->GetName() == OwnPluginName)
				{
					continue;
				}
				Roots.AddUnique(NormalisedDir(bSourceOnly ? Plugin->GetBaseDir() / TEXT("Source") : Plugin->GetBaseDir()));
			}
		}

		/** Timestamp that sorts by time and is unique per call, so two writes in one second keep separate backups. */
		FString BackupStamp()
		{
			const FDateTime Now = FDateTime::Now();
			return FString::Printf(TEXT("%s-%03d"), *Now.ToString(TEXT("%Y%m%d-%H%M%S")), Now.GetMillisecond());
		}
	}

	bool IsSourceExtension(const FString& Path)
	{
		static const TCHAR* Allowed[] = { TEXT("h"), TEXT("hpp"), TEXT("inl"), TEXT("cpp"), TEXT("cc"), TEXT("c"), TEXT("cs") };
		const FString Ext = FPaths::GetExtension(Path).ToLower();
		for (const TCHAR* Candidate : Allowed)
		{
			if (Ext == Candidate)
			{
				return true;
			}
		}
		return false;
	}

	bool ResolveWithinRoots(const FString& Path, const FString& RelativeBase, const TArray<FString>& AllowedRoots, FString& OutAbsolute, FString& OutError)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			OutError = TEXT("The path is empty.");
			return false;
		}
		// The base is made absolute first: for a project on the same drive as the engine, FPaths::ProjectDir() is
		// relative ("../../../Users/..."), and joining onto that leaves '..' that cannot be collapsed, so every
		// project-relative path would be refused as climbing out of its folder.
		FString Full = FPaths::IsRelative(Trimmed) ? FPaths::ConvertRelativePathToFull(FPaths::ConvertRelativePathToFull(RelativeBase), Trimmed) : FPaths::ConvertRelativePathToFull(Trimmed);
		FPaths::NormalizeFilename(Full);
		if (!FPaths::CollapseRelativeDirectories(Full))
		{
			OutError = FString::Printf(TEXT("'%s' climbs out of its folder with '..' and is not allowed."), *Path);
			return false;
		}
		for (const FString& Root : AllowedRoots)
		{
			if (Full.StartsWith(Root + TEXT("/"), ESearchCase::IgnoreCase))
			{
				OutAbsolute = Full;
				return true;
			}
		}
		OutError = FString::Printf(TEXT("'%s' is outside the folders this tool may use."), *Path);
		return false;
	}

	TArray<FString> WritableRoots()
	{
		TArray<FString> Roots;
		Roots.Add(NormalisedDir(FPaths::ProjectDir() / TEXT("Source")));
		AddProjectPluginRoots(Roots, /*bIncludeOwnPlugin*/ false, /*bSourceOnly*/ true);
		return Roots;
	}

	TArray<FString> ReadableRoots()
	{
		TArray<FString> Roots = WritableRoots();
		AddProjectPluginRoots(Roots, /*bIncludeOwnPlugin*/ true, /*bSourceOnly*/ true);
		Roots.AddUnique(NormalisedDir(FPaths::EngineDir() / TEXT("Source")));
		Roots.AddUnique(NormalisedDir(FPaths::EngineDir() / TEXT("Plugins")));
		return Roots;
	}

	bool ResolveForRead(const FString& Path, FString& OutAbsolute, FString& OutError)
	{
		const TArray<FString> Roots = ReadableRoots();
		// "Source/MyGame/Foo.h" is relative to the project; "Engine/Source/Runtime/..." is relative to the engine install.
		if (FPaths::IsRelative(Path))
		{
			FString Candidate;
			FString Ignored;
			if (ResolveWithinRoots(Path, FPaths::ProjectDir(), Roots, Candidate, Ignored) && (IFileManager::Get().FileExists(*Candidate) || IFileManager::Get().DirectoryExists(*Candidate)))
			{
				OutAbsolute = Candidate;
				return true;
			}
			if (ResolveWithinRoots(Path, FPaths::RootDir(), Roots, Candidate, Ignored) && (IFileManager::Get().FileExists(*Candidate) || IFileManager::Get().DirectoryExists(*Candidate)))
			{
				OutAbsolute = Candidate;
				return true;
			}
		}
		return ResolveWithinRoots(Path, FPaths::ProjectDir(), Roots, OutAbsolute, OutError);
	}

	bool ResolveForWrite(const FString& Path, FString& OutAbsolute, FString& OutError)
	{
		if (!ResolveWithinRoots(Path, FPaths::ProjectDir(), WritableRoots(), OutAbsolute, OutError))
		{
			OutError += TEXT(" Source files can only be written under the project's Source/ folder or the Source/ folder of one of its plugins.");
			return false;
		}
		if (!IsSourceExtension(OutAbsolute))
		{
			OutError = FString::Printf(TEXT("'%s' is not a source file (.h .hpp .inl .cpp .cc .c .cs)."), *Path);
			return false;
		}
		return true;
	}

	FString DefaultBackupRoot()
	{
		return NormalisedDir(FPaths::ProjectSavedDir() / TEXT("CursorUnreal") / TEXT("Backups"));
	}

	FString BackupFile(const FString& AbsoluteFile, const FString& RelativeTo, const FString& BackupRoot, FString& OutError)
	{
		if (!IFileManager::Get().FileExists(*AbsoluteFile))
		{
			return FString();
		}
		FString Relative = AbsoluteFile;
		if (!FPaths::MakePathRelativeTo(Relative, *(NormalisedDir(RelativeTo) + TEXT("/"))) || Relative.StartsWith(TEXT("..")))
		{
			// Not under the base (cannot happen for writable roots, but never write a backup outside the backup folder).
			Relative = FPaths::GetCleanFilename(AbsoluteFile);
		}
		const FString Destination = BackupRoot / BackupStamp() / Relative;
		if (IFileManager::Get().Copy(*Destination, *AbsoluteFile, /*bReplaceExisting*/ true, /*bEvenIfReadOnly*/ true) != COPY_OK)
		{
			OutError = FString::Printf(TEXT("Could not back up '%s' to '%s'. Nothing was written."), *AbsoluteFile, *Destination);
			return FString();
		}
		return Destination;
	}

	void ParseCompilerOutput(const FString& Output, TArray<FCompilerMessage>& OutMessages)
	{
		// MSVC:   D:\Proj\Source\Foo.cpp(12,5): error C2065: 'x': undeclared identifier
		static const FRegexPattern Msvc(TEXT("^\\s*(.+?)\\((\\d+)(?:,(\\d+))?\\)\\s*:\\s*(?:fatal )?(error|warning)\\s+([A-Za-z]+\\d+)\\s*:\\s*(.*)$"));
		// clang:  /proj/Source/Foo.cpp:12:5: error: use of undeclared identifier 'x'
		static const FRegexPattern Clang(TEXT("^\\s*(.+?):(\\d+):(\\d+):\\s*(?:fatal )?(error|warning):\\s*(.*)$"));
		// Linker and driver: LINK : fatal error LNK1104: cannot open file 'x.lib'
		static const FRegexPattern Linker(TEXT("^\\s*(?:.*?:\\s*)?(?:fatal )?(error)\\s+([A-Za-z]+\\d+)\\s*:\\s*(.*)$"));
		// UnrealBuildTool's own failures: ERROR: Unable to instantiate module 'X'
		static const FRegexPattern Ubt(TEXT("^\\s*ERROR:\\s*(.*)$"));

		TSet<FString> Seen;
		TArray<FString> Lines;
		Output.ParseIntoArrayLines(Lines, /*bCullEmpty*/ true);
		for (const FString& Line : Lines)
		{
			FCompilerMessage Message;
			bool bMatched = false;
			if (FRegexMatcher Matcher(Msvc, Line); Matcher.FindNext())
			{
				Message.File = Matcher.GetCaptureGroup(1);
				Message.Line = FCString::Atoi(*Matcher.GetCaptureGroup(2));
				Message.Column = FCString::Atoi(*Matcher.GetCaptureGroup(3));
				Message.Severity = Matcher.GetCaptureGroup(4);
				Message.Code = Matcher.GetCaptureGroup(5);
				Message.Text = Matcher.GetCaptureGroup(6).TrimEnd();
				bMatched = true;
			}
			else if (FRegexMatcher ClangMatcher(Clang, Line); ClangMatcher.FindNext())
			{
				Message.File = ClangMatcher.GetCaptureGroup(1);
				Message.Line = FCString::Atoi(*ClangMatcher.GetCaptureGroup(2));
				Message.Column = FCString::Atoi(*ClangMatcher.GetCaptureGroup(3));
				Message.Severity = ClangMatcher.GetCaptureGroup(4);
				Message.Text = ClangMatcher.GetCaptureGroup(5).TrimEnd();
				bMatched = true;
			}
			else if (FRegexMatcher LinkMatcher(Linker, Line); LinkMatcher.FindNext())
			{
				Message.Severity = LinkMatcher.GetCaptureGroup(1);
				Message.Code = LinkMatcher.GetCaptureGroup(2);
				Message.Text = LinkMatcher.GetCaptureGroup(3).TrimEnd();
				bMatched = true;
			}
			else if (FRegexMatcher UbtMatcher(Ubt, Line); UbtMatcher.FindNext())
			{
				Message.Severity = TEXT("error");
				Message.Text = UbtMatcher.GetCaptureGroup(1).TrimEnd();
				bMatched = true;
			}
			if (!bMatched)
			{
				continue;
			}

			bool bAlreadyReported = false;
			Seen.Add(FString::Printf(TEXT("%s|%d|%s|%s"), *Message.File, Message.Line, *Message.Code, *Message.Text), &bAlreadyReported);
			if (!bAlreadyReported)
			{
				OutMessages.Add(MoveTemp(Message));
			}
		}
	}
	bool IsValidClassName(const FString& Name, FString& OutError)
	{
		if (Name.IsEmpty() || Name.Len() > 64 || !FChar::IsAlpha(Name[0]))
		{
			OutError = TEXT("Class names must start with a letter and be at most 64 characters.");
			return false;
		}
		for (const TCHAR Ch : Name)
		{
			if (!FChar::IsAlnum(Ch) && Ch != TEXT('_'))
			{
				OutError = FString::Printf(TEXT("'%s' is not a valid class name: only letters, digits and underscores are allowed."), *Name);
				return false;
			}
		}
		if (Name.Contains(TEXT("__")))
		{
			// Double underscores are reserved identifiers in C++.
			OutError = TEXT("Class names may not contain a double underscore.");
			return false;
		}
		return true;
	}

	FString GenerateHeader(const FNewClassSpec& Spec)
	{
		FString Out;
		Out += TEXT("// Created by CursorUnreal (cpp_create_class).\n");
		Out += TEXT("\n#pragma once\n\n");
		Out += TEXT("#include \"CoreMinimal.h\"\n");
		Out += FString::Printf(TEXT("#include \"%s\"\n"), *Spec.ParentInclude);
		Out += FString::Printf(TEXT("#include \"%s.generated.h\"\n\n"), *Spec.Name);
		Out += Spec.bBlueprintable ? TEXT("UCLASS(Blueprintable)\n") : TEXT("UCLASS()\n");
		Out += FString::Printf(TEXT("class %s_API %s%s : public %s\n{\n"), *Spec.ModuleName.ToUpper(), *Spec.Prefix, *Spec.Name, *Spec.ParentCppName);
		Out += TEXT("\tGENERATED_BODY()\n\npublic:\n");
		Out += FString::Printf(TEXT("\t%s%s();\n"), *Spec.Prefix, *Spec.Name);
		Out += TEXT("};\n");
		return Out;
	}

	FString GenerateSource(const FNewClassSpec& Spec)
	{
		FString Out;
		Out += FString::Printf(TEXT("#include \"%s\"\n\n"), *Spec.OwnIncludePath);
		Out += FString::Printf(TEXT("%s%s::%s%s()\n{\n"), *Spec.Prefix, *Spec.Name, *Spec.Prefix, *Spec.Name);
		if (Spec.bIsActor)
		{
			Out += TEXT("\t// Turn this off if the actor does not need Tick, to save per-frame cost.\n");
			Out += TEXT("\tPrimaryActorTick.bCanEverTick = true;\n");
		}
		else if (Spec.bIsActorComponent)
		{
			Out += TEXT("\t// Turn this off if the component does not need Tick, to save per-frame cost.\n");
			Out += TEXT("\tPrimaryComponentTick.bCanEverTick = true;\n");
		}
		Out += TEXT("}\n");
		return Out;
	}
}
