#include "AgentSession.h"

#include "AnthropicBackend.h"
#include "CursorActivity.h"
#include "CursorBridgeBackend.h"
#include "CursorJson.h"
#include "CursorSecrets.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
	/** A tool result can be 100k characters. The chat and the transcript keep the start of it. */
	constexpr int32 MaxShownResultChars = 20000;

	const TCHAR* KindName(EAgentItemKind Kind)
	{
		switch (Kind)
		{
		case EAgentItemKind::User: return TEXT("user");
		case EAgentItemKind::Assistant: return TEXT("assistant");
		case EAgentItemKind::Thinking: return TEXT("thinking");
		case EAgentItemKind::Tool: return TEXT("tool");
		case EAgentItemKind::Error: return TEXT("error");
		default: return TEXT("notice");
		}
	}
}

FAgentSession::FAgentSession(const TSharedRef<IAgentBackend>& InBackend)
	: Backend(InBackend)
{
}

FAgentSession::~FAgentSession()
{
	if (bBusy)
	{
		// Stop listening first, so the backend's final event does not reach a half-destroyed object.
		++TurnSerial;
		Backend->Cancel();
		FCursorActivity::Get().EndSession();
	}
}

FString FAgentSession::GetTranscriptDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CursorUnreal") / TEXT("Transcripts"));
}

FAgentEventSink FAgentSession::MakeSink()
{
	const TWeakPtr<FAgentSession> Weak = AsShared();
	const uint32 Serial = TurnSerial;
	return [Weak, Serial](const FAgentEvent& Event)
	{
		const TSharedPtr<FAgentSession> Self = Weak.Pin();
		if (Self.IsValid() && Self->TurnSerial == Serial)
		{
			Self->HandleEvent(Event);
		}
	};
}

bool FAgentSession::Send(const FString& Text, const TArray<FAgentContextBlock>& Context, FText& OutWhyNot)
{
	FAgentTurnRequest Request;
	Request.Text = Text;
	Request.Context = Context;
	return Send(Request, OutWhyNot);
}

bool FAgentSession::Send(const FAgentTurnRequest& InRequest, FText& OutWhyNot)
{
	check(IsInGameThread());
	if (bBusy)
	{
		OutWhyNot = FText::FromString(TEXT("Wait for the current reply to finish, or stop it."));
		return false;
	}
	FAgentTurnRequest Request = InRequest;
	Request.Text.TrimStartAndEndInline();
	if (Request.Text.IsEmpty())
	{
		if (Request.Images.Num() == 0)
		{
			OutWhyNot = FText::FromString(TEXT("Type a message first."));
			return false;
		}
		// A picture sent on its own still needs words to go with it.
		Request.Text = Request.Images.Num() == 1 ? TEXT("Look at the attached picture.") : TEXT("Look at the attached pictures.");
	}
	const FString ImageProblem = CursorAgent::CheckMessageImages(Request.Images);
	if (!ImageProblem.IsEmpty())
	{
		OutWhyNot = FText::FromString(ImageProblem);
		return false;
	}
	if (!Backend->CheckReady(OutWhyNot))
	{
		return false;
	}

	const TSharedRef<FAgentItem> Item = AddItem(EAgentItemKind::User, Request.Text);
	if (Request.Images.Num() > 0)
	{
		AttachImages(Item.Get(), Request.Images);
		OnItemChanged.Broadcast(Item);
	}
	BeginTurn();
	Backend->SendTurn(Request, MakeSink());
	return true;
}

void FAgentSession::AttachImages(FAgentItem& Item, const TArray<FCursorImage>& Images) const
{
	const bool bSave = bWriteToDisk && UCursorUnrealSettings::Get()->bSaveTranscripts;
	for (const FCursorImage& Image : Images)
	{
		if (Image.IsValid())
		{
			FAgentItemImage& Added = Item.Images.AddDefaulted_GetRef();
			Added.Image = Image;
			if (bSave)
			{
				Added.File = CursorImages::SaveAttachment(Image);
			}
		}
	}
}

bool FAgentSession::CanRetry() const
{
	return !bBusy && Backend->CanRetry();
}

bool FAgentSession::Retry(FText& OutWhyNot)
{
	check(IsInGameThread());
	if (!CanRetry())
	{
		OutWhyNot = FText::FromString(TEXT("There is nothing to send again."));
		return false;
	}
	if (!Backend->CheckReady(OutWhyNot))
	{
		return false;
	}
	BeginTurn();
	Backend->Retry(MakeSink());
	return true;
}

void FAgentSession::Stop()
{
	if (bBusy)
	{
		Backend->Cancel();
	}
}

void FAgentSession::NewChat()
{
	check(IsInGameThread());
	if (bBusy)
	{
		// Ignore whatever the abandoned turn still sends, and close its books here.
		++TurnSerial;
		Backend->ResetConversation();
		EndTurn(TEXT("cancelled"));
	}
	else
	{
		Backend->ResetConversation();
	}
	Items.Reset();
	RoundTrip = 0;
	ChatUsage = FAgentUsage();
	TranscriptFile.Reset();
	NumItemsWritten = 0;
	OnItemsReset.Broadcast();
}

void FAgentSession::BeginTurn()
{
	bBusy = true;
	++TurnSerial;
	// RoundTrip is not reset: it numbers requests across the whole chat, so rewinding one never touches an earlier turn.
	TurnUsage = FAgentUsage();
	TurnStartSeconds = FPlatformTime::Seconds();

	Turn = FCursorTurnPerf();
	Turn.StartedAt = FDateTime::UtcNow();
	Turn.Provider = Backend->GetProviderId();
	Turn.Model = Backend->GetModelId();
	Turn.SessionId = Backend->GetSessionId();

	// Keeps the editor ticking at full rate while the agent works, even when another window has focus.
	FCursorActivity::Get().BeginSession();
	OnStateChanged.Broadcast();
}

void FAgentSession::EndTurn(const FString& Outcome)
{
	if (!bBusy)
	{
		return;
	}
	bBusy = false;

	// A call the model was still writing, or that never got its turn, did not happen. Say so rather than leave it spinning.
	for (const TSharedRef<FAgentItem>& Item : Items)
	{
		if (Item->Kind == EAgentItemKind::Tool && (Item->ToolState == EAgentToolState::Preparing || Item->ToolState == EAgentToolState::Running))
		{
			Item->ToolState = EAgentToolState::NotRun;
			OnItemChanged.Broadcast(Item);
		}
	}

	Turn.Outcome = Outcome;
	Turn.TotalMs = (FPlatformTime::Seconds() - TurnStartSeconds) * 1000.0;
	Turn.ModelMs = FMath::Max(0.0, Turn.TotalMs - Turn.ToolMs);
	Turn.InputTokens = TurnUsage.InputTokens;
	Turn.OutputTokens = TurnUsage.OutputTokens;
	Turn.CacheReadTokens = TurnUsage.CacheReadTokens;
	Turn.CacheWriteTokens = TurnUsage.CacheWriteTokens;

	ChatUsage.InputTokens += TurnUsage.InputTokens;
	ChatUsage.OutputTokens += TurnUsage.OutputTokens;
	ChatUsage.CacheReadTokens += TurnUsage.CacheReadTokens;
	ChatUsage.CacheWriteTokens += TurnUsage.CacheWriteTokens;

	FCursorActivity::Get().EndSession();
	if (bWriteToDisk)
	{
		FCursorPerfLog::Get().RecordTurn(Turn);
		WriteTranscript(Outcome);
	}
	OnStateChanged.Broadcast();
}

TSharedRef<FAgentItem> FAgentSession::AddItem(EAgentItemKind Kind, const FString& Text)
{
	const TSharedRef<FAgentItem> Item = MakeShared<FAgentItem>();
	Item->Kind = Kind;
	Item->Text = Text;
	Item->Time = FDateTime::UtcNow();
	Item->RoundTrip = RoundTrip;
	Items.Add(Item);
	OnItemAdded.Broadcast(Item);
	return Item;
}

TSharedRef<FAgentItem> FAgentSession::OpenItem(EAgentItemKind Kind)
{
	if (Items.Num() > 0 && Items.Last()->Kind == Kind && Items.Last()->RoundTrip == RoundTrip)
	{
		return Items.Last();
	}
	return AddItem(Kind, FString());
}

TSharedPtr<FAgentItem> FAgentSession::FindTool(const FString& CallId) const
{
	// Newest first: the call being looked for is nearly always the last one added.
	for (int32 Index = Items.Num() - 1; Index >= 0; --Index)
	{
		if (Items[Index]->Kind == EAgentItemKind::Tool && Items[Index]->ToolCallId == CallId)
		{
			return Items[Index];
		}
	}
	return nullptr;
}

void FAgentSession::NoteFirstOutput()
{
	if (Turn.TimeToFirstTokenMs < 0.0)
	{
		Turn.TimeToFirstTokenMs = (FPlatformTime::Seconds() - TurnStartSeconds) * 1000.0;
	}
}

void FAgentSession::HandleEvent(const FAgentEvent& Event)
{
	switch (Event.Type)
	{
	case EAgentEventType::RequestStarted:
		++RoundTrip;
		++Turn.RoundTrips;
		break;

	case EAgentEventType::TextDelta:
	case EAgentEventType::ThinkingDelta:
	{
		NoteFirstOutput();
		const TSharedRef<FAgentItem> Item = OpenItem(Event.Type == EAgentEventType::TextDelta ? EAgentItemKind::Assistant : EAgentItemKind::Thinking);
		Item->Text += Event.Text;
		OnItemChanged.Broadcast(Item);
		break;
	}

	case EAgentEventType::ToolCallStarted:
	{
		NoteFirstOutput();
		const TSharedRef<FAgentItem> Item = AddItem(EAgentItemKind::Tool, FString());
		Item->ToolCallId = Event.ToolCallId;
		Item->ToolName = Event.ToolName;
		OnItemChanged.Broadcast(Item);
		break;
	}

	case EAgentEventType::ToolCallRunning:
	{
		TSharedPtr<FAgentItem> Item = FindTool(Event.ToolCallId);
		if (!Item.IsValid())
		{
			Item = AddItem(EAgentItemKind::Tool, FString());
			Item->ToolCallId = Event.ToolCallId;
			Item->ToolName = Event.ToolName;
		}
		Item->ArgsJson = CursorRedactor::Redact(Event.ArgsJson);
		Item->ToolState = EAgentToolState::Running;
		ToolStartSeconds = FPlatformTime::Seconds();
		OnItemChanged.Broadcast(Item.ToSharedRef());
		break;
	}

	case EAgentEventType::ToolCallFinished:
	{
		TSharedPtr<FAgentItem> Item = FindTool(Event.ToolCallId);
		if (!Item.IsValid())
		{
			Item = AddItem(EAgentItemKind::Tool, FString());
			Item->ToolCallId = Event.ToolCallId;
			Item->ToolName = Event.ToolName;
		}
		const bool bRan = Item->ToolState == EAgentToolState::Running;
		Item->Text = CursorJson::Truncate(CursorRedactor::Redact(Event.Text), MaxShownResultChars);
		Item->bIsError = Event.bIsError;
		Item->ToolState = bRan ? EAgentToolState::Finished : EAgentToolState::NotRun;
		AttachImages(*Item, Event.Images);
		if (bRan)
		{
			Item->ToolMs = (FPlatformTime::Seconds() - ToolStartSeconds) * 1000.0;
			Turn.ToolMs += Item->ToolMs;
			++Turn.ToolCalls;
		}
		OnItemChanged.Broadcast(Item.ToSharedRef());
		break;
	}

	case EAgentEventType::Usage:
		TurnUsage = Event.Usage;
		break;

	case EAgentEventType::Notice:
		AddItem(EAgentItemKind::Notice, Event.Text);
		break;

	case EAgentEventType::Rewind:
	{
		// The request is being sent again; what it showed so far will be produced afresh.
		const int32 Removed = Items.RemoveAll([this](const TSharedRef<FAgentItem>& Item)
		{
			return Item->RoundTrip == RoundTrip
				&& (Item->Kind == EAgentItemKind::Assistant || Item->Kind == EAgentItemKind::Thinking || Item->Kind == EAgentItemKind::Tool);
		});
		if (Removed > 0)
		{
			OnItemsReset.Broadcast();
		}
		break;
	}

	case EAgentEventType::Done:
		TurnUsage = Event.Usage;
		if (Event.Outcome == TEXT("cancelled"))
		{
			AddItem(EAgentItemKind::Notice, TEXT("Stopped."));
		}
		EndTurn(Event.Outcome);
		break;

	case EAgentEventType::Error:
		TurnUsage = Event.Usage;
		AddItem(EAgentItemKind::Error, Event.Text);
		EndTurn(TEXT("error"));
		break;
	}
}

void FAgentSession::WriteTranscript(const FString& Outcome)
{
	if (!UCursorUnrealSettings::Get()->bSaveTranscripts)
	{
		return;
	}
	if (TranscriptFile.IsEmpty())
	{
		TranscriptFile = GetTranscriptDirectory() / FString::Printf(TEXT("%s-%s.jsonl"),
			*FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")), *Backend->GetSessionId().Left(8));
		NumItemsWritten = 0;
	}

	FString Lines;
	for (int32 Index = NumItemsWritten; Index < Items.Num(); ++Index)
	{
		const FAgentItem& Item = Items[Index].Get();
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("time"), Item.Time.ToIso8601());
		Json->SetStringField(TEXT("kind"), KindName(Item.Kind));
		// Whole items are scrubbed here rather than piece by piece as they streamed, so a secret split across two pieces is still caught.
		Json->SetStringField(TEXT("text"), CursorRedactor::Redact(Item.Text));
		if (Item.Kind == EAgentItemKind::Tool)
		{
			Json->SetStringField(TEXT("tool"), Item.ToolName);
			Json->SetStringField(TEXT("args"), CursorRedactor::Redact(Item.ArgsJson));
			Json->SetBoolField(TEXT("is_error"), Item.bIsError);
			Json->SetBoolField(TEXT("ran"), Item.ToolState == EAgentToolState::Finished);
		}
		if (Item.Images.Num() > 0)
		{
			// The pictures themselves are files next door. The transcript records which ones, so it stays small and readable.
			TArray<TSharedPtr<FJsonValue>> Images;
			for (const FAgentItemImage& Attached : Item.Images)
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Attached.Image.Name);
				Entry->SetStringField(TEXT("type"), Attached.Image.MimeType);
				Entry->SetNumberField(TEXT("width"), Attached.Image.Width);
				Entry->SetNumberField(TEXT("height"), Attached.Image.Height);
				Entry->SetNumberField(TEXT("bytes"), static_cast<double>(Attached.Image.NumBytes()));
				Entry->SetStringField(TEXT("sha1"), Attached.Image.Hash());
				if (!Attached.File.IsEmpty())
				{
					Entry->SetStringField(TEXT("file"), FPaths::GetCleanFilename(Attached.File));
				}
				Images.Add(MakeShared<FJsonValueObject>(Entry));
			}
			Json->SetArrayField(TEXT("images"), Images);
		}
		Lines += CursorJson::ToString(Json) + TEXT("\n");
	}
	NumItemsWritten = Items.Num();

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetStringField(TEXT("time"), FDateTime::UtcNow().ToIso8601());
	Summary->SetStringField(TEXT("kind"), TEXT("turn_end"));
	Summary->SetStringField(TEXT("provider"), Turn.Provider);
	Summary->SetStringField(TEXT("model"), Turn.Model);
	Summary->SetStringField(TEXT("outcome"), Outcome);
	Summary->SetNumberField(TEXT("input_tokens"), static_cast<double>(TurnUsage.TotalInput()));
	Summary->SetNumberField(TEXT("output_tokens"), static_cast<double>(TurnUsage.OutputTokens));
	Lines += CursorJson::ToString(Summary) + TEXT("\n");

	IFileManager::Get().MakeDirectory(*GetTranscriptDirectory(), true);
	FFileHelper::SaveStringToFile(Lines, *TranscriptFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}

bool CursorAgent::IsDemoMode()
{
	static const bool bDemo = FParse::Param(FCommandLine::Get(), TEXT("CursorUnrealDemo"));
	return bDemo;
}

TSharedPtr<IAgentBackend> CursorAgent::CreateDemoBackend(TSharedPtr<FAgentFixtureTransport>& OutTransport, FText& OutError)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CursorUnreal"));
	if (!Plugin.IsValid())
	{
		OutError = FText::FromString(TEXT("The CursorUnreal plugin folder could not be found."));
		return nullptr;
	}
	const FString Fixtures = Plugin->GetBaseDir() / TEXT("Resources") / TEXT("Fixtures");

	const TSharedRef<FAgentFixtureTransport> Transport = MakeShared<FAgentFixtureTransport>();
	// Small pieces, so the panel shows text arriving the way it does from the real API.
	constexpr int32 ChunkBytes = 48;
	for (const TCHAR* File : { TEXT("turn-1.sse"), TEXT("turn-2.sse"), TEXT("turn-3.sse") })
	{
		if (!Transport->EnqueueFile(Fixtures / TEXT("demo") / File, ChunkBytes))
		{
			OutError = FText::FromString(FString::Printf(TEXT("The demo script '%s' is missing from the plugin's Resources/Fixtures/demo folder."), File));
			return nullptr;
		}
	}
	Transport->LoopQueuedResponses();

	FAnthropicBackendOptions Options;
	Options.Transport = Transport;
	Options.KeyProvider = []() { return FString(TEXT("demo-mode-no-key")); };
	Options.bPersistModels = false;
	Options.Banner = TEXT("Offline demo: replies are scripted and nothing is sent to any service. Tool calls are real and run in this editor.");

	FString ModelsJson;
	FString Cursor;
	if (!FFileHelper::LoadFileToString(ModelsJson, *(Fixtures / TEXT("anthropic") / TEXT("models.json")))
		|| !FAnthropicBackend::ParseModelList(ModelsJson, Options.FixedModels, Cursor) || Options.FixedModels.Num() == 0)
	{
		OutError = FText::FromString(TEXT("The demo model list is missing from the plugin's Resources/Fixtures/anthropic folder."));
		return nullptr;
	}
	Options.ModelOverride = Options.FixedModels[0].Id;

	OutTransport = Transport;
	return MakeShared<FAnthropicBackend>(MoveTemp(Options));
}

TSharedPtr<IAgentBackend> CursorAgent::CreateBackend(ECursorProvider Provider, FText& OutError)
{
	if (IsDemoMode())
	{
		TSharedPtr<FAgentFixtureTransport> Transport;
		const TSharedPtr<IAgentBackend> Backend = CreateDemoBackend(Transport, OutError);
		if (Transport.IsValid())
		{
			Transport->StartAutoPump(0.02f);
		}
		return Backend;
	}
	switch (Provider)
	{
	case ECursorProvider::Anthropic:
		return MakeShared<FAnthropicBackend>();
	case ECursorProvider::Cursor:
		return MakeShared<FCursorBridgeBackend>();
	default:
		OutError = FText::FromString(TEXT("Unknown provider."));
		return nullptr;
	}
}
