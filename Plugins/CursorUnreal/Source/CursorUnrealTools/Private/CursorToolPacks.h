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
}
