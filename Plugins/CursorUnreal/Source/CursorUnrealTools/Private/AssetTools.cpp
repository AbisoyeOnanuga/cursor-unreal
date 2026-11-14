#include "CursorToolPacks.h"

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "CursorJson.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Subsystems/EditorAssetSubsystem.h"
#include "UObject/Package.h"

namespace
{
	const TCHAR* AssetPack = TEXT("assets");

	UEditorAssetSubsystem* AssetSubsystem()
	{
		return GEditor ? GEditor->GetEditorSubsystem<UEditorAssetSubsystem>() : nullptr;
	}

	/** Only project and plugin content may be changed. Engine content is shared by every project on the machine. */
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

	TArray<FName> PackageReferencers(const FString& PackagePath)
	{
		TArray<FName> Referencers;
		IAssetRegistry::GetChecked().GetReferencers(FName(*PackagePath), Referencers);
		Referencers.RemoveAll([&](const FName& Name) { return Name.ToString() == PackagePath; });
		return Referencers;
	}

	TArray<FString> NamesToStrings(const TArray<FName>& Names, int32 Max, int32* OutTotal = nullptr)
	{
		TArray<FString> Out;
		for (int32 i = 0; i < Names.Num() && i < Max; ++i)
		{
			Out.Add(Names[i].ToString());
		}
		if (OutTotal)
		{
			*OutTotal = Names.Num();
		}
		return Out;
	}
}

void CursorToolPacks::RegisterAssetPack()
{
	FCursorToolBuilder(TEXT("asset_search"), AssetPack)
		.Describe(TEXT("Find assets by name, class and folder using the asset registry (fast, does not load them). "
			"Returns object paths to use with other tools. Searches /Game by default."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("query"), TEXT("Text the asset name must contain. Case-insensitive."))
			.Str(TEXT("class"), TEXT("Asset class, subclasses included: Blueprint, Material, MaterialInstanceConstant, StaticMesh, Texture2D, NiagaraSystem, SoundCue, AnimBlueprint, World..."))
			.Str(TEXT("path"), TEXT("Folder to search, including subfolders. Default '/Game'. Use '/' prefixed plugin roots for plugin content."))
			.Int(TEXT("limit"), TEXT("Maximum results. Default 50, maximum 500."))
			.Int(TEXT("offset"), TEXT("Skip this many results, for paging.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			IAssetRegistry& Registry = IAssetRegistry::GetChecked();

			FARFilter Filter;
			FString Folder = Args.Str(TEXT("path"), TEXT("/Game"));
			while (Folder.Len() > 1 && Folder.EndsWith(TEXT("/")))
			{
				Folder.LeftChopInline(1);
			}
			Filter.PackagePaths.Add(FName(*Folder));
			Filter.bRecursivePaths = true;

			if (Args.Has(TEXT("class")))
			{
				FString Error;
				const UClass* Class = CursorToolHelpers::ResolveClass(Args.Str(TEXT("class")), nullptr, Error);
				if (!Class)
				{
					return FCursorToolResult::Error(Error);
				}
				Filter.ClassPaths.Add(Class->GetClassPathName());
				Filter.bRecursiveClasses = true;
			}

			TArray<FAssetData> Assets;
			Registry.GetAssets(Filter, Assets);

			const FString Query = Args.Str(TEXT("query"));
			if (!Query.IsEmpty())
			{
				Assets.RemoveAll([&](const FAssetData& Asset) { return !Asset.AssetName.ToString().Contains(Query, ESearchCase::IgnoreCase); });
			}
			Assets.Sort([](const FAssetData& A, const FAssetData& B) { return A.PackageName.LexicalLess(B.PackageName); });

			const int32 Limit = FMath::Clamp(Args.Int(TEXT("limit"), 50), 1, 500);
			const int32 Offset = FMath::Max(0, Args.Int(TEXT("offset"), 0));
			TArray<TSharedPtr<FJsonValue>> Items;
			for (int32 i = Offset; i < Assets.Num() && Items.Num() < Limit; ++i)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetStringField(TEXT("path"), Assets[i].GetObjectPathString());
				Item->SetStringField(TEXT("class"), Assets[i].AssetClassPath.GetAssetName().ToString());
				Items.Add(MakeShared<FJsonValueObject>(Item));
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetNumberField(TEXT("total"), Assets.Num());
			Out->SetNumberField(TEXT("offset"), Offset);
			Out->SetArrayField(TEXT("assets"), Items);
			if (Registry.IsLoadingAssets())
			{
				Out->SetStringField(TEXT("note"), TEXT("The editor is still scanning assets, so results may be incomplete. Try again shortly."));
			}
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("asset_get_info"), AssetPack)
		.Describe(TEXT("Metadata for one asset without opening it: class, registry tags (parent class, texture size, triangle count and so on), "
			"size on disk, what it depends on and what references it, and whether it has unsaved changes."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("Asset path, for example /Game/Meshes/SM_Rock."), true)
			.Int(TEXT("max_links"), TEXT("Maximum dependencies and referencers to list. Default 40.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			IAssetRegistry& Registry = IAssetRegistry::GetChecked();
			const FString PackagePath = CursorToolHelpers::ToPackagePath(Args.Str(TEXT("path")));

			TArray<FAssetData> InPackage;
			Registry.GetAssetsByPackageName(FName(*PackagePath), InPackage);
			if (InPackage.Num() == 0)
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("No asset at '%s'. Use asset_search to find it."), *PackagePath));
			}
			// A package normally holds one asset; prefer the one named after the package.
			const FString ShortName = FPackageName::GetShortName(PackagePath);
			const FAssetData* Asset = InPackage.FindByPredicate([&](const FAssetData& A) { return A.AssetName.ToString() == ShortName; });
			if (!Asset)
			{
				Asset = &InPackage[0];
			}

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetStringField(TEXT("path"), Asset->GetObjectPathString());
			Out->SetStringField(TEXT("class"), Asset->AssetClassPath.ToString());

			TSharedRef<FJsonObject> Tags = MakeShared<FJsonObject>();
			for (const auto& Pair : Asset->TagsAndValues.CopyMap())
			{
				// Some tags hold whole serialised blobs; a model has no use for those.
				if (Pair.Value.Len() <= 300)
				{
					Tags->SetStringField(Pair.Key.ToString(), Pair.Value);
				}
			}
			Out->SetObjectField(TEXT("tags"), Tags);

			if (const TOptional<FAssetPackageData> PackageData = Registry.GetAssetPackageDataCopy(FName(*PackagePath)); PackageData.IsSet())
			{
				Out->SetNumberField(TEXT("disk_size_bytes"), static_cast<double>(PackageData->DiskSize));
			}
			if (const UPackage* Loaded = FindPackage(nullptr, *PackagePath))
			{
				Out->SetBoolField(TEXT("loaded"), true);
				Out->SetBoolField(TEXT("has_unsaved_changes"), Loaded->IsDirty());
			}
			else
			{
				Out->SetBoolField(TEXT("loaded"), false);
			}

			const int32 MaxLinks = FMath::Clamp(Args.Int(TEXT("max_links"), 40), 0, 500);
			TArray<FName> Dependencies;
			Registry.GetDependencies(FName(*PackagePath), Dependencies);
			// Engine script packages are noise here.
			Dependencies.RemoveAll([](const FName& Name) { return Name.ToString().StartsWith(TEXT("/Script/")); });
			int32 Total = 0;
			Out->SetArrayField(TEXT("depends_on"), CursorJson::StringArray(NamesToStrings(Dependencies, MaxLinks, &Total)));
			Out->SetNumberField(TEXT("depends_on_total"), Total);
			Out->SetArrayField(TEXT("referenced_by"), CursorJson::StringArray(NamesToStrings(PackageReferencers(PackagePath), MaxLinks, &Total)));
			Out->SetNumberField(TEXT("referenced_by_total"), Total);
			return FCursorToolResult::OkJson(Out);
		})
		.Register();

	FCursorToolBuilder(TEXT("asset_save"), AssetPack)
		.Describe(TEXT("Write assets or the open level to disk. Changes made by other tools stay in memory until saved. "
			"Pass 'paths' for specific assets, or 'all_dirty' to save everything with unsaved changes. Saving cannot be undone with Ctrl+Z; "
			"the previous file contents are only recoverable from source control."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.StrArray(TEXT("paths"), TEXT("Asset or level paths to save."))
			.Bool(TEXT("all_dirty"), TEXT("Save every asset and level that has unsaved changes.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			TArray<UPackage*> Packages;
			TArray<FString> NotFound;

			if (Args.Bool(TEXT("all_dirty")))
			{
				FEditorFileUtils::GetDirtyWorldPackages(Packages);
				FEditorFileUtils::GetDirtyContentPackages(Packages);
			}
			for (const FString& Path : Args.StrArray(TEXT("paths")))
			{
				const FString PackagePath = CursorToolHelpers::ToPackagePath(Path);
				if (UPackage* Package = FindPackage(nullptr, *PackagePath))
				{
					Packages.AddUnique(Package);
				}
				else if (FPackageName::DoesPackageExist(PackagePath))
				{
					// On disk and never loaded: nothing in memory to save.
				}
				else
				{
					NotFound.Add(Path);
				}
			}
			if (NotFound.Num() > 0)
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("No such asset: %s. Nothing was saved."), *FString::Join(NotFound, TEXT(", "))));
			}

			// Unsaved transient packages (/Temp, /Engine/Transient) have no file to go to.
			Packages.RemoveAll([](const UPackage* Package)
			{
				return !Package || Package == GetTransientPackage() || Package->HasAnyFlags(RF_Transient) || Package->GetName().StartsWith(TEXT("/Temp/"));
			});

			TArray<FString> Saved;
			for (const UPackage* Package : Packages)
			{
				if (Package->IsDirty())
				{
					Saved.Add(Package->GetName());
				}
			}
			if (Saved.Num() == 0)
			{
				return FCursorToolResult::Ok(TEXT("Nothing needed saving."));
			}
			const bool bOk = UEditorLoadingAndSavingUtils::SavePackages(Packages, /*bOnlyDirty*/ true);

			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			Out->SetArrayField(bOk ? TEXT("saved") : TEXT("attempted"), CursorJson::StringArray(Saved));
			FCursorToolResult Result = FCursorToolResult::OkJson(Out);
			if (!bOk)
			{
				Result.bIsError = true;
				Result.Text = TEXT("Some packages could not be saved (read-only file, source control, or a validation error). See editor_log_tail.\n") + Result.Text;
			}
			return Result;
		})
		.Register();

	FCursorToolBuilder(TEXT("asset_duplicate"), AssetPack)
		.Describe(TEXT("Copy an asset to a new path. The copy is created in memory; call asset_save to write it to disk."))
		.Risk(ECursorToolRisk::Mutating)
		.Schema(FCursorSchema()
			.Str(TEXT("source"), TEXT("Asset to copy."), true)
			.Str(TEXT("destination"), TEXT("New asset path, for example /Game/Materials/M_Rock_Copy."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			UEditorAssetSubsystem* Assets = AssetSubsystem();
			const FString Source = CursorToolHelpers::ToPackagePath(Args.Str(TEXT("source")));
			const FString Destination = CursorToolHelpers::ToPackagePath(Args.Str(TEXT("destination")));
			FString Error;
			if (!IsWritableContentPath(Destination, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			if (!Assets || !Assets->DoesAssetExist(Source))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("No asset at '%s'."), *Source));
			}
			if (Assets->DoesAssetExist(Destination))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("'%s' already exists. Pick another name."), *Destination));
			}
			const UObject* Copy = Assets->DuplicateAsset(Source, Destination);
			return Copy ? FCursorToolResult::Ok(FString::Printf(TEXT("Created %s"), *Copy->GetPathName()))
				: FCursorToolResult::Error(TEXT("The editor refused to duplicate the asset. See editor_log_tail."));
		})
		.Register();

	FCursorToolBuilder(TEXT("asset_rename"), AssetPack)
		.Describe(TEXT("Rename or move an asset. References in loaded assets are updated and a redirector is left at the old path. "
			"Asks the user for approval because it moves files on disk and editor undo does not reverse it."))
		.Risk(ECursorToolRisk::Dangerous)
		.Schema(FCursorSchema()
			.Str(TEXT("source"), TEXT("Current asset path."), true)
			.Str(TEXT("destination"), TEXT("New asset path."), true))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			UEditorAssetSubsystem* Assets = AssetSubsystem();
			const FString Source = CursorToolHelpers::ToPackagePath(Args.Str(TEXT("source")));
			const FString Destination = CursorToolHelpers::ToPackagePath(Args.Str(TEXT("destination")));
			FString Error;
			if (!IsWritableContentPath(Source, Error) || !IsWritableContentPath(Destination, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			if (!Assets || !Assets->DoesAssetExist(Source))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("No asset at '%s'."), *Source));
			}
			if (Assets->DoesAssetExist(Destination))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("'%s' already exists."), *Destination));
			}
			return Assets->RenameAsset(Source, Destination)
				? FCursorToolResult::Ok(FString::Printf(TEXT("Moved %s to %s"), *Source, *Destination))
				: FCursorToolResult::Error(TEXT("The editor refused to rename the asset. See editor_log_tail."));
		})
		.Register();

	FCursorToolBuilder(TEXT("asset_delete"), AssetPack)
		.Describe(TEXT("Delete an asset from the project. Asks the user for approval. Refuses when other assets still reference it unless 'force' is set, "
			"in which case those references are cleared. Deleting removes the file and cannot be undone except through source control."))
		.Risk(ECursorToolRisk::Dangerous)
		.Schema(FCursorSchema()
			.Str(TEXT("path"), TEXT("Asset to delete."), true)
			.Bool(TEXT("force"), TEXT("Delete even though other assets reference it. Default false.")))
		.Run([](const FCursorArgs& Args, const FCursorToolContext&)
		{
			UEditorAssetSubsystem* Assets = AssetSubsystem();
			const FString PackagePath = CursorToolHelpers::ToPackagePath(Args.Str(TEXT("path")));
			FString Error;
			if (!IsWritableContentPath(PackagePath, Error))
			{
				return FCursorToolResult::Error(Error);
			}
			if (!Assets || !Assets->DoesAssetExist(PackagePath))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("No asset at '%s'."), *PackagePath));
			}
			const TArray<FName> Referencers = PackageReferencers(PackagePath);
			if (Referencers.Num() > 0 && !Args.Bool(TEXT("force")))
			{
				return FCursorToolResult::Error(FString::Printf(TEXT("'%s' is referenced by %d asset(s): %s. Fix those first, or pass force=true to delete anyway and clear the references."),
					*PackagePath, Referencers.Num(), *FString::Join(NamesToStrings(Referencers, 10), TEXT(", "))));
			}
			return Assets->DeleteAsset(PackagePath)
				? FCursorToolResult::Ok(FString::Printf(TEXT("Deleted %s"), *PackagePath))
				: FCursorToolResult::Error(TEXT("The editor refused to delete the asset. See editor_log_tail."));
		})
		.Register();
}

void CursorToolPacks::UnregisterAssetPack()
{
	FCursorToolRegistry::Get().UnregisterPack(AssetPack);
}
