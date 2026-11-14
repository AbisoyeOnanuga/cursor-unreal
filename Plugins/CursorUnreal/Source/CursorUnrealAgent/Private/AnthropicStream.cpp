#include "AnthropicStream.h"

#include "AgentJsonUtil.h"
#include "CursorJson.h"

void FAnthropicStreamAssembler::Reset()
{
	Parser.Reset();
	Blocks.Reset();
	ToolUses.Reset();
	Usage = FAgentUsage();
	StopReason.Reset();
	StopCategory.Reset();
	ErrorType.Reset();
	ErrorMessage.Reset();
	bSawMessageStart = false;
	bSawMessageStop = false;
	bEmitted = false;
	++Epoch;
}

void FAnthropicStreamAssembler::Feed(TConstArrayView<uint8> Bytes)
{
	TArray<FSseEvent> Events;
	Parser.Feed(Bytes, Events);
	const uint32 StartEpoch = Epoch;
	for (const FSseEvent& Event : Events)
	{
		HandleEvent(Event);
		// A listener can end the turn, and even start the next one, from inside an event. The
		// rest of this chunk then belongs to a reply nobody is waiting for.
		if (Epoch != StartEpoch)
		{
			break;
		}
	}
}

void FAnthropicStreamAssembler::Emit(const FAgentEvent& Event)
{
	bEmitted = true;
	if (OnEvent)
	{
		OnEvent(Event);
	}
}

FAnthropicStreamAssembler::FBlock* FAnthropicStreamAssembler::FindBlock(int32 Index)
{
	FBlock* Block = Blocks.Find(Index);
	return Block && !Block->bClosed ? Block : nullptr;
}

void FAnthropicStreamAssembler::ReadUsage(const TSharedPtr<FJsonObject>& UsageJson)
{
	if (!UsageJson.IsValid())
	{
		return;
	}
	// Counts are cumulative for the message, and a later event may repeat only some of them.
	double Number = 0.0;
	if (UsageJson->TryGetNumberField(TEXT("input_tokens"), Number))
	{
		Usage.InputTokens = static_cast<int64>(Number);
	}
	if (UsageJson->TryGetNumberField(TEXT("output_tokens"), Number))
	{
		Usage.OutputTokens = static_cast<int64>(Number);
	}
	if (UsageJson->TryGetNumberField(TEXT("cache_read_input_tokens"), Number))
	{
		Usage.CacheReadTokens = static_cast<int64>(Number);
	}
	if (UsageJson->TryGetNumberField(TEXT("cache_creation_input_tokens"), Number))
	{
		Usage.CacheWriteTokens = static_cast<int64>(Number);
	}
}

void FAnthropicStreamAssembler::HandleEvent(const FSseEvent& Event)
{
	const TSharedPtr<FJsonObject> Json = CursorJson::Parse(Event.Data);
	if (!Json.IsValid())
	{
		return;
	}
	// The data carries the same type as the event name; prefer it, since it is what the API documents.
	FString Type = AgentJson::Str(Json, TEXT("type"));
	if (Type.IsEmpty())
	{
		Type = Event.Event;
	}
	const int32 Index = static_cast<int32>(AgentJson::Num(Json, TEXT("index"), -1.0));

	if (Type == TEXT("content_block_delta"))
	{
		FBlock* Block = FindBlock(Index);
		const TSharedPtr<FJsonObject> Delta = AgentJson::Obj(Json, TEXT("delta"));
		if (!Block || !Delta.IsValid())
		{
			return;
		}
		const FString DeltaType = AgentJson::Str(Delta, TEXT("type"));
		if (DeltaType == TEXT("text_delta"))
		{
			const FString Piece = AgentJson::Str(Delta, TEXT("text"));
			Block->Text += Piece;
			if (!Piece.IsEmpty())
			{
				Emit(FAgentEvent::Make(EAgentEventType::TextDelta, Piece));
			}
		}
		else if (DeltaType == TEXT("thinking_delta"))
		{
			const FString Piece = AgentJson::Str(Delta, TEXT("thinking"));
			Block->Text += Piece;
			// With thinking display omitted the API sends one empty delta; there is nothing to show.
			if (!Piece.IsEmpty())
			{
				Emit(FAgentEvent::Make(EAgentEventType::ThinkingDelta, Piece));
			}
		}
		else if (DeltaType == TEXT("signature_delta"))
		{
			Block->Signature += AgentJson::Str(Delta, TEXT("signature"));
		}
		else if (DeltaType == TEXT("input_json_delta"))
		{
			Block->PartialJson += AgentJson::Str(Delta, TEXT("partial_json"));
		}
		// Other delta types belong to features this plugin does not switch on.
	}
	else if (Type == TEXT("content_block_start"))
	{
		const TSharedPtr<FJsonObject> Content = AgentJson::Obj(Json, TEXT("content_block"));
		if (!Content.IsValid() || Index < 0)
		{
			return;
		}
		FBlock& Block = Blocks.Add(Index);
		Block.Json = Content;
		Block.Type = AgentJson::Str(Content, TEXT("type"));
		// Blocks that arrive whole carry their content here rather than in deltas.
		if (Block.Type == TEXT("text"))
		{
			Block.Text = AgentJson::Str(Content, TEXT("text"));
		}
		else if (Block.Type == TEXT("thinking"))
		{
			Block.Text = AgentJson::Str(Content, TEXT("thinking"));
			Block.Signature = AgentJson::Str(Content, TEXT("signature"));
		}
		else if (Block.Type == TEXT("tool_use"))
		{
			FAgentEvent Started = FAgentEvent::Make(EAgentEventType::ToolCallStarted);
			Started.ToolCallId = AgentJson::Str(Content, TEXT("id"));
			Started.ToolName = AgentJson::Str(Content, TEXT("name"));
			Emit(Started);
		}
	}
	else if (Type == TEXT("content_block_stop"))
	{
		FBlock* Block = FindBlock(Index);
		if (!Block)
		{
			return;
		}
		Block->bClosed = true;
		if (Block->Type == TEXT("text"))
		{
			Block->Json->SetStringField(TEXT("text"), Block->Text);
		}
		else if (Block->Type == TEXT("thinking"))
		{
			Block->Json->SetStringField(TEXT("thinking"), Block->Text);
			Block->Json->SetStringField(TEXT("signature"), Block->Signature);
		}
		else if (Block->Type == TEXT("tool_use"))
		{
			FAnthropicToolUse& Use = ToolUses.AddDefaulted_GetRef();
			Use.Id = AgentJson::Str(Block->Json, TEXT("id"));
			Use.Name = AgentJson::Str(Block->Json, TEXT("name"));
			Use.RawInput = Block->PartialJson;
			if (Block->PartialJson.TrimStartAndEnd().IsEmpty())
			{
				// No deltas means no arguments. Keep whatever the start event carried, normally {}.
				Use.Input = AgentJson::Obj(Block->Json, TEXT("input"));
				if (!Use.Input.IsValid())
				{
					Use.Input = MakeShared<FJsonObject>();
				}
			}
			else
			{
				Use.Input = CursorJson::Parse(Block->PartialJson);
			}
			Block->bValid = Use.Input.IsValid();
			if (Block->bValid)
			{
				Block->Json->SetObjectField(TEXT("input"), Use.Input);
			}
		}
	}
	else if (Type == TEXT("message_start"))
	{
		bSawMessageStart = true;
		ReadUsage(AgentJson::Obj(AgentJson::Obj(Json, TEXT("message")), TEXT("usage")));
	}
	else if (Type == TEXT("message_delta"))
	{
		const TSharedPtr<FJsonObject> Delta = AgentJson::Obj(Json, TEXT("delta"));
		const FString Reason = AgentJson::Str(Delta, TEXT("stop_reason"));
		if (!Reason.IsEmpty())
		{
			StopReason = Reason;
		}
		const FString Category = AgentJson::Str(AgentJson::Obj(Delta, TEXT("stop_details")), TEXT("category"));
		if (!Category.IsEmpty())
		{
			StopCategory = Category;
		}
		ReadUsage(AgentJson::Obj(Json, TEXT("usage")));
	}
	else if (Type == TEXT("message_stop"))
	{
		bSawMessageStop = true;
	}
	else if (Type == TEXT("error"))
	{
		const TSharedPtr<FJsonObject> Error = AgentJson::Obj(Json, TEXT("error"));
		ErrorType = AgentJson::Str(Error, TEXT("type"));
		ErrorMessage = AgentJson::Str(Error, TEXT("message"));
		if (ErrorType.IsEmpty())
		{
			ErrorType = TEXT("error");
		}
	}
	// "ping" and event types added later are ignored, as the API's versioning policy asks.
}

TArray<TSharedPtr<FJsonValue>> FAnthropicStreamAssembler::BuildContent() const
{
	TArray<int32> Indices;
	Blocks.GetKeys(Indices);
	Indices.Sort();

	TArray<TSharedPtr<FJsonValue>> Content;
	for (const int32 Index : Indices)
	{
		const FBlock& Block = Blocks[Index];
		if (!Block.bClosed || !Block.bValid || !Block.Json.IsValid())
		{
			// Cut off mid-block. A half-written block cannot be sent back.
			continue;
		}
		if (Block.Type == TEXT("text") && Block.Text.IsEmpty())
		{
			// The API rejects empty text blocks on input.
			continue;
		}
		Content.Add(MakeShared<FJsonValueObject>(Block.Json));
	}
	return Content;
}
