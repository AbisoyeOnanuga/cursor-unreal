#include "CursorSkillLibrary.h"

#include "CursorJson.h"
#include "CursorUnrealLog.h"
#include "CursorUnrealSettings.h"
#include "DirectoryWatcherModule.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "IDirectoryWatcher.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Modules/ModuleManager.h"

const TCHAR* LexToString(ECursorSkillLayer Layer)
{
	switch (Layer)
	{
	case ECursorSkillLayer::Project: return TEXT("project");
	case ECursorSkillLayer::User: return TEXT("user");
	default: return TEXT("built-in");
	}
}

namespace
{
	const TCHAR* SkillFileName = TEXT("SKILL.md");
	const TCHAR* ManifestFileName = TEXT(".cursorunreal-manifest.json");

	FString NormalizeDir(const FString& Dir)
	{
		FString Full = FPaths::ConvertRelativePathToFull(Dir);
		FPaths::NormalizeDirectoryName(Full);
		return Full;
	}

	FString UserHome()
	{
		FString Home = FPlatformMisc::GetEnvironmentVariable(PLATFORM_WINDOWS ? TEXT("USERPROFILE") : TEXT("HOME"));
		if (Home.IsEmpty())
		{
			Home = FPlatformProcess::UserHomeDir();
		}
		return Home;
	}

	/** Hash of a file's text with line endings normalised, so a CRLF checkout does not look like an edit. */
	FString HashFile(const FString& Path)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return FString();
		}
		Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
		const FTCHARToUTF8 Utf8(*Text);
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
		return BytesToHex(Digest, FSHA1::DigestSize);
	}

	TMap<FString, FString> LoadManifest(const FString& Path)
	{
		TMap<FString, FString> Out;
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *Path))
		{
			if (const TSharedPtr<FJsonObject> Root = CursorJson::Parse(Text))
			{
				const TSharedPtr<FJsonObject>* Files = nullptr;
				if (Root->TryGetObjectField(TEXT("files"), Files) && Files)
				{
					for (const auto& Pair : (*Files)->Values)
					{
						FString Hash;
						if (Pair.Value.IsValid() && Pair.Value->TryGetString(Hash))
						{
							Out.Add(FString(Pair.Key), Hash);
						}
					}
				}
			}
		}
		return Out;
	}

	bool SaveManifest(const FString& Path, const TMap<FString, FString>& Manifest)
	{
		TSharedRef<FJsonObject> Files = MakeShared<FJsonObject>();
		TArray<FString> Keys;
		Manifest.GetKeys(Keys);
		Keys.Sort();
		for (const FString& Key : Keys)
		{
			Files->SetStringField(Key, Manifest[Key]);
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("note"), TEXT("Written by the CursorUnreal plugin. Records which skill files it installed so it can update them without touching ones you edited."));
		Root->SetObjectField(TEXT("files"), Files);
		return FFileHelper::SaveStringToFile(CursorJson::ToString(Root, true), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	FString Unquote(const FString& In)
	{
		FString S = In.TrimStartAndEnd();
		if (S.Len() >= 2 && ((S.StartsWith(TEXT("\"")) && S.EndsWith(TEXT("\""))) || (S.StartsWith(TEXT("'")) && S.EndsWith(TEXT("'")))))
		{
			S = S.Mid(1, S.Len() - 2);
		}
		return S;
	}
}

FString FCursorSkillInstallReport::ToText() const
{
	FString Out = FString::Printf(TEXT("%d installed, %d updated, %d left alone because they were edited."),
		Installed.Num(), Updated.Num(), SkippedEdited.Num());
	for (const FString& File : SkippedEdited)
	{
		Out += FString::Printf(TEXT("\n  kept your version: %s"), *File);
	}
	for (const FString& Error : Errors)
	{
		Out += FString::Printf(TEXT("\n  error: %s"), *Error);
	}
	return Out;
}

FCursorSkillLibrary& FCursorSkillLibrary::Get()
{
	static FCursorSkillLibrary Instance;
	return Instance;
}

FCursorSkillLibrary::FCursorSkillLibrary()
{
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CursorUnreal")))
	{
		Dirs.BuiltIn = NormalizeDir(Plugin->GetBaseDir() / TEXT("Skills"));
	}
	Dirs.User = NormalizeDir(UserHome() / TEXT(".cursor") / TEXT("skills"));
	Dirs.Project = NormalizeDir(FPaths::ProjectDir() / TEXT(".cursor") / TEXT("skills"));
	bUseSettings = true;
	Refresh();
}

FCursorSkillLibrary::FCursorSkillLibrary(const FLayerDirs& InDirs)
{
	Dirs.BuiltIn = InDirs.BuiltIn.IsEmpty() ? FString() : NormalizeDir(InDirs.BuiltIn);
	Dirs.User = InDirs.User.IsEmpty() ? FString() : NormalizeDir(InDirs.User);
	Dirs.Project = InDirs.Project.IsEmpty() ? FString() : NormalizeDir(InDirs.Project);
	bUseSettings = false;
	Refresh();
}

FCursorSkillLibrary::~FCursorSkillLibrary()
{
	StopWatching();
}

FString FCursorSkillLibrary::ManifestPath() const
{
	return Dirs.Project / ManifestFileName;
}

bool FCursorSkillLibrary::ParseSkillFile(const FString& InText, TMap<FString, FString>& OutFrontmatter, FString& OutBody)
{
	FString Text = InText;
	Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));

	TArray<FString> Lines;
	Text.ParseIntoArray(Lines, TEXT("\n"), false);
	if (Lines.Num() == 0 || Lines[0].TrimEnd() != TEXT("---"))
	{
		OutBody = Text;
		return false;
	}

	int32 CloseIndex = INDEX_NONE;
	for (int32 i = 1; i < Lines.Num(); ++i)
	{
		if (Lines[i].TrimEnd() == TEXT("---"))
		{
			CloseIndex = i;
			break;
		}
	}
	if (CloseIndex == INDEX_NONE)
	{
		OutBody = Text;
		return false;
	}

	// The subset of YAML that skill frontmatter uses: "key: value" pairs, with ">" and "|" block
	// scalars for long descriptions.
	for (int32 i = 1; i < CloseIndex; ++i)
	{
		const FString& Line = Lines[i];
		if (Line.TrimStart().IsEmpty() || Line.TrimStart().StartsWith(TEXT("#")) || FChar::IsWhitespace(Line[0]))
		{
			continue;
		}
		int32 Colon = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), Colon))
		{
			continue;
		}
		const FString Key = Line.Left(Colon).TrimStartAndEnd();
		FString Value = Line.Mid(Colon + 1).TrimStartAndEnd();

		if (Value.StartsWith(TEXT(">")) || Value.StartsWith(TEXT("|")))
		{
			const bool bFolded = Value.StartsWith(TEXT(">"));
			TArray<FString> Block;
			while (i + 1 < CloseIndex && (Lines[i + 1].IsEmpty() || FChar::IsWhitespace(Lines[i + 1][0])))
			{
				Block.Add(Lines[++i].TrimStartAndEnd());
			}
			Value = FString::Join(Block, bFolded ? TEXT(" ") : TEXT("\n")).TrimStartAndEnd();
		}
		else
		{
			Value = Unquote(Value);
		}
		OutFrontmatter.Add(Key, Value);
	}

	TArray<FString> BodyLines;
	for (int32 i = CloseIndex + 1; i < Lines.Num(); ++i)
	{
		BodyLines.Add(Lines[i]);
	}
	OutBody = FString::Join(BodyLines, TEXT("\n")).TrimStartAndEnd();
	return true;
}

void FCursorSkillLibrary::ScanLayer(const FString& Root, ECursorSkillLayer Layer, TMap<FString, FCursorSkillInfo>& InOut) const
{
	if (Root.IsEmpty() || !IFileManager::Get().DirectoryExists(*Root))
	{
		return;
	}

	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *(Root / TEXT("*")), false, true);
	for (const FString& Folder : Folders)
	{
		const FString Directory = Root / Folder;
		const FString SkillFile = Directory / SkillFileName;
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *SkillFile))
		{
			continue;
		}

		TMap<FString, FString> Frontmatter;
		FString Body;
		ParseSkillFile(Text, Frontmatter, Body);

		FCursorSkillInfo Info;
		Info.Name = Frontmatter.FindRef(TEXT("name"));
		if (Info.Name.IsEmpty())
		{
			Info.Name = Folder;
		}
		Info.Description = Frontmatter.FindRef(TEXT("description"));
		Info.Layer = Layer;
		Info.Directory = Directory;
		Info.SkillFile = SkillFile;
		Info.bOverridesLowerLayer = InOut.Contains(Info.Name);
		InOut.Add(Info.Name, Info);
	}
}

bool FCursorSkillLibrary::IsEnabled(const FCursorSkillInfo& Info, bool bHasBuiltInOfSameName) const
{
	if (!bUseSettings)
	{
		return true;
	}
	const UCursorUnrealSettings* Settings = UCursorUnrealSettings::Get();
	if (Settings->DisabledSkills.Contains(Info.Name))
	{
		return false;
	}
	// Personal skills are mostly about other kinds of work, so they are opt-in unless they
	// deliberately replace one of the plugin's skills.
	if (Info.Layer == ECursorSkillLayer::User && !bHasBuiltInOfSameName)
	{
		return Settings->EnabledUserSkills.Contains(Info.Name);
	}
	return true;
}

void FCursorSkillLibrary::Refresh()
{
	TMap<FString, FCursorSkillInfo> BuiltIns;
	ScanLayer(Dirs.BuiltIn, ECursorSkillLayer::BuiltIn, BuiltIns);

	TMap<FString, FCursorSkillInfo> Resolved = BuiltIns;
	ScanLayer(Dirs.User, ECursorSkillLayer::User, Resolved);
	ScanLayer(Dirs.Project, ECursorSkillLayer::Project, Resolved);

	const TMap<FString, FString> Manifest = Dirs.Project.IsEmpty() ? TMap<FString, FString>() : LoadManifest(ManifestPath());

	Skills.Reset();
	for (auto& Pair : Resolved)
	{
		FCursorSkillInfo& Info = Pair.Value;
		Info.bEnabled = IsEnabled(Info, BuiltIns.Contains(Info.Name));
		if (Info.Layer == ECursorSkillLayer::Project)
		{
			const FString Relative = FPaths::GetCleanFilename(Info.Directory) / SkillFileName;
			if (const FString* Recorded = Manifest.Find(Relative))
			{
				Info.bInstalledUnmodified = (*Recorded == HashFile(Info.SkillFile));
			}
		}
		Skills.Add(Info);
	}
	Skills.Sort([](const FCursorSkillInfo& A, const FCursorSkillInfo& B) { return A.Name < B.Name; });

	OnChanged.Broadcast();
}

TArray<FCursorSkillInfo> FCursorSkillLibrary::ListEnabled() const
{
	return Skills.FilterByPredicate([](const FCursorSkillInfo& Info) { return Info.bEnabled; });
}

const FCursorSkillInfo* FCursorSkillLibrary::Find(const FString& Name) const
{
	return Skills.FindByPredicate([&Name](const FCursorSkillInfo& Info) { return Info.Name.Equals(Name, ESearchCase::IgnoreCase); });
}

bool FCursorSkillLibrary::Load(const FString& Name, FString& OutBody, FString& OutError) const
{
	const FCursorSkillInfo* Info = Find(Name);
	if (!Info)
	{
		OutError = FString::Printf(TEXT("No skill named '%s'."), *Name);
		return false;
	}
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Info->SkillFile))
	{
		OutError = FString::Printf(TEXT("Could not read %s."), *Info->SkillFile);
		return false;
	}
	TMap<FString, FString> Frontmatter;
	ParseSkillFile(Text, Frontmatter, OutBody);
	return true;
}

TArray<FString> FCursorSkillLibrary::ListFiles(const FString& Name) const
{
	TArray<FString> Out;
	if (const FCursorSkillInfo* Info = Find(Name))
	{
		TArray<FString> Found;
		IFileManager::Get().FindFilesRecursive(Found, *Info->Directory, TEXT("*"), true, false);
		for (FString& File : Found)
		{
			FPaths::MakePathRelativeTo(File, *(Info->Directory + TEXT("/")));
			if (File != SkillFileName)
			{
				Out.Add(File);
			}
		}
		Out.Sort();
	}
	return Out;
}

bool FCursorSkillLibrary::ReadFile(const FString& Name, const FString& RelativePath, FString& OutText, FString& OutError) const
{
	const FCursorSkillInfo* Info = Find(Name);
	if (!Info)
	{
		OutError = FString::Printf(TEXT("No skill named '%s'."), *Name);
		return false;
	}
	FString Full = FPaths::ConvertRelativePathToFull(Info->Directory / RelativePath);
	FPaths::CollapseRelativeDirectories(Full);
	if (!FPaths::IsUnderDirectory(Full, Info->Directory))
	{
		OutError = TEXT("That path is outside the skill folder.");
		return false;
	}
	if (!FFileHelper::LoadFileToString(OutText, *Full))
	{
		OutError = FString::Printf(TEXT("Could not read '%s' in skill '%s'."), *RelativePath, *Name);
		return false;
	}
	return true;
}

FCursorSkillInstallReport FCursorSkillLibrary::InstallBuiltInsToProject(const FString& OnlySkill)
{
	FCursorSkillInstallReport Report;
	if (Dirs.BuiltIn.IsEmpty() || Dirs.Project.IsEmpty())
	{
		Report.Errors.Add(TEXT("Skill folders are not configured."));
		return Report;
	}

	TMap<FString, FCursorSkillInfo> BuiltIns;
	ScanLayer(Dirs.BuiltIn, ECursorSkillLayer::BuiltIn, BuiltIns);

	IFileManager& Files = IFileManager::Get();
	Files.MakeDirectory(*Dirs.Project, true);
	TMap<FString, FString> Manifest = LoadManifest(ManifestPath());

	for (const auto& Pair : BuiltIns)
	{
		const FCursorSkillInfo& Skill = Pair.Value;
		if (!OnlySkill.IsEmpty() && !Skill.Name.Equals(OnlySkill, ESearchCase::IgnoreCase))
		{
			continue;
		}

		const FString FolderName = FPaths::GetCleanFilename(Skill.Directory);
		TArray<FString> Sources;
		Files.FindFilesRecursive(Sources, *Skill.Directory, TEXT("*"), true, false);
		for (const FString& Source : Sources)
		{
			FString Inner = Source;
			FPaths::MakePathRelativeTo(Inner, *(Skill.Directory + TEXT("/")));
			const FString Relative = FolderName / Inner;
			const FString Dest = Dirs.Project / Relative;
			const FString SourceHash = HashFile(Source);

			auto Copy = [&](TArray<FString>& Bucket)
			{
				Files.MakeDirectory(*FPaths::GetPath(Dest), true);
				if (Files.Copy(*Dest, *Source, true, true) == COPY_OK)
				{
					Manifest.Add(Relative, SourceHash);
					Bucket.Add(Relative);
				}
				else
				{
					Report.Errors.Add(FString::Printf(TEXT("Could not copy %s"), *Relative));
				}
			};

			if (!Files.FileExists(*Dest))
			{
				Copy(Report.Installed);
				continue;
			}

			const FString DestHash = HashFile(Dest);
			if (DestHash == SourceHash)
			{
				Manifest.Add(Relative, SourceHash); // Already current.
				continue;
			}

			const FString* Recorded = Manifest.Find(Relative);
			if (Recorded && *Recorded == DestHash)
			{
				// Untouched since we installed it, and the plugin's copy has moved on.
				Copy(Report.Updated);
			}
			else
			{
				Report.SkippedEdited.Add(Relative);
			}
		}
	}

	if (!SaveManifest(ManifestPath(), Manifest))
	{
		Report.Errors.Add(TEXT("Could not write the install manifest."));
	}

	Refresh();
	if (WatchHandles.Num() > 0)
	{
		StartWatching(); // The project folder may have just been created.
	}
	return Report;
}

FString FCursorSkillLibrary::BuildIndexForPrompt() const
{
	const TArray<FCursorSkillInfo> Enabled = ListEnabled();
	if (Enabled.Num() == 0)
	{
		return FString();
	}
	FString Out = TEXT("Skills hold project and engine guidance. Before starting work a skill covers, call load_skill with its name and follow what it says.\n");
	for (const FCursorSkillInfo& Info : Enabled)
	{
		Out += FString::Printf(TEXT("- %s: %s\n"), *Info.Name, *Info.Description);
	}
	return Out;
}

void FCursorSkillLibrary::StartWatching()
{
	FDirectoryWatcherModule& Module = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* Watcher = Module.Get();
	if (!Watcher)
	{
		return;
	}
	for (const FString& Dir : { Dirs.BuiltIn, Dirs.User, Dirs.Project })
	{
		if (Dir.IsEmpty() || WatchHandles.Contains(Dir) || !IFileManager::Get().DirectoryExists(*Dir))
		{
			continue;
		}
		FDelegateHandle Handle;
		if (Watcher->RegisterDirectoryChangedCallback_Handle(Dir,
			IDirectoryWatcher::FDirectoryChanged::CreateRaw(this, &FCursorSkillLibrary::OnDirectoryChanged),
			Handle, IDirectoryWatcher::WatchOptions::IncludeDirectoryChanges))
		{
			WatchHandles.Add(Dir, Handle);
		}
	}
}

void FCursorSkillLibrary::StopWatching()
{
	if (PendingRefresh.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PendingRefresh);
		PendingRefresh.Reset();
	}
	if (WatchHandles.Num() == 0)
	{
		return;
	}
	if (FDirectoryWatcherModule* Module = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")))
	{
		if (IDirectoryWatcher* Watcher = Module->Get())
		{
			for (const auto& Pair : WatchHandles)
			{
				Watcher->UnregisterDirectoryChangedCallback_Handle(Pair.Key, Pair.Value);
			}
		}
	}
	WatchHandles.Reset();
}

void FCursorSkillLibrary::OnDirectoryChanged(const TArray<FFileChangeData>& Changes)
{
	// Editors save in bursts (temp file, rename, touch). Wait for the burst to end, then rescan once.
	if (PendingRefresh.IsValid())
	{
		return;
	}
	PendingRefresh = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float)
	{
		PendingRefresh.Reset();
		Refresh();
		return false;
	}), 0.5f);
}
