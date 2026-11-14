#pragma once

#include "CoreMinimal.h"

namespace CursorAgent
{
	/**
	 * Instructions given to the model at the start of a conversation: where it is, how the editor
	 * tools behave, which skills exist, and the project's own rules from settings.
	 *
	 * Holds nothing that changes while a conversation runs (no clock, no current selection), so
	 * the provider can cache it. Anything that changes travels with the user's message instead.
	 */
	CURSORUNREALAGENT_API FString BuildSystemPrompt();
}
