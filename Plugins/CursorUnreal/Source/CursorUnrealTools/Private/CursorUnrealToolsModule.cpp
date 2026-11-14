#include "CursorToolPacks.h"
#include "Modules/ModuleManager.h"

class FCursorUnrealToolsModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		CursorToolPacks::RegisterEditorPack();
		CursorToolPacks::RegisterAssetPack();
	}

	virtual void ShutdownModule() override
	{
		CursorToolPacks::UnregisterEditorPack();
		CursorToolPacks::UnregisterAssetPack();
	}
};

IMPLEMENT_MODULE(FCursorUnrealToolsModule, CursorUnrealTools);
