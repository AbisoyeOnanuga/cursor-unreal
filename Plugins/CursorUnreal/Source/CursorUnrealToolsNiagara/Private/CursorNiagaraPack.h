#pragma once

#include "CoreMinimal.h"

/** Registration entry points for the Niagara tool pack. Lives in its own module so a project without Niagara can drop it. */
namespace CursorNiagaraPack
{
	/** "niagara": create systems, inspect emitters and module stacks, add and remove emitters, user parameters, compile, spawn. */
	void Register();
	void Unregister();
}
