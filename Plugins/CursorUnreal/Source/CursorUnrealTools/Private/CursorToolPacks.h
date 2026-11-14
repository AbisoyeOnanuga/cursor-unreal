#pragma once

#include "CoreMinimal.h"

/** Registration entry points for the built-in tool packs. One pair per pack, one .cpp per pack. */
namespace CursorToolPacks
{
	/** "editor": level, actors, selection, properties, log, console and Python. */
	void RegisterEditorPack();
	void UnregisterEditorPack();

	/** "assets": search, metadata, save, duplicate, rename, delete. */
	void RegisterAssetPack();
	void UnregisterAssetPack();

	/** "blueprint": create, inspect and edit Blueprints (variables, components, common graph nodes, compile). */
	void RegisterBlueprintPack();
	void UnregisterBlueprintPack();

	/** "cpp": cpp_create_class, cpp_build, and the NativeAgentOnly source_read / source_search / source_write. */
	void RegisterCppPack();
	void UnregisterCppPack();

	/** "materials": create materials and instances, build expression graphs, set parameters, compile. */
	void RegisterMaterialPack();
	void UnregisterMaterialPack();

	/** "animation": create and inspect Animation Blueprints (graphs, nodes, compile). */
	void RegisterAnimBlueprintPack();
	void UnregisterAnimBlueprintPack();

	/** "audio": SoundCue create/inspect/play, attenuation assets, ambient spawn. */
	void RegisterSoundPack();
	void UnregisterSoundPack();

	/** "optimisation": read-only mesh/actor cost estimates and LOD suggestions. */
	void RegisterOptimisationPack();
	void UnregisterOptimisationPack();
}
