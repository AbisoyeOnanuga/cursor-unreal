#pragma once

#include "CoreMinimal.h"

class UObject;
class UPackage;

/**
 * Helpers shared by the Blueprint, C++ and Material packs. Private to this module: anything
 * that other modules should call lives in CursorToolHelpers.h instead.
 */
namespace CursorPackSupport
{
	/** Only project and plugin content may be changed. Engine content is shared by every project on the machine. */
	bool IsWritableContentPath(const FString& PackagePath, FString& OutError);

	/**
	 * Validates the path of an asset a tool is about to create and makes its (new, empty) package.
	 * Fails when the path is not project content, is not a valid long package name, or already holds an asset.
	 */
	bool PrepareNewAsset(const FString& AssetPath, UPackage*& OutPackage, FString& OutAssetName, FString& OutError);

	/** Tells the asset registry about a freshly created asset and marks its package dirty so asset_save picks it up. */
	void FinishNewAsset(UObject* Asset);

	/** True when an asset (on disk or only in memory) already uses this package path. */
	bool AssetExists(const FString& PackagePath);

	/** Case-insensitive search of a list for a name; returns INDEX_NONE when absent. */
	int32 IndexOfName(const TArray<FString>& Names, const FString& Wanted);

	/** Lower case with everything but letters and digits removed, so "Space Bar", "space_bar" and "SpaceBar" compare equal. */
	FString Squash(const FString& Text);

	/**
	 * Turns the spellings a model reaches for ("Space", (KeyName="W"), EKeys::D, gamepad_a) into the engine's own key name.
	 * An empty value gives "None". Fails, naming close matches, when the text is not a key.
	 */
	bool NormaliseKeyName(const FString& Value, FString& OutName, FString& OutError);
}

/**
 * Pure helpers for the C++ workflow pack. Kept free of editor state (every root is passed in) so
 * they can be tested without touching the real project.
 */
namespace CursorSource
{
	/** One compiler or linker diagnostic parsed out of build output. */
	struct FCompilerMessage
	{
		FString File;
		int32 Line = 0;
		int32 Column = 0;
		/** "error" or "warning". */
		FString Severity;
		/** Compiler code such as C2065, empty for clang messages and UBT's own errors. */
		FString Code;
		FString Text;
	};

	/** Extensions source_read, source_search and source_write will touch (lower-case, without the dot). */
	bool IsSourceExtension(const FString& Path);

	/**
	 * Resolves Path (absolute, or relative to RelativeBase) to a normalised absolute path and checks
	 * that it lies inside one of AllowedRoots. Fails on ".." escapes, empty paths and anything outside the roots.
	 */
	bool ResolveWithinRoots(const FString& Path, const FString& RelativeBase, const TArray<FString>& AllowedRoots, FString& OutAbsolute, FString& OutError);

	/** Folders source_write may change: the project's Source/ and the Source/ of its own plugins, minus this plugin. */
	TArray<FString> WritableRoots();
	/** Folders source_read and source_search may look in: the writable ones, this plugin, and the engine's Source/ and Plugins/. */
	TArray<FString> ReadableRoots();

	/** Resolves a path the agent gave for reading. Relative paths are tried against the project, then the engine's parent folder. */
	bool ResolveForRead(const FString& Path, FString& OutAbsolute, FString& OutError);
	/** Resolves a path the agent gave for writing. Must be a source file under WritableRoots(). */
	bool ResolveForWrite(const FString& Path, FString& OutAbsolute, FString& OutError);

	/**
	 * Copies an existing file to <BackupRoot>/<timestamp>/<path relative to RelativeTo> before it is changed.
	 * Returns the backup path, or an empty string when there was nothing to back up. Sets OutError on failure.
	 */
	FString BackupFile(const FString& AbsoluteFile, const FString& RelativeTo, const FString& BackupRoot, FString& OutError);

	/** Project-relative backup folder: Saved/CursorUnreal/Backups. */
	FString DefaultBackupRoot();

	/** Parses MSVC, clang and UBT diagnostics out of build output. Duplicate lines are reported once. */
	void ParseCompilerOutput(const FString& Output, TArray<FCompilerMessage>& OutMessages);

	/** What cpp_create_class writes. */
	struct FNewClassSpec
	{
		/** Class name without the A/U prefix. */
		FString Name;
		/** Module the class belongs to, for the _API macro. */
		FString ModuleName;
		/** "A", "U" or "F", matching the parent. */
		FString Prefix;
		/** Native parent such as "AActor", and the header that declares it (relative to the engine's Public folder). */
		FString ParentCppName;
		FString ParentInclude;
		/** Include path of the generated header as the .cpp must write it, e.g. "Gameplay/MyActor.h". */
		FString OwnIncludePath;
		bool bIsActor = false;
		bool bIsActorComponent = false;
		bool bBlueprintable = true;
	};

	FString GenerateHeader(const FNewClassSpec& Spec);
	FString GenerateSource(const FNewClassSpec& Spec);

	/** True for a valid C++ identifier that does not collide with a keyword we care about. */
	bool IsValidClassName(const FString& Name, FString& OutError);
}
