#include "CursorEditorState.h"

FCursorEditorState& FCursorEditorState::Get()
{
	static FCursorEditorState Instance;
	return Instance;
}

void FCursorEditorState::Shutdown()
{
	Session.Reset();
	bBuilt = false;
	bPinned = false;
	OnSessionReplaced.Clear();
}

TSharedPtr<FAgentSession> FCursorEditorState::GetSession()
{
	if (!bBuilt)
	{
		Rebuild();
	}
	return Session;
}

void FCursorEditorState::SyncWithSettings()
{
	if (bPinned)
	{
		return;
	}
	if (!bBuilt || SessionProvider != UCursorUnrealSettings::Get()->Provider)
	{
		Rebuild();
		OnSessionReplaced.Broadcast();
	}
}

void FCursorEditorState::RebuildSession()
{
	if (bPinned)
	{
		return;
	}
	Rebuild();
	OnSessionReplaced.Broadcast();
}

void FCursorEditorState::UseSession(const TSharedPtr<FAgentSession>& InSession)
{
	Session = InSession;
	// Handing back nothing means "go back to the session the settings ask for", which is built on next use.
	bBuilt = InSession.IsValid();
	bPinned = InSession.IsValid();
	Problem = FText::GetEmpty();
	OnSessionReplaced.Broadcast();
}

void FCursorEditorState::Rebuild()
{
	bBuilt = true;
	SessionProvider = UCursorUnrealSettings::Get()->Provider;
	Problem = FText::GetEmpty();
	// Let go of the old session first: destroying it stops its turn and closes its books.
	Session.Reset();

	const TSharedPtr<IAgentBackend> Backend = CursorAgent::CreateBackend(SessionProvider, Problem);
	if (Backend.IsValid())
	{
		Session = MakeShared<FAgentSession>(Backend.ToSharedRef());
	}
	else if (Problem.IsEmpty())
	{
		Problem = FText::FromString(TEXT("The chosen provider could not be started."));
	}
}
