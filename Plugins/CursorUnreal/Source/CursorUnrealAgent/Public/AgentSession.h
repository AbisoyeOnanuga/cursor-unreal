#pragma once

#include "CoreMinimal.h"
#include "AgentBackend.h"
#include "AgentHttpTransport.h"
#include "CursorPerfLog.h"
#include "CursorUnrealSettings.h"

enum class EAgentItemKind : uint8
{
	User,
	Assistant,
	Thinking,
	Tool,
	Notice,
	Error
};

enum class EAgentToolState : uint8
{
	/** The model is still writing the call. */
	Preparing,
	Running,
	Finished,
	/** The turn ended before the call ran. */
	NotRun
};

/** A picture that goes with a chat entry: one the user attached, or one a tool returned. */
struct FAgentItemImage
{
	FCursorImage Image;
	/** The copy under Saved/CursorUnreal/Attachments. Empty when transcripts are off: the picture is then only in memory, until the chat is cleared. */
	FString File;
};

/** One entry in the chat: a message, a block of thinking, a tool call or a notice. */
struct FAgentItem
{
	EAgentItemKind Kind = EAgentItemKind::Notice;
	FString Text;
	FDateTime Time;
	/** User messages: what was attached. Tool calls: what the tool showed the model. */
	TArray<FAgentItemImage> Images;

	// Tool calls only.
	FString ToolCallId;
	FString ToolName;
	FString ArgsJson;
	EAgentToolState ToolState = EAgentToolState::Preparing;
	bool bIsError = false;
	double ToolMs = 0.0;

	/** Which model request produced this, so a retried request can take back what it showed. */
	int32 RoundTrip = 0;
};

/**
 * One chat: a backend, what has been said, and the bookkeeping around each turn (activity
 * tracking, the performance record, the transcript on disk). The chat panel draws this and
 * nothing else, so it stays the same whichever provider is behind it.
 */
class CURSORUNREALAGENT_API FAgentSession : public TSharedFromThis<FAgentSession>
{
public:
	explicit FAgentSession(const TSharedRef<IAgentBackend>& InBackend);
	~FAgentSession();

	IAgentBackend& GetBackend() const { return Backend.Get(); }
	bool IsBusy() const { return bBusy; }

	/** Starts a turn. Returns false, with the reason, when it cannot start; nothing is added to the chat in that case. */
	bool Send(const FString& Text, const TArray<FAgentContextBlock>& Context, FText& OutWhyNot);
	/** The same, with pictures attached. The text may be empty when there is at least one picture. */
	bool Send(const FAgentTurnRequest& Request, FText& OutWhyNot);
	bool CanRetry() const;
	bool Retry(FText& OutWhyNot);
	void Stop();
	/** Forgets the conversation and clears the chat. */
	void NewChat();

	const TArray<TSharedRef<FAgentItem>>& GetItems() const { return Items; }
	/** Tokens used by this chat so far. */
	const FAgentUsage& GetUsage() const { return ChatUsage; }
	/** The turn that is running, or the last one that ran. */
	const FCursorTurnPerf& GetLastTurn() const { return Turn; }
	/** Where this chat is being saved. Empty when saving is off or nothing has been written yet. */
	const FString& GetTranscriptFile() const { return TranscriptFile; }

	static FString GetTranscriptDirectory();

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnItem, const TSharedRef<FAgentItem>&);
	FOnItem OnItemAdded;
	FOnItem OnItemChanged;
	/** Items were removed. Redraw the whole list. */
	FSimpleMulticastDelegate OnItemsReset;
	/** A turn started or ended. */
	FSimpleMulticastDelegate OnStateChanged;

	/** Set false in tests to keep them from writing transcripts and performance records to disk. */
	bool bWriteToDisk = true;

private:
	void BeginTurn();
	void EndTurn(const FString& Outcome);
	void HandleEvent(const FAgentEvent& Event);
	FAgentEventSink MakeSink();

	TSharedRef<FAgentItem> AddItem(EAgentItemKind Kind, const FString& Text);
	/** The last item, if it is of this kind and from the current request; otherwise a new one. */
	TSharedRef<FAgentItem> OpenItem(EAgentItemKind Kind);
	TSharedPtr<FAgentItem> FindTool(const FString& CallId) const;
	void NoteFirstOutput();
	void WriteTranscript(const FString& Outcome);
	/** Attaches pictures to a chat entry, saving a copy of each when this chat is being saved. */
	void AttachImages(FAgentItem& Item, const TArray<FCursorImage>& Images) const;

	TSharedRef<IAgentBackend> Backend;
	TArray<TSharedRef<FAgentItem>> Items;
	FAgentUsage ChatUsage;
	FCursorTurnPerf Turn;

	bool bBusy = false;
	/** Tells events of the current turn from stragglers of one that was abandoned. */
	uint32 TurnSerial = 0;
	int32 RoundTrip = 0;
	double TurnStartSeconds = 0.0;
	double ToolStartSeconds = 0.0;
	FAgentUsage TurnUsage;

	FString TranscriptFile;
	int32 NumItemsWritten = 0;
};

namespace CursorAgent
{
	/** Builds the backend for a provider. Null, with the reason, when that provider is not available. */
	CURSORUNREALAGENT_API TSharedPtr<IAgentBackend> CreateBackend(ECursorProvider Provider, FText& OutError);

	/** True when the editor was started with -CursorUnrealDemo. CreateBackend then returns the demo backend whatever the provider. */
	CURSORUNREALAGENT_API bool IsDemoMode();

	/**
	 * A backend that replays scripted replies from the plugin's Resources/Fixtures/demo folder, so
	 * the panel can be tried, and its documentation screenshots taken, without a key or any cost.
	 * Nothing leaves the process. The tool calls in the script are real and run in the editor.
	 * The caller drives the transport: StartAutoPump for a live panel, PumpAll to finish at once.
	 */
	CURSORUNREALAGENT_API TSharedPtr<IAgentBackend> CreateDemoBackend(TSharedPtr<FAgentFixtureTransport>& OutTransport, FText& OutError);
}
