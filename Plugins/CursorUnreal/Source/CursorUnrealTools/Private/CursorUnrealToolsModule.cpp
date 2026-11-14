#include "CursorToolPacks.h"
#include "Modules/ModuleManager.h"

class FCursorUnrealToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		CursorToolPacks::RegisterEditorPack();
		CursorToolPacks::RegisterAssetPack();
		CursorToolPacks::RegisterBlueprintPack();
		CursorToolPacks::RegisterCppPack();
		CursorToolPacks::RegisterMaterialPack();
		CursorToolPacks::RegisterAnimBlueprintPack();
		CursorToolPacks::RegisterSoundPack();
		CursorToolPacks::RegisterOptimisationPack();
	}

	virtual void ShutdownModule() override
	{
		CursorToolPacks::UnregisterEditorPack();
		CursorToolPacks::UnregisterAssetPack();
		CursorToolPacks::UnregisterBlueprintPack();
		CursorToolPacks::UnregisterCppPack();
		CursorToolPacks::UnregisterMaterialPack();
		CursorToolPacks::UnregisterAnimBlueprintPack();
		CursorToolPacks::UnregisterSoundPack();
		CursorToolPacks::UnregisterOptimisationPack();
	}
};

IMPLEMENT_MODULE(FCursorUnrealToolsModule, CursorUnrealTools);
