#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "CursorJson.h"
#include "CursorTestSupport.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/PackageName.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "Editor.h"

/** Helpers shared by the Blueprint, C++ and Material pack tests. */
namespace CursorPackTest
{
	/** Replaces {0} and {1}. Plain replacement because the JSON around the placeholders is full of braces. */
	inline FString Args(const TCHAR* Format, const FString& A, const FString& B = FString(), const FString& C = FString())
	{
		return FString(Format).Replace(TEXT("{0}"), *A).Replace(TEXT("{1}"), *B).Replace(TEXT("{2}"), *C);
	}

	inline FString UniqueName(const TCHAR* Prefix)
	{
		return FString(Prefix) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Short);
	}

	/**
	 * Hands out asset paths under /Game/CursorUnrealTests and deletes whatever the test made, from memory and from disk,
	 * so repeated runs leave the project as it was.
	 */
	struct FScopedTestAssets
	{
		TArray<FString> PackagePaths;

		FString Add(const TCHAR* Prefix)
		{
			return PackagePaths.Add_GetRef(TEXT("/Game/CursorUnrealTests/") + UniqueName(Prefix));
		}

		~FScopedTestAssets()
		{
			UEditorAssetSubsystem* Assets = GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
			for (const FString& Path : PackagePaths)
			{
				// DeleteAsset removes the file itself for assets that were saved; the manual delete is for any it leaves behind.
				if (Assets && Assets->DoesAssetExist(Path))
				{
					Assets->DeleteAsset(Path);
				}
				FString File;
				if (FPackageName::TryConvertLongPackageNameToFilename(Path, File, FPackageName::GetAssetPackageExtension())
					&& IFileManager::Get().FileExists(*File))
				{
					IFileManager::Get().Delete(*File, /*RequireExists*/ false, /*EvenReadOnly*/ true);
				}
			}
		}
	};

	/** Deletes files and whole folders when the test ends. */
	struct FScopedFiles
	{
		TArray<FString> Files;
		TArray<FString> Folders;

		/** Registers a backup file and the timestamped folder tool wrote it into (Backups/<stamp>/...), so neither outlives the test. */
		void AddBackup(const FString& BackupFile)
		{
			Files.Add(BackupFile);
			FString Relative = BackupFile;
			const FString Root = TEXT("/Backups/");
			const int32 At = Relative.Find(Root);
			if (At != INDEX_NONE)
			{
				const FString AfterRoot = Relative.Mid(At + Root.Len());
				int32 Slash = INDEX_NONE;
				if (AfterRoot.FindChar(TEXT('/'), Slash))
				{
					Folders.Add(Relative.Left(At + Root.Len()) + AfterRoot.Left(Slash));
				}
			}
		}

		~FScopedFiles()
		{
			for (const FString& File : Files)
			{
				IFileManager::Get().Delete(*File, false, true);
			}
			for (const FString& Folder : Folders)
			{
				IFileManager::Get().DeleteDirectory(*Folder, false, true);
			}
		}
	};
}

#endif // WITH_DEV_AUTOMATION_TESTS
