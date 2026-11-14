#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/** Where a skill came from. Higher layers override lower ones with the same name. */
enum class ECursorSkillLayer : uint8
{
	/** Shipped with the plugin. */
	BuiltIn,
	/** ~/.cursor/skills, shared across all your projects. */
	User,
	/** <Project>/.cursor/skills, shared with the team through source control. */
	Project
};

CURSORUNREALCORE_API const TCHAR* LexToString(ECursorSkillLayer Layer);

struct FCursorSkillInfo
{
	FString Name;
	FString Description;
	ECursorSkillLayer Layer = ECursorSkillLayer::BuiltIn;
	/** Folder holding SKILL.md and any reference files. */
	FString Directory;
	FString SkillFile;
	/** Whether the in-editor agent is offered this skill. Cursor agents read .cursor/skills themselves and ignore this. */
	bool bEnabled = true;
	/** A lower layer has a skill of the same name that this one replaces. */
	bool bOverridesLowerLayer = false;
	/** Project copy of a built-in that has not been edited since it was installed. */
	bool bInstalledUnmodified = false;
};

struct CURSORUNREALCORE_API FCursorSkillInstallReport
{
	TArray<FString> Installed;
	TArray<FString> Updated;
	/** Left alone because the project copy was edited, or was not installed by this plugin. */
	TArray<FString> SkippedEdited;
	TArray<FString> Errors;

	FString ToText() const;
};

/**
 * Finds SKILL.md folders in three layers and resolves overrides. The format is the same one
 * Cursor uses, so a skill written here also works in Cursor IDE and for Cursor SDK agents.
 */
class CURSORUNREALCORE_API FCursorSkillLibrary
{
public:
	struct FLayerDirs
	{
		FString BuiltIn;
		FString User;
		FString Project;
	};

	static FCursorSkillLibrary& Get();

	/** Uses the real plugin, home and project folders. */
	FCursorSkillLibrary();
	/** For tests: explicit folders, and every skill counts as enabled. */
	explicit FCursorSkillLibrary(const FLayerDirs& InDirs);
	~FCursorSkillLibrary();

	const FLayerDirs& GetDirs() const { return Dirs; }

	/** Rescans all three layers. */
	void Refresh();

	/** One entry per skill name after overrides, sorted by name. */
	const TArray<FCursorSkillInfo>& List() const { return Skills; }
	TArray<FCursorSkillInfo> ListEnabled() const;
	const FCursorSkillInfo* Find(const FString& Name) const;

	/** Reads the body of SKILL.md, without the frontmatter. */
	bool Load(const FString& Name, FString& OutBody, FString& OutError) const;
	/** Reads a reference file that sits next to SKILL.md. Paths that leave the skill folder are refused. */
	bool ReadFile(const FString& Name, const FString& RelativePath, FString& OutText, FString& OutError) const;
	/** Files in the skill folder other than SKILL.md, relative to it. */
	TArray<FString> ListFiles(const FString& Name) const;

	/**
	 * Copies built-in skills into the project layer so Cursor IDE and Cursor SDK agents can read
	 * them. Never overwrites a file that was edited after it was installed. Pass a name to copy one.
	 */
	FCursorSkillInstallReport InstallBuiltInsToProject(const FString& OnlySkill = FString());

	/** Names and descriptions of enabled skills, for the system prompt. Empty when there are none. */
	FString BuildIndexForPrompt() const;

	/** Watch the layer folders and refresh when files change. */
	void StartWatching();
	void StopWatching();

	FSimpleMulticastDelegate OnChanged;

	/** Splits SKILL.md into frontmatter fields and body. Returns false when there is no frontmatter. */
	static bool ParseSkillFile(const FString& Text, TMap<FString, FString>& OutFrontmatter, FString& OutBody);

private:
	void ScanLayer(const FString& Root, ECursorSkillLayer Layer, TMap<FString, FCursorSkillInfo>& InOut) const;
	bool IsEnabled(const FCursorSkillInfo& Info, bool bHasBuiltInOfSameName) const;
	void OnDirectoryChanged(const TArray<struct FFileChangeData>& Changes);
	FString ManifestPath() const;

	FLayerDirs Dirs;
	TArray<FCursorSkillInfo> Skills;
	bool bUseSettings = true;

	TMap<FString, FDelegateHandle> WatchHandles;
	FTSTicker::FDelegateHandle PendingRefresh;
};
