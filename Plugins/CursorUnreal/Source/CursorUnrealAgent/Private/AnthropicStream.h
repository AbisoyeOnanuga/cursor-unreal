#pragma once

#include "CoreMinimal.h"
#include "AgentBackend.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "SseParser.h"

/** A tool call the model made in one reply. */
struct FAnthropicToolUse
{
	FString Id;
	FString Name;
	/** Null when the arguments were not valid JSON, which happens when the reply was cut off mid-call. */
	TSharedPtr<FJsonObject> Input;
	FString RawInput;
};

/**
 * Turns the event stream of one Messages API reply into a finished assistant message.
 *
 * Feed it the response body as it arrives. It reports text and thinking as they stream, and
 * once the stream ends it holds the content blocks in the form the API expects to get back on
 * the next request. Thinking blocks in particular must be returned exactly as received, so
 * blocks of any type this code does not know are kept untouched rather than dropped.
 */
class FAnthropicStreamAssembler
{
public:
	/** Receives TextDelta, ThinkingDelta and ToolCallStarted events. */
	FAgentEventSink OnEvent;

	void Reset();
	void Feed(TConstArrayView<uint8> Bytes);

	bool SawMessageStart() const { return bSawMessageStart; }
	/** True once message_stop arrived: the reply is whole. */
	bool IsComplete() const { return bSawMessageStop; }
	/** True if anything was reported through OnEvent. */
	bool HasEmitted() const { return bEmitted; }
	bool HasOverflowed() const { return Parser.HasOverflowed(); }

	const FString& GetStopReason() const { return StopReason; }
	/** The refusal category when the stop reason is "refusal". */
	const FString& GetStopCategory() const { return StopCategory; }

	/** Set when the stream carried an error event. */
	bool HasError() const { return !ErrorType.IsEmpty(); }
	const FString& GetErrorType() const { return ErrorType; }
	const FString& GetErrorMessage() const { return ErrorMessage; }

	const FAgentUsage& GetUsage() const { return Usage; }

	/** Content blocks to send back as the assistant message, in order. Incomplete tool calls and empty text are left out. */
	TArray<TSharedPtr<FJsonValue>> BuildContent() const;
	const TArray<FAnthropicToolUse>& GetToolUses() const { return ToolUses; }

private:
	struct FBlock
	{
		FString Type;
		TSharedPtr<FJsonObject> Json;
		FString Text;
		FString Signature;
		FString PartialJson;
		bool bClosed = false;
		bool bValid = true;
	};

	void HandleEvent(const FSseEvent& Event);
	void ReadUsage(const TSharedPtr<FJsonObject>& UsageJson);
	void Emit(const FAgentEvent& Event);
	FBlock* FindBlock(int32 Index);

	FSseParser Parser;
	TMap<int32, FBlock> Blocks;
	TArray<FAnthropicToolUse> ToolUses;
	FAgentUsage Usage;
	FString StopReason;
	FString StopCategory;
	FString ErrorType;
	FString ErrorMessage;
	bool bSawMessageStart = false;
	bool bSawMessageStop = false;
	bool bEmitted = false;
	/** Bumped by Reset, so Feed can tell the assembler was reused while it was still handing out events. */
	uint32 Epoch = 0;
};
