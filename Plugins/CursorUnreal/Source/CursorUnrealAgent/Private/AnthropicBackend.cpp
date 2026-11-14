#include "AnthropicBackend.h"

#include "AgentJsonUtil.h"
#include "AgentSystemPrompt.h"
#include "Containers/Ticker.h"
#include "CursorJson.h"
#include "CursorSecrets.h"
#include "CursorToolRegistry.h"
#include "CursorUnrealLog.h"
#include "CursorUnrealSettings.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace
{
	/** Enough to hold any error body; the rest of a streamed reply is not kept twice. */
	constexpr int32 RawHeadLimit = 64 * 1024;
	/** Waiting longer than this for a retry is worse than telling the user. */
	constexpr float MaxRetryWaitSeconds = 60.0f;
	constexpr int32 MaxModelPages = 10;

	const TCHAR* EffortNames[] = { TEXT("low"), TEXT("medium"), TEXT("high"), TEXT("xhigh"), TEXT("max") };

	/** The statuses Anthropic's own SDKs send again: timeouts, conflicts, rate limits and server faults. */
	bool IsRetryableStatus(int32 Status)
	{
		return Status == 408 || Status == 409 || Status == 429 || Status >= 500;
	}

	bool IsRetryableStreamError(const FString& Type)
	{
		return Type == TEXT("overloaded_error") || Type == TEXT("api_error") || Type == TEXT("rate_limit_error") || Type == TEXT("timeout_error");
	}

	FString DescribeApiError(int32 Status, const FString& ApiMessage, const FString& RequestId)
	{
		FString Text;
		switch (Status)
		{
		case 401: Text = TEXT("Anthropic rejected the API key (401). Check the key in the Setup tab."); break;
		case 402: Text = TEXT("Anthropic reports a billing problem on this account (402)."); break;
		case 403: Text = TEXT("This API key is not allowed to use that model or feature (403)."); break;
		case 404: Text = TEXT("Anthropic did not recognise the request (404). The model id or the endpoint setting may be wrong."); break;
		case 413: Text = TEXT("The request is too large for the API (413). Start a new chat to shorten the conversation."); break;
		case 429: Text = TEXT("Anthropic is rate limiting this account (429)."); break;
		case 529: Text = TEXT("Anthropic is overloaded (529)."); break;
		default:
			Text = Status >= 500
				? FString::Printf(TEXT("Anthropic had an internal error (%d)."), Status)
				: FString::Printf(TEXT("Anthropic rejected the request (%d)."), Status);
			break;
		}
		if (!ApiMessage.IsEmpty())
		{
			Text += TEXT(" It said: ") + ApiMessage;
		}
		if (!RequestId.IsEmpty())
		{
			Text += FString::Printf(TEXT(" [request %s]"), *RequestId);
		}
		return Text;
	}

	FString DecodeUtf8(const TArray<uint8>& Bytes)
	{
		if (Bytes.Num() == 0)
		{
			return FString();
		}
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(FStringView(Converted.Get(), Converted.Length()));
	}

	FString MessageRole(const TSharedPtr<FJsonValue>& Message)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Message.IsValid() && Message->TryGetObject(Object) ? AgentJson::Str(*Object, TEXT("role")) : FString();
	}

	TSharedRef<FJsonObject> EphemeralCache()
	{
		TSharedRef<FJsonObject> Cache = MakeShared<FJsonObject>();
		Cache->SetStringField(TEXT("type"), TEXT("ephemeral"));
		return Cache;
	}

	TSharedRef<FJsonValueObject> TextBlock(const FString& Text)
	{
		TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("text"));
		Block->SetStringField(TEXT("text"), Text);
		return MakeShared<FJsonValueObject>(Block);
	}

	TSharedRef<FJsonValueObject> ImageBlock(const FCursorImage& Image)
	{
		TSharedRef<FJsonObject> Source = MakeShared<FJsonObject>();
		Source->SetStringField(TEXT("type"), TEXT("base64"));
		Source->SetStringField(TEXT("media_type"), Image.MimeType);
		Source->SetStringField(TEXT("data"), Image.ToBase64());
		TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
		Block->SetStringField(TEXT("type"), TEXT("image"));
		Block->SetObjectField(TEXT("source"), Source);
		return MakeShared<FJsonValueObject>(Block);
	}

	bool IsImageBlock(const TSharedPtr<FJsonValue>& Block)
	{
		const TSharedPtr<FJsonObject>* Object = nullptr;
		return Block.IsValid() && Block->TryGetObject(Object) && AgentJson::Str(*Object, TEXT("type")) == TEXT("image");
	}

	const TCHAR* RemovedImageText = TEXT("[A picture that was here has been dropped from the conversation to keep it small. Ask for it again, or take a new screenshot, if you need to see it.]");

	/**
	 * Calls Visit for every array of content blocks that can hold a picture: the content of each
	 * user message, and the content of each tool result inside one. Visit may change the array.
	 */
	void ForEachImageHolder(const TArray<TSharedPtr<FJsonValue>>& History, TFunctionRef<void(TArray<TSharedPtr<FJsonValue>>&)> Visit)
	{
		for (const TSharedPtr<FJsonValue>& Message : History)
		{
			const TSharedPtr<FJsonObject>* MessageObject = nullptr;
			if (!Message.IsValid() || !Message->TryGetObject(MessageObject) || AgentJson::Str(*MessageObject, TEXT("role")) != TEXT("user"))
			{
				continue;
			}
			const TArray<TSharedPtr<FJsonValue>>* Content = nullptr;
			if (!(*MessageObject)->TryGetArrayField(TEXT("content"), Content))
			{
				continue;
			}
			TArray<TSharedPtr<FJsonValue>> Blocks = *Content;
			for (const TSharedPtr<FJsonValue>& Block : Blocks)
			{
				const TSharedPtr<FJsonObject>* BlockObject = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Inner = nullptr;
				if (Block.IsValid() && Block->TryGetObject(BlockObject) && AgentJson::Str(*BlockObject, TEXT("type")) == TEXT("tool_result")
					&& (*BlockObject)->TryGetArrayField(TEXT("content"), Inner))
				{
					TArray<TSharedPtr<FJsonValue>> InnerBlocks = *Inner;
					Visit(InnerBlocks);
					(*BlockObject)->SetArrayField(TEXT("content"), InnerBlocks);
				}
			}
			Visit(Blocks);
			(*MessageObject)->SetArrayField(TEXT("content"), Blocks);
		}
	}

	/** The editor tools the in-editor agent may use, in the shape the Messages API takes. Sorted by name, so the order is stable. */
	TArray<TSharedPtr<FJsonValue>> DescribeTools()
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const TSharedRef<ICursorTool>& Tool : FCursorToolRegistry::Get().GetTools(FCursorToolRegistry::CallerNative))
		{
			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Tool->GetName().ToString());
			Entry->SetStringField(TEXT("description"), Tool->GetDescription().ToString());
			Entry->SetObjectField(TEXT("input_schema"), Tool->GetInputSchema());
			Out.Add(MakeShared<FJsonValueObject>(Entry));
		}
		return Out;
	}

	FString Fingerprint(const TArray<TSharedPtr<FJsonValue>>& Tools)
	{
		return FMD5::HashAnsiString(*CursorJson::ToString(Tools));
	}
}

FAnthropicBackend::FAnthropicBackend(FAnthropicBackendOptions InOptions)
	: Options(MoveTemp(InOptions))
	, SessionId(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower))
{
	if (!Options.Transport.IsValid())
	{
		Options.Transport = CursorAgent::MakeEngineHttpTransport();
	}
	if (!Options.KeyProvider)
	{
		Options.KeyProvider = []() { return CursorSecrets::ResolveAnthropicKey(); };
	}
	if (!Options.Scheduler)
	{
		Options.Scheduler = [](float Seconds, TFunction<void()> Callback)
		{
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
				[Callback = MoveTemp(Callback)](float)
				{
					Callback();
					return false;
				}), Seconds);
		};
	}

	// The assembler is a member, so it cannot outlive this object.
	Assembler.OnEvent = [this](const FAgentEvent& Event) { Emit(Event); };

	if (Options.FixedModels.Num() > 0)
	{
		Models = Options.FixedModels;
		Options.bPersistModels = false;
	}
	else if (Options.bPersistModels)
	{
		LoadModelCache();
	}
}

FAnthropicBackend::~FAnthropicBackend()
{
	if (ActiveCall.IsValid())
	{
		ActiveCall->Cancel();
	}
	if (ModelsCall.IsValid())
	{
		ModelsCall->Cancel();
	}
}

// --- Configuration -------------------------------------------------------------------------------

FString FAnthropicBackend::GetModelId() const
{
	return Options.ModelOverride.IsEmpty() ? UCursorUnrealSettings::Get()->AnthropicModel.TrimStartAndEnd() : Options.ModelOverride;
}

void FAnthropicBackend::SetModelId(const FString& ModelId)
{
	if (!Options.ModelOverride.IsEmpty())
	{
		Options.ModelOverride = ModelId;
		return;
	}
	UCursorUnrealSettings* Settings = UCursorUnrealSettings::GetMutable();
	Settings->AnthropicModel = ModelId.TrimStartAndEnd();
	Settings->SaveConfig();
}

const FAgentModelInfo* FAnthropicBackend::FindModel(const FString& Id) const
{
	return Models.FindByPredicate([&Id](const FAgentModelInfo& Model) { return Model.Id == Id; });
}

FString FAnthropicBackend::ApiUrl(const TCHAR* Path) const
{
	FString Base = UCursorUnrealSettings::Get()->AnthropicBaseUrl.TrimStartAndEnd();
	while (Base.EndsWith(TEXT("/")))
	{
		Base.LeftChopInline(1);
	}
	// Accept an endpoint written with or without the version segment.
	if (Base.EndsWith(TEXT("/v1"), ESearchCase::IgnoreCase))
	{
		Base.LeftChopInline(3);
	}
	return Base + Path;
}

TMap<FString, FString> FAnthropicBackend::AuthHeaders() const
{
	TMap<FString, FString> Headers;
	Headers.Add(TEXT("x-api-key"), Options.KeyProvider());
	Headers.Add(TEXT("anthropic-version"), UCursorUnrealSettings::Get()->AnthropicApiVersion.TrimStartAndEnd());
	Headers.Add(TEXT("content-type"), TEXT("application/json"));
	return Headers;
}

bool FAnthropicBackend::CheckReady(FText& OutWhyNot) const
{
	const UCursorUnrealSettings* Settings = UCursorUnrealSettings::Get();

	const FString UrlProblem = UCursorUnrealSettings::ValidateBaseUrl(Settings->AnthropicBaseUrl);
	if (!UrlProblem.IsEmpty())
	{
		OutWhyNot = FText::FromString(UrlProblem);
		return false;
	}
	if (Settings->AnthropicApiVersion.TrimStartAndEnd().IsEmpty())
	{
		OutWhyNot = FText::FromString(TEXT("The Anthropic API version setting is empty."));
		return false;
	}
	if (Options.KeyProvider().IsEmpty())
	{
		OutWhyNot = FText::FromString(TEXT("No Anthropic API key. Add one in the Setup tab, or set ANTHROPIC_API_KEY before starting the editor."));
		return false;
	}
	if (GetModelId().IsEmpty())
	{
		OutWhyNot = FText::FromString(TEXT("No model chosen. Pick one from the model list."));
		return false;
	}
	return true;
}

// --- Conversation --------------------------------------------------------------------------------

void FAnthropicBackend::StartConversationIfNeeded()
{
	if (bConversationStarted)
	{
		return;
	}
	bConversationStarted = true;
	bWarnedToolsChanged = false;
	bWarnedEffort = false;

	SystemPrompt = CursorAgent::BuildSystemPrompt();

	ToolsJson = DescribeTools();
	ToolsFingerprint = Fingerprint(ToolsJson);
}

void FAnthropicBackend::AppendUserContent(const TArray<TSharedPtr<FJsonValue>>& Blocks)
{
	if (Blocks.Num() == 0)
	{
		return;
	}
	// Roles must alternate. After a stopped or failed turn the last message is still the user's, so add to it.
	if (History.Num() > 0 && MessageRole(History.Last()) == TEXT("user"))
	{
		const TSharedPtr<FJsonObject> Last = History.Last()->AsObject();
		TArray<TSharedPtr<FJsonValue>> Content = Last->GetArrayField(TEXT("content"));
		Content.Append(Blocks);
		Last->SetArrayField(TEXT("content"), Content);
		return;
	}
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("role"), TEXT("user"));
	Message->SetArrayField(TEXT("content"), Blocks);
	History.Add(MakeShared<FJsonValueObject>(Message));
}

int32 FAnthropicBackend::NumHistoryImages() const
{
	int32 Count = 0;
	ForEachImageHolder(History, [&Count](TArray<TSharedPtr<FJsonValue>>& Blocks)
	{
		for (const TSharedPtr<FJsonValue>& Block : Blocks)
		{
			Count += IsImageBlock(Block) ? 1 : 0;
		}
	});
	return Count;
}

void FAnthropicBackend::PruneImages()
{
	const int32 Count = NumHistoryImages();
	if (Count <= CursorAgent::MaxImagesInHistory)
	{
		return;
	}
	// Oldest first: the conversation is visited in order.
	int32 ToDrop = Count - CursorAgent::KeepImagesInHistory;
	const int32 Dropped = ToDrop;
	ForEachImageHolder(History, [&ToDrop](TArray<TSharedPtr<FJsonValue>>& Blocks)
	{
		for (TSharedPtr<FJsonValue>& Block : Blocks)
		{
			if (ToDrop > 0 && IsImageBlock(Block))
			{
				Block = TextBlock(RemovedImageText);
				--ToDrop;
			}
		}
	});
	EmitNotice(FString::Printf(TEXT("The %d oldest pictures were dropped from what the model is sent, to keep the conversation small. It still has the newest %d. Your saved copies are untouched."),
		Dropped, CursorAgent::KeepImagesInHistory));
}

void FAnthropicBackend::SendTurn(const FAgentTurnRequest& Request, FAgentEventSink InSink)
{
	check(IsInGameThread());
	if (IsBusy())
	{
		InSink(FAgentEvent::Make(EAgentEventType::Error, TEXT("A turn is already running.")));
		return;
	}
	FText WhyNot;
	if (!CheckReady(WhyNot))
	{
		InSink(FAgentEvent::Make(EAgentEventType::Error, WhyNot.ToString()));
		return;
	}
	const FString Text = Request.Text.TrimStartAndEnd();
	if (Text.IsEmpty())
	{
		InSink(FAgentEvent::Make(EAgentEventType::Error, TEXT("The message is empty.")));
		return;
	}

	const FString ImageProblem = CursorAgent::CheckMessageImages(Request.Images);
	if (!ImageProblem.IsEmpty())
	{
		InSink(FAgentEvent::Make(EAgentEventType::Error, ImageProblem));
		return;
	}

	StartConversationIfNeeded();

	// Reference material and pictures first and the request last, which is where models attend to it best.
	TArray<TSharedPtr<FJsonValue>> Blocks;
	for (const FAgentContextBlock& Context : Request.Context)
	{
		if (!Context.Text.TrimStartAndEnd().IsEmpty())
		{
			Blocks.Add(TextBlock(FString::Printf(TEXT("<context name=\"%s\">\n%s\n</context>"), *Context.Title, *Context.Text)));
		}
	}
	for (int32 Index = 0; Index < Request.Images.Num(); ++Index)
	{
		Blocks.Add(TextBlock(CursorAgent::DescribeAttachedImage(Request.Images[Index], Index, Request.Images.Num())));
		Blocks.Add(ImageBlock(Request.Images[Index]));
	}
	Blocks.Add(TextBlock(Text));
	AppendUserContent(Blocks);

	BeginTurn(MoveTemp(InSink));
}

bool FAnthropicBackend::CanRetry() const
{
	return Phase == EPhase::Idle && History.Num() > 0 && MessageRole(History.Last()) == TEXT("user");
}

void FAnthropicBackend::Retry(FAgentEventSink InSink)
{
	check(IsInGameThread());
	if (!CanRetry())
	{
		InSink(FAgentEvent::Make(EAgentEventType::Error, TEXT("There is nothing to send again.")));
		return;
	}
	FText WhyNot;
	if (!CheckReady(WhyNot))
	{
		InSink(FAgentEvent::Make(EAgentEventType::Error, WhyNot.ToString()));
		return;
	}
	BeginTurn(MoveTemp(InSink));
}

void FAnthropicBackend::BeginTurn(FAgentEventSink InSink)
{
	Sink = MakeShared<FAgentEventSink>(MoveTemp(InSink));
	Iteration = 0;
	Attempt = 0;
	bCancelRequested = false;
	TurnUsage = FAgentUsage();
	ToolImagesThisTurn = 0;
	OutcomeAfterTools = nullptr;
	PruneImages();

	if (!bWarnedToolsChanged && Fingerprint(DescribeTools()) != ToolsFingerprint)
	{
		bWarnedToolsChanged = true;
		EmitNotice(TEXT("The available tools changed since this chat began. This chat keeps the set it started with; start a new chat to use the new one."));
	}

	StartRequest();
}

FString FAnthropicBackend::BuildRequestBody(TArray<FString>& OutNotices) const
{
	const UCursorUnrealSettings* Settings = UCursorUnrealSettings::Get();
	const FString ModelId = GetModelId();
	const FAgentModelInfo* Model = FindModel(ModelId);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("model"), ModelId);

	int32 MaxTokens = Settings->MaxTokens;
	if (Model && Model->MaxOutputTokens > 0)
	{
		MaxTokens = FMath::Min(MaxTokens, Model->MaxOutputTokens);
	}
	Root->SetNumberField(TEXT("max_tokens"), MaxTokens);
	Root->SetBoolField(TEXT("stream"), true);

	// Two cache points. The one on the system block covers tools and system prompt, which never
	// change within a conversation. The top-level one tells the API to cache up to the newest
	// message, so each round trip of a tool loop re-reads everything before it at the cached rate.
	Root->SetObjectField(TEXT("cache_control"), EphemeralCache());

	TSharedRef<FJsonObject> SystemBlock = MakeShared<FJsonObject>();
	SystemBlock->SetStringField(TEXT("type"), TEXT("text"));
	SystemBlock->SetStringField(TEXT("text"), SystemPrompt);
	SystemBlock->SetObjectField(TEXT("cache_control"), EphemeralCache());
	TArray<TSharedPtr<FJsonValue>> System;
	System.Add(MakeShared<FJsonValueObject>(SystemBlock));
	Root->SetArrayField(TEXT("system"), System);

	if (ToolsJson.Num() > 0)
	{
		Root->SetArrayField(TEXT("tools"), ToolsJson);
	}
	Root->SetArrayField(TEXT("messages"), History);

	// Thinking and effort are only sent when the model is known to accept them. Sending a mode a
	// model does not support is a 400, and which modes exist differs from model to model, so the
	// model list is the authority rather than anything written here. Left out, each model uses
	// its own default.
	if (Model && Model->bAdaptiveThinking)
	{
		TSharedRef<FJsonObject> Thinking = MakeShared<FJsonObject>();
		Thinking->SetStringField(TEXT("type"), TEXT("adaptive"));
		Thinking->SetStringField(TEXT("display"), Settings->bShowThinking ? TEXT("summarized") : TEXT("omitted"));
		Root->SetObjectField(TEXT("thinking"), Thinking);
	}

	if (Settings->Effort != ECursorEffort::ModelDefault)
	{
		const FString Effort = LexToString(Settings->Effort);
		if (!Model)
		{
			OutNotices.Add(FString::Printf(TEXT("The effort setting was not sent: what %s supports is not known yet. Refresh the model list."), *ModelId));
		}
		else if (!Model->EffortLevels.Contains(Effort))
		{
			OutNotices.Add(FString::Printf(TEXT("%s does not offer effort level \"%s\", so its default is used."), *Model->DisplayName, *Effort));
		}
		else
		{
			TSharedRef<FJsonObject> OutputConfig = MakeShared<FJsonObject>();
			OutputConfig->SetStringField(TEXT("effort"), Effort);
			Root->SetObjectField(TEXT("output_config"), OutputConfig);
		}
	}

	return CursorJson::ToString(Root);
}

// --- Model round trip ----------------------------------------------------------------------------

void FAnthropicBackend::StartRequest()
{
	Phase = EPhase::Requesting;
	Assembler.Reset();
	RawHead.Reset();

	TArray<FString> Notices;
	FAgentHttpRequest Request;
	Request.Verb = TEXT("POST");
	Request.Url = ApiUrl(TEXT("/v1/messages"));
	Request.Headers = AuthHeaders();
	Request.Headers.Add(TEXT("accept"), TEXT("text/event-stream"));
	Request.Body = BuildRequestBody(Notices);
	Request.bStream = true;

	if (!bWarnedEffort && Notices.Num() > 0)
	{
		bWarnedEffort = true;
		for (const FString& Notice : Notices)
		{
			EmitNotice(Notice);
		}
	}

	Emit(FAgentEvent::Make(EAgentEventType::RequestStarted));

	const TWeakPtr<FAnthropicBackend> Weak = AsShared();
	const uint32 ExpectedGeneration = Generation;
	ActiveCall = Options.Transport->Send(Request,
		[Weak, ExpectedGeneration](TConstArrayView<uint8> Bytes)
		{
			const TSharedPtr<FAnthropicBackend> Self = Weak.Pin();
			if (!Self.IsValid() || Self->Generation != ExpectedGeneration || Self->Phase != EPhase::Requesting)
			{
				return;
			}
			const int32 Room = RawHeadLimit - Self->RawHead.Num();
			if (Room > 0)
			{
				Self->RawHead.Append(Bytes.GetData(), FMath::Min(Room, Bytes.Num()));
			}
			Self->Assembler.Feed(Bytes);
		},
		[Weak, ExpectedGeneration](const FAgentHttpResponse& Response)
		{
			const TSharedPtr<FAnthropicBackend> Self = Weak.Pin();
			if (Self.IsValid() && Self->Generation == ExpectedGeneration && Self->Phase == EPhase::Requesting)
			{
				Self->HandleResponse(Response);
			}
		});
}

void FAnthropicBackend::HandleResponse(const FAgentHttpResponse& Response)
{
	ActiveCall.Reset();

	float RetryAfter = -1.0f;
	if (const FString* Header = Response.Headers.Find(TEXT("retry-after")))
	{
		if (Header->IsNumeric())
		{
			RetryAfter = FCString::Atof(**Header);
		}
	}
	const FString RequestId = Response.Headers.FindRef(TEXT("request-id"));

	if (Response.Status == 0)
	{
		RetryOrFail(FString::Printf(TEXT("Could not reach the Anthropic API (%s)."),
			Response.FailureReason.IsEmpty() ? TEXT("no response") : *Response.FailureReason), -1.0f);
		return;
	}

	if (Response.Status != 200)
	{
		// An error reply is one JSON object. For a streamed request it arrived through the chunk callback.
		const FString BodyText = Response.Body.IsEmpty() ? DecodeUtf8(RawHead) : Response.Body;
		const TSharedPtr<FJsonObject> Body = CursorJson::Parse(BodyText);
		const FString ApiMessage = AgentJson::Str(AgentJson::Obj(Body, TEXT("error")), TEXT("message"));
		const FString Message = DescribeApiError(Response.Status, ApiMessage, RequestId);

		UE_LOG(LogCursorUnreal, Warning, TEXT("Anthropic request failed with status %d (request %s)."), Response.Status, RequestId.IsEmpty() ? TEXT("unknown") : *RequestId);
		if (IsRetryableStatus(Response.Status))
		{
			RetryOrFail(Message, RetryAfter);
		}
		else
		{
			Fail(Message);
		}
		return;
	}

	if (Assembler.HasOverflowed())
	{
		Fail(TEXT("The reply contained a single event too large to handle, so it was abandoned."));
		return;
	}

	if (Assembler.HasError())
	{
		// Errors can arrive inside a stream that began with 200.
		const FString Message = FString::Printf(TEXT("Anthropic reported an error part-way through the reply (%s). %s"),
			*Assembler.GetErrorType(), *Assembler.GetErrorMessage());
		if (IsRetryableStreamError(Assembler.GetErrorType()))
		{
			RetryOrFail(Message, RetryAfter);
		}
		else
		{
			Fail(Message);
		}
		return;
	}

	if (!Assembler.SawMessageStart())
	{
		if (RawHead.Num() > 0 || !Response.Body.IsEmpty())
		{
			Fail(TEXT("The endpoint answered, but not with a Messages API event stream. Check the Anthropic endpoint setting."));
		}
		else
		{
			RetryOrFail(TEXT("The Anthropic API returned an empty reply."), -1.0f);
		}
		return;
	}

	if (!Assembler.IsComplete())
	{
		RetryOrFail(TEXT("The connection to Anthropic dropped before the reply finished."), -1.0f);
		return;
	}

	CommitReply();
}

void FAnthropicBackend::RetryOrFail(const FString& Reason, float RetryAfterSeconds)
{
	if (Attempt >= Options.MaxRetries)
	{
		Fail(Attempt > 0 ? FString::Printf(TEXT("%s Gave up after %d attempts."), *Reason, Attempt + 1) : Reason);
		return;
	}
	if (RetryAfterSeconds > MaxRetryWaitSeconds)
	{
		Fail(FString::Printf(TEXT("%s The service asked for a wait of %d seconds before trying again."), *Reason, FMath::CeilToInt(RetryAfterSeconds)));
		return;
	}

	++Attempt;
	// Exponential backoff with jitter, unless the service said how long to wait.
	const float Backoff = FMath::Min(8.0f, static_cast<float>(1 << (Attempt - 1))) * FMath::FRandRange(1.0f, 1.25f);
	const float Delay = RetryAfterSeconds >= 0.0f ? RetryAfterSeconds : Backoff;

	// Whatever this attempt showed is not part of the conversation and is about to be produced again.
	if (Assembler.HasEmitted())
	{
		Emit(FAgentEvent::Make(EAgentEventType::Rewind));
	}
	EmitNotice(FString::Printf(TEXT("%s Trying again in %d s (attempt %d of %d)."), *Reason, FMath::CeilToInt(Delay), Attempt + 1, Options.MaxRetries + 1));

	Phase = EPhase::WaitingToRetry;
	const TWeakPtr<FAnthropicBackend> Weak = AsShared();
	const uint32 ExpectedGeneration = Generation;
	Options.Scheduler(Delay, [Weak, ExpectedGeneration]()
	{
		const TSharedPtr<FAnthropicBackend> Self = Weak.Pin();
		if (Self.IsValid() && Self->Generation == ExpectedGeneration && Self->Phase == EPhase::WaitingToRetry)
		{
			Self->StartRequest();
		}
	});
}

void FAnthropicBackend::CommitReply()
{
	Attempt = 0;

	const FAgentUsage& Usage = Assembler.GetUsage();
	TurnUsage.InputTokens += Usage.InputTokens;
	TurnUsage.OutputTokens += Usage.OutputTokens;
	TurnUsage.CacheReadTokens += Usage.CacheReadTokens;
	TurnUsage.CacheWriteTokens += Usage.CacheWriteTokens;
	EmitUsage();

	const FString Reason = Assembler.GetStopReason();

	if (Reason == TEXT("refusal"))
	{
		// A refused reply is not added to the conversation, so the same refusal is not replayed on every later request.
		const FString Category = Assembler.GetStopCategory();
		EmitNotice(FString::Printf(TEXT("The model declined to answer%s. Rephrase the request, or start a new chat."),
			Category.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Category)));
		Finish(TEXT("refused"));
		return;
	}

	const TArray<TSharedPtr<FJsonValue>> Content = Assembler.BuildContent();
	if (Content.Num() > 0)
	{
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("role"), TEXT("assistant"));
		Message->SetArrayField(TEXT("content"), Content);
		History.Add(MakeShared<FJsonValueObject>(Message));
	}

	// Tool calls that made it into the conversation. Each one must be answered, or the next request is invalid.
	TArray<FAnthropicToolUse> Calls;
	for (const FAnthropicToolUse& Use : Assembler.GetToolUses())
	{
		if (Use.Input.IsValid())
		{
			Calls.Add(Use);
		}
	}

	const bool bCutOffByLength = Reason == TEXT("max_tokens");
	const bool bCutOffByContext = Reason == TEXT("model_context_window_exceeded");
	if (bCutOffByLength)
	{
		EmitNotice(TEXT("The reply reached the Max Tokens limit and was cut off. Raise the limit in the settings, or ask the model to continue."));
	}
	else if (bCutOffByContext)
	{
		EmitNotice(TEXT("The conversation has filled the model's context window, so the reply was cut off. Start a new chat to carry on."));
	}

	if (Calls.Num() > 0)
	{
		PendingTools = MoveTemp(Calls);
		// A reply that was cut off may be missing calls the model meant to make alongside these, so none of them run.
		OutcomeAfterTools = (bCutOffByLength || bCutOffByContext) ? TEXT("truncated") : nullptr;
		RunTools();
		return;
	}

	if (bCutOffByLength || bCutOffByContext)
	{
		Finish(TEXT("truncated"));
	}
	else if (Reason == TEXT("tool_use"))
	{
		Fail(TEXT("The model asked for a tool, but the call could not be read."));
	}
	else if (Reason == TEXT("pause_turn"))
	{
		// The model paused a long turn. Sending the conversation back as it stands lets it carry on.
		++Iteration;
		if (Iteration >= UCursorUnrealSettings::Get()->MaxToolIterations)
		{
			EmitNotice(TEXT("Stopped at the round-trip limit from the settings."));
			Finish(TEXT("max_iterations"));
		}
		else
		{
			StartRequest();
		}
	}
	else
	{
		if (Reason != TEXT("end_turn") && Reason != TEXT("stop_sequence") && !Reason.IsEmpty())
		{
			EmitNotice(FString::Printf(TEXT("The model stopped for a reason this plugin does not know (%s)."), *Reason));
		}
		Finish(TEXT("done"));
	}
}

// --- Tools ---------------------------------------------------------------------------------------

void FAnthropicBackend::RunTools()
{
	Phase = EPhase::RunningTools;
	ToolResults.Reset();
	NextTool = 0;
	RunNextTool();
}

void FAnthropicBackend::RunNextTool()
{
	// Tools run one after another, in the order the model asked: later calls often depend on earlier ones having happened.
	if (NextTool >= PendingTools.Num())
	{
		FinishTools();
		return;
	}

	const FAnthropicToolUse& Use = PendingTools[NextTool];
	const int32 Index = NextTool;

	if (OutcomeAfterTools != nullptr)
	{
		HandleToolResult(Index, true, TEXT("Not run: the reply that asked for this was cut off before it finished."));
		return;
	}
	if (bCancelRequested)
	{
		HandleToolResult(Index, true, TEXT("Not run: the user stopped the turn."));
		return;
	}

	// Copies, not references into PendingTools: a tool that finishes before Invoke returns ends the batch and clears the array.
	const FName ToolName(*Use.Name);
	const TSharedPtr<FJsonObject> Input = Use.Input;

	FAgentEvent Running = FAgentEvent::Make(EAgentEventType::ToolCallRunning);
	Running.ToolCallId = Use.Id;
	Running.ToolName = Use.Name;
	Running.ArgsJson = CursorJson::ToString(Input.ToSharedRef());

	FCursorToolContext Context;
	Context.Caller = FCursorToolRegistry::CallerNative;
	Context.SessionId = SessionId;
	Context.CallId = Use.Id;

	Emit(Running);

	const TWeakPtr<FAnthropicBackend> Weak = AsShared();
	const uint32 ExpectedGeneration = Generation;
	FCursorToolRegistry::Get().Invoke(ToolName, Input, Context,
		[Weak, ExpectedGeneration, Index](const FCursorToolResult& Result)
		{
			const TSharedPtr<FAnthropicBackend> Self = Weak.Pin();
			// A tool cannot be interrupted. If the conversation was reset while it ran, its result has nowhere to go.
			if (Self.IsValid() && Self->Generation == ExpectedGeneration && Self->Phase == EPhase::RunningTools && Self->NextTool == Index)
			{
				Self->HandleToolResult(Index, Result.bIsError, Result.Text, Result.Images);
			}
		});
}

void FAnthropicBackend::HandleToolResult(int32 Index, bool bIsError, const FString& Text, const TArray<FCursorImage>& Images)
{
	const FAnthropicToolUse& Use = PendingTools[Index];
	// The API does not accept an empty result.
	FString Content = Text.IsEmpty() ? TEXT("(no output)") : Text;
	const TArray<FCursorImage> Sent = CursorAgent::TakeToolImages(Images, ToolImagesThisTurn, Content);

	TSharedRef<FJsonObject> Block = MakeShared<FJsonObject>();
	Block->SetStringField(TEXT("type"), TEXT("tool_result"));
	Block->SetStringField(TEXT("tool_use_id"), Use.Id);
	if (Sent.Num() > 0)
	{
		TArray<TSharedPtr<FJsonValue>> Parts;
		Parts.Add(TextBlock(Content));
		for (const FCursorImage& Image : Sent)
		{
			Parts.Add(ImageBlock(Image));
		}
		Block->SetArrayField(TEXT("content"), Parts);
	}
	else
	{
		Block->SetStringField(TEXT("content"), Content);
	}
	if (bIsError)
	{
		Block->SetBoolField(TEXT("is_error"), true);
	}
	ToolResults.Add(MakeShared<FJsonValueObject>(Block));

	FAgentEvent Finished = FAgentEvent::Make(EAgentEventType::ToolCallFinished, Content);
	Finished.ToolCallId = Use.Id;
	Finished.ToolName = Use.Name;
	Finished.bIsError = bIsError;
	Finished.Images = Sent;
	Emit(Finished);

	++NextTool;
	RunNextTool();
}

void FAnthropicBackend::FinishTools()
{
	// All results for one reply go back together in a single user message.
	AppendUserContent(ToolResults);
	ToolResults.Reset();
	PendingTools.Reset();
	PruneImages();

	if (OutcomeAfterTools != nullptr)
	{
		Finish(OutcomeAfterTools);
		return;
	}
	if (bCancelRequested)
	{
		Finish(TEXT("cancelled"));
		return;
	}

	++Iteration;
	const int32 Limit = UCursorUnrealSettings::Get()->MaxToolIterations;
	if (Iteration >= Limit)
	{
		EmitNotice(FString::Printf(TEXT("Stopped after %d model round trips, the limit in the settings. Send another message to let it continue."), Limit));
		Finish(TEXT("max_iterations"));
		return;
	}
	StartRequest();
}

// --- Ending a turn -------------------------------------------------------------------------------

void FAnthropicBackend::Cancel()
{
	check(IsInGameThread());
	if (Phase == EPhase::Idle)
	{
		return;
	}
	bCancelRequested = true;

	if (Phase == EPhase::RunningTools)
	{
		// The tool that is running finishes; the rest are skipped and the turn ends from RunNextTool.
		return;
	}
	if (ActiveCall.IsValid())
	{
		ActiveCall->Cancel();
		ActiveCall.Reset();
	}
	// The half-received reply was never added to the conversation, so there is nothing to undo.
	Finish(TEXT("cancelled"));
}

void FAnthropicBackend::ResetConversation()
{
	check(IsInGameThread());
	if (Phase != EPhase::Idle)
	{
		if (ActiveCall.IsValid())
		{
			ActiveCall->Cancel();
			ActiveCall.Reset();
		}
		Finish(TEXT("cancelled"));
	}
	History.Reset();
	SystemPrompt.Reset();
	ToolsJson.Reset();
	ToolsFingerprint.Reset();
	PendingTools.Reset();
	ToolResults.Reset();
	bConversationStarted = false;
	SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
}

void FAnthropicBackend::Emit(const FAgentEvent& Event)
{
	// Hold a reference for the length of the call: the listener may stop the turn from inside it.
	const TSharedPtr<FAgentEventSink> Keep = Sink;
	if (Keep.IsValid() && *Keep)
	{
		(*Keep)(Event);
	}
}

void FAnthropicBackend::EmitLast(const FAgentEvent& Event)
{
	Phase = EPhase::Idle;
	++Generation;
	// Let go of the sink before calling it: the listener may start the next turn from inside this call.
	const TSharedPtr<FAgentEventSink> Last = MoveTemp(Sink);
	Sink.Reset();
	if (Last.IsValid() && *Last)
	{
		(*Last)(Event);
	}
}

void FAnthropicBackend::EmitNotice(const FString& Text)
{
	Emit(FAgentEvent::Make(EAgentEventType::Notice, CursorRedactor::Redact(Text)));
}

void FAnthropicBackend::EmitUsage()
{
	FAgentEvent Event = FAgentEvent::Make(EAgentEventType::Usage);
	Event.Usage = TurnUsage;
	Emit(Event);
}

void FAnthropicBackend::Finish(const TCHAR* Outcome)
{
	FAgentEvent Event = FAgentEvent::Make(EAgentEventType::Done);
	Event.Outcome = Outcome;
	Event.Usage = TurnUsage;
	EmitLast(Event);
}

void FAnthropicBackend::Fail(const FString& Message)
{
	FAgentEvent Event = FAgentEvent::Make(EAgentEventType::Error, CursorRedactor::Redact(Message));
	Event.Usage = TurnUsage;
	EmitLast(Event);
}

// --- Models --------------------------------------------------------------------------------------

bool FAnthropicBackend::ParseModelList(const FString& Json, TArray<FAgentModelInfo>& OutModels, FString& OutNextCursor)
{
	OutNextCursor.Reset();
	const TSharedPtr<FJsonObject> Root = CursorJson::Parse(Json);
	const TArray<TSharedPtr<FJsonValue>>* Data = AgentJson::Arr(Root, TEXT("data"));
	if (!Data)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Data)
	{
		const TSharedPtr<FJsonObject>* EntryPtr = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(EntryPtr))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Entry = *EntryPtr;

		FAgentModelInfo Info;
		Info.Id = AgentJson::Str(Entry, TEXT("id"));
		const FString Lifecycle = AgentJson::Str(Entry, TEXT("lifecycle"));
		if (Info.Id.IsEmpty() || Lifecycle == TEXT("retired"))
		{
			continue;
		}
		Info.DisplayName = AgentJson::Str(Entry, TEXT("display_name"));
		if (Info.DisplayName.IsEmpty())
		{
			Info.DisplayName = Info.Id;
		}
		Info.Line = AgentJson::Str(Entry, TEXT("line"));
		Info.bDeprecated = Lifecycle == TEXT("deprecated");
		Info.MaxOutputTokens = static_cast<int32>(AgentJson::Num(Entry, TEXT("max_tokens")));
		Info.MaxInputTokens = static_cast<int32>(AgentJson::Num(Entry, TEXT("max_input_tokens")));

		const TSharedPtr<FJsonObject> Capabilities = AgentJson::Obj(Entry, TEXT("capabilities"));
		const TSharedPtr<FJsonObject> ThinkingTypes = AgentJson::Obj(AgentJson::Obj(Capabilities, TEXT("thinking")), TEXT("types"));
		Info.bAdaptiveThinking = AgentJson::Bool(AgentJson::Obj(ThinkingTypes, TEXT("adaptive")), TEXT("supported"));

		const TSharedPtr<FJsonObject> Effort = AgentJson::Obj(Capabilities, TEXT("effort"));
		if (AgentJson::Bool(Effort, TEXT("supported")))
		{
			for (const TCHAR* Level : EffortNames)
			{
				if (AgentJson::Bool(AgentJson::Obj(Effort, Level), TEXT("supported")))
				{
					Info.EffortLevels.Add(Level);
				}
			}
		}
		OutModels.Add(MoveTemp(Info));
	}

	if (AgentJson::Bool(Root, TEXT("has_more")))
	{
		OutNextCursor = AgentJson::Str(Root, TEXT("last_id"));
	}
	return true;
}

void FAnthropicBackend::RefreshModels(FAgentModelsCallback Callback)
{
	check(IsInGameThread());
	if (Options.FixedModels.Num() > 0)
	{
		Callback(true, Models, FString());
		return;
	}
	const FString UrlProblem = UCursorUnrealSettings::ValidateBaseUrl(UCursorUnrealSettings::Get()->AnthropicBaseUrl);
	if (!UrlProblem.IsEmpty())
	{
		Callback(false, Models, UrlProblem);
		return;
	}
	if (Options.KeyProvider().IsEmpty())
	{
		Callback(false, Models, TEXT("An Anthropic API key is needed to fetch the model list."));
		return;
	}
	if (ModelsCall.IsValid())
	{
		ModelsCall->Cancel();
		ModelsCall.Reset();
	}
	FetchModelPage(FString(), MakeShared<TArray<FAgentModelInfo>>(), MoveTemp(Callback), 0);
}

void FAnthropicBackend::FetchModelPage(const FString& AfterId, TSharedRef<TArray<FAgentModelInfo>> Accumulated, FAgentModelsCallback Callback, int32 Page)
{
	FAgentHttpRequest Request;
	Request.Verb = TEXT("GET");
	Request.Url = ApiUrl(TEXT("/v1/models?limit=1000"));
	if (!AfterId.IsEmpty())
	{
		Request.Url += TEXT("&after_id=") + AfterId;
	}
	Request.Headers = AuthHeaders();
	Request.ActivityTimeoutSeconds = 30.0f;

	const TWeakPtr<FAnthropicBackend> Weak = AsShared();
	ModelsCall = Options.Transport->Send(Request, nullptr,
		[Weak, Accumulated, Callback = MoveTemp(Callback), Page](const FAgentHttpResponse& Response) mutable
		{
			const TSharedPtr<FAnthropicBackend> Self = Weak.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->ModelsCall.Reset();

			if (Response.Status == 0)
			{
				Callback(false, Self->Models, FString::Printf(TEXT("Could not reach the Anthropic API (%s)."),
					Response.FailureReason.IsEmpty() ? TEXT("no response") : *Response.FailureReason));
				return;
			}
			if (Response.Status != 200)
			{
				const FString ApiMessage = AgentJson::Str(AgentJson::Obj(CursorJson::Parse(Response.Body), TEXT("error")), TEXT("message"));
				Callback(false, Self->Models, CursorRedactor::Redact(DescribeApiError(Response.Status, ApiMessage, Response.Headers.FindRef(TEXT("request-id")))));
				return;
			}

			FString NextCursor;
			if (!ParseModelList(Response.Body, Accumulated.Get(), NextCursor))
			{
				Callback(false, Self->Models, TEXT("The model list from the endpoint could not be read."));
				return;
			}
			if (!NextCursor.IsEmpty() && Page + 1 < MaxModelPages)
			{
				Self->FetchModelPage(NextCursor, Accumulated, MoveTemp(Callback), Page + 1);
				return;
			}

			Self->Models = Accumulated.Get();
			if (Self->Options.bPersistModels)
			{
				Self->SaveModelCache();
			}
			Callback(true, Self->Models, FString());
		});
}

FString FAnthropicBackend::ModelCacheFile()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CursorUnreal") / TEXT("Cache") / TEXT("anthropic-models.json"));
}

void FAnthropicBackend::LoadModelCache()
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *ModelCacheFile()))
	{
		return;
	}
	// Kept in a variable: Entries points into it.
	const TSharedPtr<FJsonObject> Root = CursorJson::Parse(Text);
	const TArray<TSharedPtr<FJsonValue>>* Entries = AgentJson::Arr(Root, TEXT("models"));
	if (!Entries)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Entries)
	{
		const TSharedPtr<FJsonObject>* EntryPtr = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(EntryPtr))
		{
			continue;
		}
		const TSharedPtr<FJsonObject> Entry = *EntryPtr;
		FAgentModelInfo Info;
		Info.Id = AgentJson::Str(Entry, TEXT("id"));
		if (Info.Id.IsEmpty())
		{
			continue;
		}
		Info.DisplayName = AgentJson::Str(Entry, TEXT("display_name"));
		Info.Line = AgentJson::Str(Entry, TEXT("line"));
		Info.bDeprecated = AgentJson::Bool(Entry, TEXT("deprecated"));
		Info.MaxOutputTokens = static_cast<int32>(AgentJson::Num(Entry, TEXT("max_output_tokens")));
		Info.MaxInputTokens = static_cast<int32>(AgentJson::Num(Entry, TEXT("max_input_tokens")));
		Info.bAdaptiveThinking = AgentJson::Bool(Entry, TEXT("adaptive_thinking"));
		if (const TArray<TSharedPtr<FJsonValue>>* Levels = AgentJson::Arr(Entry, TEXT("effort_levels")))
		{
			for (const TSharedPtr<FJsonValue>& Level : *Levels)
			{
				Info.EffortLevels.Add(Level->AsString());
			}
		}
		Models.Add(MoveTemp(Info));
	}
}

void FAnthropicBackend::SaveModelCache() const
{
	// Model names and capabilities only. Nothing here identifies the account.
	TArray<TSharedPtr<FJsonValue>> Entries;
	for (const FAgentModelInfo& Info : Models)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("id"), Info.Id);
		Entry->SetStringField(TEXT("display_name"), Info.DisplayName);
		Entry->SetStringField(TEXT("line"), Info.Line);
		Entry->SetBoolField(TEXT("deprecated"), Info.bDeprecated);
		Entry->SetNumberField(TEXT("max_output_tokens"), Info.MaxOutputTokens);
		Entry->SetNumberField(TEXT("max_input_tokens"), Info.MaxInputTokens);
		Entry->SetBoolField(TEXT("adaptive_thinking"), Info.bAdaptiveThinking);
		Entry->SetArrayField(TEXT("effort_levels"), CursorJson::StringArray(Info.EffortLevels));
		Entries.Add(MakeShared<FJsonValueObject>(Entry));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("fetched_at"), FDateTime::UtcNow().ToIso8601());
	Root->SetArrayField(TEXT("models"), Entries);

	const FString Path = ModelCacheFile();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	FFileHelper::SaveStringToFile(CursorJson::ToString(Root, true), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
