#include "CursorNiagaraPack.h"
#include "Modules/ModuleManager.h"

class FCursorUnrealToolsNiagaraModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		CursorNiagaraPack::Register();
	}

	virtual void ShutdownModule() override
	{
		CursorNiagaraPack::Unregister();
	}
};

IMPLEMENT_MODULE(FCursorUnrealToolsNiagaraModule, CursorUnrealToolsNiagara);
