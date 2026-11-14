#pragma once

#include "CoreMinimal.h"
#include "CursorImage.h"

/** Token counts for one user turn, summed over every model round trip it took. */
struct FAgentUsage
{
	/** Input tokens that were neither read from nor written to the prompt cache. */
	int64 InputTokens = 0;
	int64 OutputTokens = 0;
	int64 CacheReadTokens = 0;
	int64 CacheWriteTokens = 0;

	int64 TotalInput() const { return InputTokens + CacheReadTokens + CacheWriteTokens; }
};

enum class EAgentEventType : uint8
{
	/** A request is about to go to the model. One per round trip, retries included. */
	RequestStarted,
	/** Text holds the next piece of the reply. */
	TextDelta,
	/** Text holds the next piece of the model's thinking summary. */
	ThinkingDelta,
	/** The model began writing a tool call. ToolCallId and ToolName are set; the arguments are still arriving. */
	ToolCallStarted,
	/** The tool is about to run. ArgsJson is complete. */
	ToolCallRunning,
	/** The tool finished. Text is its result, bIsError says whether it failed. */
	ToolCallFinished,
	/** Usage holds the running totals for this turn. */
	Usage,
	/** Something the user should know that is not an error: a retry, a truncated reply. */
	Notice,
	/** The current round trip is being thrown away and retried: remove what it has shown so far. */
	Rewind,
	/** The turn is over. Outcome is "done", "cancelled", "truncated", "refused" or "max_iterations". Always the last event. */
	Done,
	/** The turn failed. Text says why. Always the last event. */
	Error
};

struct FAgentEvent
{
	EAgentEventType Type = EAgentEventType::Notice;
	FString Text;
	FString ToolCallId;
	FString ToolName;
	FString ArgsJson;
	bool bIsError = false;
	FAgentUsage Usage;
	FString Outcome;
	/** ToolCallFinished only: pictures the tool returned, as they were sent to the model. */
	TArray<FCursorImage> Images;

	static FAgentEvent Make(EAgentEventType InType, const FString& InText = FString())
	{
		FAgentEvent Event;
		Event.Type = InType;
		Event.Text = InText;
		return Event;
	}
};

using FAgentEventSink = TFunction<void(const FAgentEvent&)>;

/** A model the provider offers, with what the plugin needs to know to talk to it correctly. */
struct FAgentModelInfo
{
	FString Id;
	FString DisplayName;
	/** Model family as the provider reports it ("opus", "sonnet", ...). May be empty. */
	FString Line;
	/** Still callable but on its way out. */
	bool bDeprecated = false;
	/** Largest max_tokens the model accepts. Zero when unknown. */
	int32 MaxOutputTokens = 0;
	int32 MaxInputTokens = 0;
	/** Accepts thinking: {type: "adaptive"}. */
	bool bAdaptiveThinking = false;
	/** Effort levels the model accepts, such as "low" or "xhigh". Empty when it has no effort control. */
	TArray<FString> EffortLevels;
};

using FAgentModelsCallback = TFunction<void(bool bOk, const TArray<FAgentModelInfo>& Models, const FString& Error)>;

/** Extra material sent along with a message, such as the current selection. */
struct FAgentContextBlock
{
	FString Title;
	FString Text;
};

struct FAgentTurnRequest
{
	FString Text;
	TArray<FAgentContextBlock> Context;
	/** Pictures attached to the message: reference art, a sketch, a screenshot. At most CursorImages::MaxPerMessage. */
	TArray<FCursorImage> Images;
};

namespace CursorAgent
{
	/**
	 * Pictures cost tokens every time the conversation is sent again, so a conversation keeps only
	 * the newest few. When it holds more than MaxImagesInHistory, the oldest are dropped until
	 * KeepImagesInHistory remain. Dropping in steps rather than one at a time keeps the prompt
	 * cache useful in between.
	 */
	constexpr int32 MaxImagesInHistory = 10;
	constexpr int32 KeepImagesInHistory = 5;
	/** Most pictures tools may return to the model in one turn. Later ones are described in text instead. */
	constexpr int32 MaxToolImagesPerTurn = 12;

	/** Empty when the pictures can go with one message, otherwise why not: too many, too large, or unreadable. */
	CURSORUNREALAGENT_API FString CheckMessageImages(const TArray<FCursorImage>& Images);
	/** The line that introduces picture Index (from zero) of Count in a user message, so the model can tell them apart. */
	CURSORUNREALAGENT_API FString DescribeAttachedImage(const FCursorImage& Image, int32 Index, int32 Count);
	/**
	 * The pictures of a tool result that may still go to the model this turn. Counts them in
	 * InOutSentThisTurn. When some are held back, says so at the end of InOutText, so the model
	 * knows it did not get them.
	 */
	CURSORUNREALAGENT_API TArray<FCursorImage> TakeToolImages(const TArray<FCursorImage>& Images, int32& InOutSentThisTurn, FString& InOutText);
}

/**
 * A way of running an agent: a model provider plus whatever drives its tool loop. Each backend
 * owns one conversation. Everything here is called on the game thread, and events are delivered
 * on the game thread.
 */
class IAgentBackend
{
public:
	virtual ~IAgentBackend() = default;

	/** Short id for logs and the performance record: "anthropic", "cursor". */
	virtual FString GetProviderId() const = 0;
	/** The model the next turn will use. Empty when none is chosen. */
	virtual FString GetModelId() const = 0;
	/** Chooses the model for later turns and remembers the choice in the settings. */
	virtual void SetModelId(const FString& ModelId) = 0;
	virtual FString GetSessionId() const = 0;
	/** Says so when this backend is not the real thing (the offline demo). Empty otherwise. */
	virtual FString GetBanner() const { return FString(); }

	/** False, with a reason to show the user, when a turn cannot start (no key, no model, bad endpoint). */
	virtual bool CheckReady(FText& OutWhyNot) const = 0;
	virtual bool IsBusy() const = 0;

	/**
	 * Runs one user turn to completion: model round trips and tool calls until the model stops
	 * asking for tools. Every turn ends with exactly one Done or Error event.
	 */
	virtual void SendTurn(const FAgentTurnRequest& Request, FAgentEventSink Sink) = 0;

	/** True when the last turn failed or was stopped before the model answered, so it can be sent again. */
	virtual bool CanRetry() const = 0;
	/** Sends the conversation again as it stands, without adding a message. */
	virtual void Retry(FAgentEventSink Sink) = 0;

	/** Stops the running turn. It still ends with a Done event whose outcome is "cancelled". */
	virtual void Cancel() = 0;
	/** Forgets the conversation. Stops a running turn first. */
	virtual void ResetConversation() = 0;

	/** Models known without asking the provider: the last list fetched, kept between editor runs. */
	virtual TArray<FAgentModelInfo> GetCachedModels() const = 0;
	/** Asks the provider for its model list. The callback runs on the game thread. */
	virtual void RefreshModels(FAgentModelsCallback Callback) = 0;
};
