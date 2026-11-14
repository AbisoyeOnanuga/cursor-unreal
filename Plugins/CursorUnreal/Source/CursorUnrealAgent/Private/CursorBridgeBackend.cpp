#include "CursorBridgeBackend.h"

#include "AgentJsonUtil.h"
#include "AgentSystemPrompt.h"
#include "CursorBridgeInstall.h"
#include "CursorJson.h"
#include "CursorSecrets.h"
#include "CursorTool.h"
#include "CursorToolRegistry.h"
#include "CursorUnrealSettings.h"
#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogCursorBridge, Log, All);

namespace
{
	FString ModelCacheFile()
	{
		return FPaths::ProjectSavedDir() / TEXT("CursorUnreal") / TEXT("cursor-models.json");
	}

	void LoadModelCache(TArray<FAgentModelInfo>& Out)
	{
		Out.Reset();
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *ModelCacheFile()))
		{
			return;
		}
		const TSharedPtr<FJsonObject> Root = CursorJson::Parse(Text);
		const TArray<TSharedPtr<FJsonValue>>* Data = AgentJson::Arr(Root, TEXT("models"));
		if (!Data)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *Data)
		{
			const TSharedPtr<FJsonObject> Obj = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				continue;
			}
			FAgentModelInfo Model;
			Model.Id = AgentJson::Str(Obj, TEXT("id"));
			Model.DisplayName = AgentJson::Str(Obj, TEXT("displayName"));
			if (Model.DisplayName.IsEmpty())
			{
				Model.DisplayName = Model.Id;
			}
			if (!Model.Id.IsEmpty())
			{
				Out.Add(MoveTemp(Model));
			}
		}
	}

	void SaveModelCache(const TArray<FAgentModelInfo>& Models)
	{
		TArray<TSharedPtr<FJsonValue>> Data;
		for (const FAgentModelInfo& Model : Models)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			Obj->SetStringField(TEXT("id"), Model.Id);
			Obj->SetStringField(TEXT("displayName"), Model.DisplayName);
			Data.Add(MakeShared<FJsonValueObject>(Obj));
		}
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("models"), Data);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(ModelCacheFile()), true);
		FFileHelper::SaveStringToFile(CursorJson::ToString(Root, true), *ModelCacheFile(),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

FCursorBridgeBackend::FCursorBridgeBackend(FCursorBridgeBackendOptions InOptions)
	: Options(MoveTemp(InOptions))
	, SessionId(FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower))
{
	if (!Options.KeyProvider)
	{
		Options.KeyProvider = []() { return CursorSecrets::ResolveCursorKey(); };
	}
	LoadModelCache(Models);
	StatusLine = CursorBridge::ProbeInstall().StatusLine();
}

FCursorBridgeBackend::~FCursorBridgeBackend()
{
	if (Process)
	{
		Process->OnLine = nullptr;
		Process->OnExit = nullptr;
		Process->Stop();
		Process.Reset();
	}
}

FString FCursorBridgeBackend::GetModelId() const
{
	return UCursorUnrealSettings::Get()->CursorModel.TrimStartAndEnd();
}

void FCursorBridgeBackend::SetModelId(const FString& ModelId)
{
	UCursorUnrealSettings* Settings = UCursorUnrealSettings::GetMutable();
	Settings->CursorModel = ModelId.TrimStartAndEnd();
	Settings->SaveConfig();
}

bool FCursorBridgeBackend::CheckReady(FText& OutWhyNot) const
{
	if (Options.KeyProvider().IsEmpty())
	{
		OutWhyNot = FText::FromString(TEXT("No Cursor API key. Add one in the Setup tab, or set CURSOR_API_KEY before starting the editor."));
		return false;
	}
	if (GetModelId().IsEmpty())
	{
		OutWhyNot = FText::FromString(TEXT("No model chosen. Pick one from the model list, or set Cursor Model to \"auto\"."));
		return false;
	}
	if (!Options.bSkipInstallProbe)
	{
		const FCursorBridgeInstall Install = CursorBridge::ProbeInstall();
		if (!Install.bOk)
		{
			OutWhyNot = FText::FromString(Install.Problem);
			return false;
		}
	}
	else if (Options.SdkModuleOverride.IsEmpty())
	{
		OutWhyNot = FText::FromString(TEXT("A stand-in SDK module is required when the install probe is skipped."));
		return false;
	}
	return true;
}

bool FCursorBridgeBackend::EnsureProcess(FString& OutError)
{
	if (Process && Process->IsRunning())
	{
		return true;
	}

	FCursorBridgeInstall Install;
	if (Options.bSkipInstallProbe)
	{
		Install.bOk = true;
		Install.NodePath = CursorBridge::FindNodeExecutable();
		Install.BridgeScript = CursorBridge::GetBridgeScriptPath();
		if (Install.NodePath.IsEmpty())
		{
			OutError = TEXT("Node.js was not found.");
			return false;
		}
		if (Install.BridgeScript.IsEmpty() || !FPaths::FileExists(Install.BridgeScript))
		{
			OutError = TEXT("The bridge script is missing.");
			return false;
		}
	}
	else
	{
		Install = CursorBridge::ProbeInstall();
		StatusLine = Install.StatusLine();
		if (!Install.bOk)
		{
			OutError = Install.Problem;
			return false;
		}
	}

	TUniquePtr<FCursorBridgeProcess> Next = MakeUnique<FCursorBridgeProcess>();
	// SharedThis is safe: this object is always owned by a TSharedPtr (CreateBackend / tests).
	const TWeakPtr<FCursorBridgeBackend> Weak = SharedThis(this);
	Next->OnLine = [Weak](const FString& Line)
	{
		if (const TSharedPtr<FCursorBridgeBackend> Self = Weak.Pin())
		{
			Self->HandleLine(Line);
		}
	};
	Next->OnExit = [Weak](int32 Code)
	{
		if (const TSharedPtr<FCursorBridgeBackend> Self = Weak.Pin())
		{
			// Never destroy the process from inside its own callback; mark it dead and drop it next tick.
			Self->HandleProcessExit(Code);
		}
	};

	TArray<FString> ExtraArgs;
	if (!Options.SdkModuleOverride.IsEmpty())
	{
		ExtraArgs.Add(FString::Printf(TEXT("--sdk-module=%s"), *Options.SdkModuleOverride));
	}

	bHelloSeen = false;
	bReady = false;
	bInitInFlight = false;
	InitFingerprint.Reset();
	if (!Next->Start(Install.NodePath, Install.BridgeScript, ExtraArgs, OutError))
	{
		return false;
	}
	const uint32 Pid = Next->GetProcessId();
	Process = MoveTemp(Next);
	StatusLine = FString::Printf(TEXT("Bridge starting (pid %u)."), Pid);
	return true;
}

bool FCursorBridgeBackend::SendInit(FString& OutError)
{
	if (!Process || !Process->IsRunning())
	{
		OutError = TEXT("The Cursor bridge is not running.");
		return false;
	}

	const FString Key = Options.KeyProvider();
	if (Key.IsEmpty())
	{
		OutError = TEXT("No Cursor API key.");
		return false;
	}

	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("init"));
	Message->SetStringField(TEXT("apiKey"), Key);
	Message->SetStringField(TEXT("cwd"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	const TArray<TSharedPtr<FJsonValue>> Tools = DescribeTools();
	Message->SetStringField(TEXT("builtinTools"), LexToString(UCursorUnrealSettings::Get()->CursorBuiltinTools));
	Message->SetBoolField(TEXT("prewarm"), true);
	Message->SetArrayField(TEXT("settingSources"), CursorJson::StringArray({ TEXT("project") }));
	Message->SetArrayField(TEXT("tools"), Tools);

	// Fingerprint without the key, so a key rotation still re-inits.
	InitFingerprint = FString::Printf(TEXT("%s|%s|%s"),
		*FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()),
		LexToString(UCursorUnrealSettings::Get()->CursorBuiltinTools),
		*CursorJson::ToString(Tools));

	bReady = false;
	bInitInFlight = true;
	if (!Process->WriteLine(CursorJson::ToString(Message)))
	{
		OutError = TEXT("Could not write to the Cursor bridge.");
		return false;
	}
	return true;
}

TArray<TSharedPtr<FJsonValue>> FCursorBridgeBackend::DescribeTools() const
{
	TArray<TSharedPtr<FJsonValue>> Tools;
	for (const TSharedRef<ICursorTool>& Tool : FCursorToolRegistry::Get().GetTools(FCursorToolRegistry::CallerCursorSdk))
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("name"), Tool->GetName().ToString());
		Entry->SetStringField(TEXT("description"), Tool->GetDescription().ToString());
		Entry->SetObjectField(TEXT("inputSchema"), Tool->GetInputSchema());
		Entry->SetBoolField(TEXT("readOnly"), Tool->GetRisk() == ECursorToolRisk::ReadOnly);
		Entry->SetBoolField(TEXT("destructive"), Tool->GetRisk() == ECursorToolRisk::Dangerous);
		Tools.Add(MakeShared<FJsonValueObject>(Entry));
	}
	return Tools;
}

FString FCursorBridgeBackend::BuildUserText(const FAgentTurnRequest& Request) const
{
	FString Text;
	const FString System = CursorAgent::BuildSystemPrompt();
	if (!System.IsEmpty())
	{
		// Cursor's local agent has its own system prompt; project instructions go in the user message.
		Text += TEXT("<unreal_editor_instructions>\n");
		Text += System;
		Text += TEXT("\n</unreal_editor_instructions>\n\n");
	}
	for (const FAgentContextBlock& Context : Request.Context)
	{
		if (!Context.Text.TrimStartAndEnd().IsEmpty())
		{
			Text += FString::Printf(TEXT("<context name=\"%s\">\n%s\n</context>\n\n"), *Context.Title, *Context.Text);
		}
	}
	Text += Request.Text.TrimStartAndEnd();
	return Text;
}

FString FCursorBridgeBackend::NextTurnId()
{
	return FString::Printf(TEXT("turn-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

FString FCursorBridgeBackend::NextModelsId()
{
	return FString::Printf(TEXT("models-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
}

void FCursorBridgeBackend::SendTurn(const FAgentTurnRequest& Request, FAgentEventSink InSink)
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
	if (Request.Text.TrimStartAndEnd().IsEmpty())
	{
		InSink(FAgentEvent::Make(EAgentEventType::Error, TEXT("The message is empty.")));
		return;
	}

	LastUserText = Request.Text.TrimStartAndEnd();
	LastContext = Request.Context;
	bCanRetry = false;

	FString Error;
	if (!EnsureProcess(Error))
	{
		InSink(FAgentEvent::Make(EAgentEventType::Error, CursorRedactor::Redact(Error)));
		return;
	}

	BeginTurn(MoveTemp(InSink));
	Phase = EPhase::Starting;

	if (!bReady)
	{
		if (!bInitInFlight && !SendInit(Error))
		{
			Fail(Error);
			return;
		}
		// HandleMessage will start the turn once ready arrives.
		return;
	}

	ActiveTurnId = NextTurnId();
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("send"));
	Message->SetStringField(TEXT("id"), ActiveTurnId);
	Message->SetStringField(TEXT("text"), BuildUserText(Request));
	TSharedRef<FJsonObject> Model = MakeShared<FJsonObject>();
	Model->SetStringField(TEXT("id"), GetModelId());
	Message->SetObjectField(TEXT("model"), Model);

	Phase = EPhase::Requesting;
	Emit(FAgentEvent::Make(EAgentEventType::RequestStarted));
	if (!Process->WriteLine(CursorJson::ToString(Message)))
	{
		Fail(TEXT("Could not write to the Cursor bridge."));
	}
}

bool FCursorBridgeBackend::CanRetry() const
{
	return Phase == EPhase::Idle && bCanRetry && !LastUserText.IsEmpty();
}

void FCursorBridgeBackend::Retry(FAgentEventSink InSink)
{
	if (!CanRetry())
	{
		InSink(FAgentEvent::Make(EAgentEventType::Error, TEXT("There is nothing to send again.")));
		return;
	}
	FAgentTurnRequest Request;
	Request.Text = LastUserText;
	Request.Context = LastContext;
	SendTurn(Request, MoveTemp(InSink));
}

void FCursorBridgeBackend::Cancel()
{
	check(IsInGameThread());
	if (Phase == EPhase::Idle)
	{
		return;
	}
	bCancelRequested = true;
	if (Process && Process->IsRunning() && !ActiveTurnId.IsEmpty())
	{
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("cancel"));
		Message->SetStringField(TEXT("id"), ActiveTurnId);
		Process->WriteLine(CursorJson::ToString(Message));
	}
	else
	{
		Finish(TEXT("cancelled"));
	}
}

void FCursorBridgeBackend::ResetConversation()
{
	check(IsInGameThread());
	if (Phase != EPhase::Idle)
	{
		Cancel();
	}
	if (Process && Process->IsRunning())
	{
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("reset"));
		Process->WriteLine(CursorJson::ToString(Message));
	}
	SessionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	LastUserText.Reset();
	LastContext.Reset();
	bCanRetry = false;
	ActiveTurnId.Reset();
	PendingToolCalls.Reset();
}

void FCursorBridgeBackend::RefreshModels(FAgentModelsCallback Callback)
{
	check(IsInGameThread());
	FText WhyNot;
	if (Options.KeyProvider().IsEmpty())
	{
		Callback(false, Models, TEXT("No Cursor API key."));
		return;
	}
	if (!Options.bSkipInstallProbe)
	{
		const FCursorBridgeInstall Install = CursorBridge::ProbeInstall();
		if (!Install.bOk)
		{
			Callback(false, Models, Install.Problem);
			return;
		}
	}

	FString Error;
	if (!EnsureProcess(Error))
	{
		Callback(false, Models, Error);
		return;
	}

	ModelsCallback = MoveTemp(Callback);
	ModelsRequestId = NextModelsId();

	if (!bReady)
	{
		if (!bInitInFlight && !SendInit(Error))
		{
			ModelsCallback(false, Models, Error);
			ModelsCallback = nullptr;
			ModelsRequestId.Reset();
		}
		return;
	}

	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("list_models"));
	Message->SetStringField(TEXT("id"), ModelsRequestId);
	if (!Process->WriteLine(CursorJson::ToString(Message)))
	{
		ModelsCallback(false, Models, TEXT("Could not write to the Cursor bridge."));
		ModelsCallback = nullptr;
		ModelsRequestId.Reset();
	}
}

void FCursorBridgeBackend::HandleLine(const FString& Line)
{
	const TSharedPtr<FJsonObject> Message = CursorJson::Parse(Line);
	if (!Message.IsValid())
	{
		UE_LOG(LogCursorBridge, Warning, TEXT("Bridge sent a non-JSON line."));
		return;
	}
	HandleMessage(Message);
}

void FCursorBridgeBackend::HandleMessage(const TSharedPtr<FJsonObject>& Message)
{
	const FString Type = AgentJson::Str(Message, TEXT("type"));
	if (Type == TEXT("hello"))
	{
		bHelloSeen = true;
		StatusLine = FString::Printf(TEXT("Bridge up (Node %s). Waiting to initialise."),
			*AgentJson::Str(Message, TEXT("node")));
		if ((Phase == EPhase::Starting || ModelsCallback) && !bInitInFlight)
		{
			FString Error;
			if (!SendInit(Error) && Phase == EPhase::Starting)
			{
				Fail(Error);
			}
		}
		return;
	}
	if (Type == TEXT("ready"))
	{
		bReady = true;
		bInitInFlight = false;
		StatusLine = FString::Printf(TEXT("Ready. Node bridge, SDK %s, %d editor tools."),
			*AgentJson::Str(Message, TEXT("sdk")),
			static_cast<int32>(AgentJson::Num(Message, TEXT("tools"))));

		if (ModelsCallback)
		{
			TSharedRef<FJsonObject> List = MakeShared<FJsonObject>();
			List->SetStringField(TEXT("type"), TEXT("list_models"));
			List->SetStringField(TEXT("id"), ModelsRequestId.IsEmpty() ? NextModelsId() : ModelsRequestId);
			ModelsRequestId = AgentJson::Str(List, TEXT("id"));
			Process->WriteLine(CursorJson::ToString(List));
		}

		if (Phase == EPhase::Starting && Sink.IsValid())
		{
			ActiveTurnId = NextTurnId();
			FAgentTurnRequest Request;
			Request.Text = LastUserText;
			Request.Context = LastContext;
			TSharedRef<FJsonObject> Send = MakeShared<FJsonObject>();
			Send->SetStringField(TEXT("type"), TEXT("send"));
			Send->SetStringField(TEXT("id"), ActiveTurnId);
			Send->SetStringField(TEXT("text"), BuildUserText(Request));
			TSharedRef<FJsonObject> Model = MakeShared<FJsonObject>();
			Model->SetStringField(TEXT("id"), GetModelId());
			Send->SetObjectField(TEXT("model"), Model);
			Phase = EPhase::Requesting;
			Emit(FAgentEvent::Make(EAgentEventType::RequestStarted));
			if (!Process->WriteLine(CursorJson::ToString(Send)))
			{
				Fail(TEXT("Could not write to the Cursor bridge."));
			}
		}
		return;
	}
	if (Type == TEXT("init_error") || Type == TEXT("fatal"))
	{
		bInitInFlight = false;
		bReady = false;
		const FString Text = CursorRedactor::Redact(AgentJson::Str(Message, TEXT("message")));
		StatusLine = Text;
		if (ModelsCallback)
		{
			ModelsCallback(false, Models, Text);
			ModelsCallback = nullptr;
			ModelsRequestId.Reset();
		}
		if (Phase != EPhase::Idle)
		{
			Fail(Text);
		}
		return;
	}
	if (Type == TEXT("prewarmed"))
	{
		StatusLine = FString::Printf(TEXT("Workspace prewarmed in %.0f ms."), AgentJson::Num(Message, TEXT("ms")));
		return;
	}
	if (Type == TEXT("log"))
	{
		UE_LOG(LogCursorBridge, Display, TEXT("[%s] %s"),
			*AgentJson::Str(Message, TEXT("level")),
			*CursorRedactor::Redact(AgentJson::Str(Message, TEXT("text"))));
		return;
	}
	if (Type == TEXT("models") || Type == TEXT("models_error"))
	{
		if (!ModelsCallback)
		{
			return;
		}
		const FAgentModelsCallback Callback = MoveTemp(ModelsCallback);
		ModelsCallback = nullptr;
		ModelsRequestId.Reset();
		if (Type == TEXT("models_error"))
		{
			Callback(false, Models, CursorRedactor::Redact(AgentJson::Str(Message, TEXT("message"))));
			return;
		}
		TArray<FAgentModelInfo> Next;
		if (const TArray<TSharedPtr<FJsonValue>>* List = AgentJson::Arr(Message, TEXT("models")))
		{
			for (const TSharedPtr<FJsonValue>& Entry : *List)
			{
				const TSharedPtr<FJsonObject> Obj = Entry.IsValid() ? Entry->AsObject() : nullptr;
				if (!Obj.IsValid())
				{
					continue;
				}
				FAgentModelInfo Model;
				Model.Id = AgentJson::Str(Obj, TEXT("id"));
				Model.DisplayName = AgentJson::Str(Obj, TEXT("displayName"));
				if (Model.DisplayName.IsEmpty())
				{
					Model.DisplayName = Model.Id;
				}
				if (!Model.Id.IsEmpty())
				{
					Next.Add(MoveTemp(Model));
				}
			}
		}
		Models = MoveTemp(Next);
		SaveModelCache(Models);
		Callback(true, Models, FString());
		return;
	}

	const FString Id = AgentJson::Str(Message, TEXT("id"));
	if (!ActiveTurnId.IsEmpty() && !Id.IsEmpty() && Id != ActiveTurnId)
	{
		return;
	}

	if (Type == TEXT("started"))
	{
		// RequestStarted was already emitted when the send left this process.
		return;
	}
	if (Type == TEXT("text_delta"))
	{
		Emit(FAgentEvent::Make(EAgentEventType::TextDelta, AgentJson::Str(Message, TEXT("text"))));
		return;
	}
	if (Type == TEXT("thinking_delta"))
	{
		Emit(FAgentEvent::Make(EAgentEventType::ThinkingDelta, AgentJson::Str(Message, TEXT("text"))));
		return;
	}
	if (Type == TEXT("notice"))
	{
		EmitNotice(AgentJson::Str(Message, TEXT("text")));
		return;
	}
	if (Type == TEXT("usage"))
	{
		if (const TSharedPtr<FJsonObject> Usage = AgentJson::Obj(Message, TEXT("usage")))
		{
			TurnUsage.InputTokens += static_cast<int64>(AgentJson::Num(Usage, TEXT("inputTokens")));
			TurnUsage.OutputTokens += static_cast<int64>(AgentJson::Num(Usage, TEXT("outputTokens")));
			TurnUsage.CacheReadTokens += static_cast<int64>(AgentJson::Num(Usage, TEXT("cacheReadTokens")));
			TurnUsage.CacheWriteTokens += static_cast<int64>(AgentJson::Num(Usage, TEXT("cacheWriteTokens")));
			FAgentEvent Event = FAgentEvent::Make(EAgentEventType::Usage);
			Event.Usage = TurnUsage;
			Emit(Event);
		}
		return;
	}
	if (Type == TEXT("builtin_tool"))
	{
		const FString CallId = AgentJson::Str(Message, TEXT("callId"));
		const FString Name = AgentJson::Str(Message, TEXT("name"));
		const FString Status = AgentJson::Str(Message, TEXT("status"));
		if (Status == TEXT("running"))
		{
			FAgentEvent Started = FAgentEvent::Make(EAgentEventType::ToolCallStarted);
			Started.ToolCallId = CallId;
			Started.ToolName = Name;
			Emit(Started);
			FAgentEvent Running = FAgentEvent::Make(EAgentEventType::ToolCallRunning);
			Running.ToolCallId = CallId;
			Running.ToolName = Name;
			Running.ArgsJson = AgentJson::Str(Message, TEXT("args"));
			Emit(Running);
		}
		else
		{
			FAgentEvent Finished = FAgentEvent::Make(EAgentEventType::ToolCallFinished, AgentJson::Str(Message, TEXT("result")));
			Finished.ToolCallId = CallId;
			Finished.ToolName = Name;
			Finished.bIsError = Status == TEXT("error");
			Emit(Finished);
		}
		return;
	}
	if (Type == TEXT("tool_call"))
	{
		const FString CallId = AgentJson::Str(Message, TEXT("callId"));
		const FString Name = AgentJson::Str(Message, TEXT("name"));
		const TSharedPtr<FJsonObject> Args = AgentJson::Obj(Message, TEXT("args"));
		const double SentAtMs = AgentJson::Num(Message, TEXT("sentAtMs"));

		FAgentEvent Started = FAgentEvent::Make(EAgentEventType::ToolCallStarted);
		Started.ToolCallId = CallId;
		Started.ToolName = Name;
		Emit(Started);

		FAgentEvent Running = FAgentEvent::Make(EAgentEventType::ToolCallRunning);
		Running.ToolCallId = CallId;
		Running.ToolName = Name;
		Running.ArgsJson = Args.IsValid() ? CursorJson::ToString(Args.ToSharedRef()) : TEXT("{}");
		Emit(Running);

		Phase = EPhase::RunningTools;
		PendingToolCalls.Add(CallId);
		RunEditorTool(CallId, Name, Args, SentAtMs);
		return;
	}
	if (Type == TEXT("done"))
	{
		const FString Outcome = AgentJson::Str(Message, TEXT("outcome"));
		Finish(Outcome.IsEmpty() ? TEXT("done") : *Outcome);
		return;
	}
	if (Type == TEXT("error"))
	{
		bCanRetry = true;
		Fail(AgentJson::Str(Message, TEXT("message")));
		return;
	}
}

void FCursorBridgeBackend::RunEditorTool(const FString& CallId, const FString& Name, const TSharedPtr<FJsonObject>& Args, double SentAtMs)
{
	FCursorToolContext Context;
	Context.Caller = FCursorToolRegistry::CallerCursorSdk;
	Context.SessionId = SessionId;
	Context.CallId = CallId;
	Context.ClientSentUnixMs = SentAtMs;

	const TSharedRef<FJsonObject> Input = Args.IsValid() ? Args.ToSharedRef() : MakeShared<FJsonObject>();
	const TWeakPtr<FCursorBridgeBackend> Weak = AsShared();
	const uint32 ExpectedGeneration = Generation;
	FCursorToolRegistry::Get().Invoke(FName(*Name), Input, Context,
		[Weak, ExpectedGeneration, CallId, Name](const FCursorToolResult& Result)
		{
			const TSharedPtr<FCursorBridgeBackend> Self = Weak.Pin();
			if (!Self.IsValid() || Self->Generation != ExpectedGeneration)
			{
				return;
			}
			Self->PendingToolCalls.Remove(CallId);

			FAgentEvent Finished = FAgentEvent::Make(EAgentEventType::ToolCallFinished, Result.Text);
			Finished.ToolCallId = CallId;
			Finished.ToolName = Name;
			Finished.bIsError = Result.bIsError;
			Self->Emit(Finished);

			if (Self->Process && Self->Process->IsRunning())
			{
				TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
				Message->SetStringField(TEXT("type"), TEXT("tool_result"));
				Message->SetStringField(TEXT("callId"), CallId);
				Message->SetStringField(TEXT("text"), Result.Text.IsEmpty() ? TEXT("(no output)") : Result.Text);
				Message->SetBoolField(TEXT("isError"), Result.bIsError);
				Self->Process->WriteLine(CursorJson::ToString(Message));
			}

			if (Self->PendingToolCalls.Num() == 0 && Self->Phase == EPhase::RunningTools)
			{
				Self->Phase = EPhase::Requesting;
			}
		});
}

void FCursorBridgeBackend::HandleProcessExit(int32 ExitCode)
{
	bReady = false;
	bInitInFlight = false;
	bHelloSeen = false;
	StatusLine = FString::Printf(TEXT("Bridge exited (code %d)."), ExitCode);
	if (ModelsCallback)
	{
		ModelsCallback(false, Models, StatusLine);
		ModelsCallback = nullptr;
	}
	if (Phase != EPhase::Idle)
	{
		Fail(FString::Printf(TEXT("The Cursor bridge stopped unexpectedly (exit code %d)."), ExitCode));
	}
	// Drop the process on the next tick so we are not destroying it from inside its ticker callback.
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Weak = TWeakPtr<FCursorBridgeBackend>(SharedThis(this))](float)
		{
			if (const TSharedPtr<FCursorBridgeBackend> Self = Weak.Pin())
			{
				if (Self->Process && !Self->Process->IsRunning())
				{
					Self->Process.Reset();
				}
			}
			return false;
		}), 0.0f);
}

void FCursorBridgeBackend::BeginTurn(FAgentEventSink InSink)
{
	Sink = MakeShared<FAgentEventSink>(MoveTemp(InSink));
	bCancelRequested = false;
	TurnUsage = FAgentUsage();
	PendingToolCalls.Reset();
	ActiveTurnId.Reset();
}

void FCursorBridgeBackend::Emit(const FAgentEvent& Event)
{
	const TSharedPtr<FAgentEventSink> Keep = Sink;
	if (Keep.IsValid() && *Keep)
	{
		(*Keep)(Event);
	}
}

void FCursorBridgeBackend::EmitLast(const FAgentEvent& Event)
{
	Phase = EPhase::Idle;
	++Generation;
	ActiveTurnId.Reset();
	PendingToolCalls.Reset();
	const TSharedPtr<FAgentEventSink> Last = MoveTemp(Sink);
	Sink.Reset();
	if (Last.IsValid() && *Last)
	{
		(*Last)(Event);
	}
}

void FCursorBridgeBackend::EmitNotice(const FString& Text)
{
	Emit(FAgentEvent::Make(EAgentEventType::Notice, CursorRedactor::Redact(Text)));
}

void FCursorBridgeBackend::Finish(const TCHAR* Outcome)
{
	if (FCString::Strcmp(Outcome, TEXT("cancelled")) == 0)
	{
		bCanRetry = true;
	}
	FAgentEvent Event = FAgentEvent::Make(EAgentEventType::Done);
	Event.Outcome = Outcome;
	Event.Usage = TurnUsage;
	EmitLast(Event);
}

void FCursorBridgeBackend::Fail(const FString& Message)
{
	bCanRetry = true;
	FAgentEvent Event = FAgentEvent::Make(EAgentEventType::Error, CursorRedactor::Redact(Message));
	Event.Usage = TurnUsage;
	EmitLast(Event);
}
