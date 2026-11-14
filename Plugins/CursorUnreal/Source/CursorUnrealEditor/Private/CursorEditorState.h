#pragma once

#include "CoreMinimal.h"
#include "AgentSession.h"
#include "CursorUnrealSettings.h"

/**
 * Owns the chat session for the editor. It lives here rather than in the panel so that closing
 * the tab, or docking it somewhere else, does not throw the conversation away.
 */
class FCursorEditorState
{
public:
	static FCursorEditorState& Get();

	void Shutdown();

	/** The session for the provider chosen in the settings, created on first use. Null when that provider cannot be used. */
	TSharedPtr<FAgentSession> GetSession();
	/** Why there is no session. Empty when there is one. */
	FText GetProblem() const { return Problem; }

	/** Replaces the session if the provider setting no longer matches it. A running turn is stopped. */
	void SyncWithSettings();

	/** Puts a ready-made session in place. Used for the documentation screenshots and by tests. */
	void UseSession(const TSharedPtr<FAgentSession>& InSession);

	FSimpleMulticastDelegate OnSessionReplaced;

private:
	void Rebuild();

	TSharedPtr<FAgentSession> Session;
	ECursorProvider SessionProvider = ECursorProvider::Anthropic;
	bool bBuilt = false;
	/** True while a session supplied through UseSession is in place, so settings changes leave it alone. */
	bool bPinned = false;
	FText Problem;
};
