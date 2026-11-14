#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class AActor;
class UWorld;

/**
 * Shared lookups and conversions for tool packs. Public so that tool packs in other modules
 * (and in your own plugins) resolve names the same way the built-in ones do.
 */
namespace CursorToolHelpers
{
	/** The level being edited. Null in the rare case no editor world exists. */
	CURSORUNREALTOOLS_API UWorld* GetEditorWorld();

	/**
	 * Finds one actor in the editor world by object name, label or full path.
	 * Fails with a message naming the candidates when a label matches more than one actor.
	 */
	CURSORUNREALTOOLS_API AActor* FindActor(const FString& NameOrLabel, FString& OutError);

	/**
	 * Resolves what a tool argument refers to:
	 *  - "/Game/Folder/Asset" or "/Game/Folder/Asset.Asset"  -> the asset (loaded if needed)
	 *  - "/Script/Engine.StaticMeshActor"                     -> a native class
	 *  - "MyActorLabel"                                       -> an actor in the open level
	 *  - "MyActorLabel.ComponentName"                         -> one of that actor's components
	 */
	CURSORUNREALTOOLS_API UObject* ResolveObject(const FString& Path, FString& OutError);

	/** Loads an asset by package or object path. */
	CURSORUNREALTOOLS_API UObject* LoadAsset(const FString& Path, FString& OutError);

	/**
	 * Resolves a class by "/Script/Module.Class", a short native name ("StaticMeshActor"), or a
	 * Blueprint asset path (returns its generated class).
	 */
	CURSORUNREALTOOLS_API UClass* ResolveClass(const FString& NameOrPath, UClass* RequiredBase, FString& OutError);

	/** "/Game/Folder/Asset.Asset" or "/Game/Folder/Asset" -> "/Game/Folder/Asset". */
	CURSORUNREALTOOLS_API FString ToPackagePath(const FString& AssetPath);

	/** Summary used by actor tools: name, label, class, folder, transform. */
	CURSORUNREALTOOLS_API TSharedRef<FJsonObject> DescribeActor(const AActor* Actor, bool bIncludeComponents);

	/** Reads properties as JSON. With no names, returns the ones the Details panel shows. */
	CURSORUNREALTOOLS_API TSharedRef<FJsonObject> ReadProperties(const UObject* Object, const TArray<FString>& Names, const FString& NameContains, int32 MaxProperties, TArray<FString>& OutMissing);

	/**
	 * Sets one property from JSON, with the same notifications the Details panel sends.
	 * PropertyPath may reach into structs with dots: "RelativeLocation.X", "LightComponent.Intensity" is not
	 * supported (address the component itself instead).
	 */
	CURSORUNREALTOOLS_API bool WriteProperty(UObject* Object, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, FString& OutError);
}
