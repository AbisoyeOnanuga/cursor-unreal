#include "CursorToolRegistry.h"

#include "CursorActivity.h"
#include "CursorApprovalGate.h"
#include "CursorJson.h"
#include "CursorPerfLog.h"
#include "CursorSecrets.h"
#include "CursorUnrealLog.h"
#include "CursorUnrealSettings.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "HAL/PlatformTime.h"
#include "Misc/Guid.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

const FString FCursorToolRegistry::CallerNative(TEXT("native"));
const FString FCursorToolRegistry::CallerCursorSdk(TEXT("cursor-sdk"));
const FString FCursorToolRegistry::CallerMcp(TEXT("mcp"));

FCursorToolRegistry& FCursorToolRegistry::Get()
{
	static FCursorToolRegistry Instance;
	return Instance;
}

void FCursorToolRegistry::Register(const TSharedRef<ICursorTool>& Tool)
{
	const FName Name = Tool->GetName();
	if (Tools.Contains(Name))
	{
		UE_LOG(LogCursorUnreal, Warning, TEXT("Tool '%s' was registered twice; the later registration wins."), *Name.ToString());
	}
	Tools.Add(Name, Tool);
	ToolsChanged.Broadcast();
}

void FCursorToolRegistry::Unregister(FName Name)
{
	if (Tools.Remove(Name) > 0)
	{
		ToolsChanged.Broadcast();
	}
}

void FCursorToolRegistry::UnregisterPack(FName Pack)
{
	bool bRemoved = false;
	for (auto It = Tools.CreateIterator(); It; ++It)
	{
		if (It.Value()->GetPack() == Pack)
		{
			It.RemoveCurrent();
			bRemoved = true;
		}
	}
	if (bRemoved)
	{
		ToolsChanged.Broadcast();
	}
}

TSharedPtr<ICursorTool> FCursorToolRegistry::Find(FName Name) const
{
	const TSharedRef<ICursorTool>* Found = Tools.Find(Name);
	return Found ? TSharedPtr<ICursorTool>(*Found) : nullptr;
}

TArray<TSharedRef<ICursorTool>> FCursorToolRegistry::GetAllTools() const
{
	TArray<TSharedRef<ICursorTool>> Out;
	Tools.GenerateValueArray(Out);
	Out.Sort([](const TSharedRef<ICursorTool>& A, const TSharedRef<ICursorTool>& B)
	{
		return A->GetName().ToString() < B->GetName().ToString();
	});
	return Out;
}

TArray<TSharedRef<ICursorTool>> FCursorToolRegistry::GetTools(const FString& Caller) const
{
	const UCursorUnrealSettings* Settings = UCursorUnrealSettings::Get();
	const bool bNative = Caller == CallerNative;

	TArray<TSharedRef<ICursorTool>> Out;
	for (const TSharedRef<ICursorTool>& Tool : GetAllTools())
	{
		if (!Settings->IsPackEnabled(Tool->GetPack()))
		{
			continue;
		}
		if (!bNative && Tool->GetAudience() == ECursorToolAudience::NativeAgentOnly)
		{
			continue;
		}
		Out.Add(Tool);
	}
	return Out;
}

TArray<FName> FCursorToolRegistry::GetPacks() const
{
	TArray<FName> Packs;
	for (const auto& Pair : Tools)
	{
		Packs.AddUnique(Pair.Value->GetPack());
	}
	Packs.Sort([](const FName& A, const FName& B) { return A.ToString() < B.ToString(); });
	return Packs;
}

namespace
{
	double NowUnixMs()
	{
		return (FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds();
	}

	/** State for one call, shared by the approval callback and the tool's completion callback. */
	struct FInvocation : public TSharedFromThis<FInvocation>
	{
		TSharedPtr<ICursorTool> Tool;
		TSharedPtr<FJsonObject> Args;
		FCursorToolContext Ctx;
		FCursorToolCompletion OnComplete;
		TSharedPtr<FCursorToolCallRecord> Record;

		double InvokedAt = 0.0;
		double ExecStartedAt = 0.0;
		bool bFinished = false;

		bool bOpenedTransaction = false;
		bool bTransactionWasActive = false;
		int32 UndoQueueBefore = 0;

		void Finish(FCursorToolResult Result, ECursorCallStatus Status)
		{
			if (bFinished)
			{
				UE_LOG(LogCursorUnreal, Warning, TEXT("Tool '%s' reported completion more than once; ignoring the extra call."), *Record->ToolName);
				return;
			}
			bFinished = true;

			if (bOpenedTransaction && GEditor)
			{
				GEditor->EndTransaction();
				if (!bTransactionWasActive && GEditor->Trans)
				{
					Record->bRecordedUndo = GEditor->Trans->GetQueueLength() > UndoQueueBefore;
				}
			}

			const double Now = FPlatformTime::Seconds();
			if (ExecStartedAt > 0.0)
			{
				Record->ExecMs = (Now - ExecStartedAt) * 1000.0;
				Record->QueueMs = FMath::Max(0.0, (ExecStartedAt - InvokedAt) * 1000.0 - Record->ApprovalMs);
			}
			else
			{
				Record->QueueMs = FMath::Max(0.0, (Now - InvokedAt) * 1000.0 - Record->ApprovalMs);
			}

			if (Result.Text.Len() > FCursorToolRegistry::MaxResultChars)
			{
				Result.Text = CursorJson::Truncate(Result.Text, FCursorToolRegistry::MaxResultChars)
					+ TEXT("\nNarrow the request (filters, limit, offset) to see the rest.");
				Result.Structured.Reset();
			}

			Record->Status = Status;
			Record->ResultChars = Result.Text.Len();
			Record->ResultPreview = CursorJson::Truncate(CursorRedactor::Redact(Result.Text), 4000);

			FCursorActivity::Get().NoteActivity();
			FCursorPerfLog::Get().RecordTool(*Record);
			FCursorActivityLog::Get().NotifyUpdated(Record.ToSharedRef());

			// Move out first: the callback may release the last reference to this invocation.
			FCursorToolCompletion Callback = MoveTemp(OnComplete);
			if (Callback)
			{
				Callback(Result);
			}
		}

		void Run()
		{
			Record->Status = ECursorCallStatus::Running;
			FCursorActivityLog::Get().NotifyUpdated(Record.ToSharedRef());

			if (Tool->GetRisk() != ECursorToolRisk::ReadOnly && !Tool->ManagesOwnTransaction() && GEditor && GEditor->CanTransact())
			{
				bTransactionWasActive = GEditor->IsTransactionActive();
				UndoQueueBefore = GEditor->Trans ? GEditor->Trans->GetQueueLength() : 0;
				GEditor->BeginTransaction(TEXT("CursorUnreal"),
					FText::FromString(FString::Printf(TEXT("Cursor Unreal: %s"), *Record->ToolName)), nullptr);
				bOpenedTransaction = true;
			}

			ExecStartedAt = FPlatformTime::Seconds();

			const TSharedRef<FInvocation> Self = AsShared();
			const FString ScopeName = FString::Printf(TEXT("CursorUnreal.Tool.%s"), *Record->ToolName);
			TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(*ScopeName);
			Tool->Execute(Args.ToSharedRef(), Ctx, [Self](const FCursorToolResult& Result)
			{
				if (!IsInGameThread())
				{
					UE_LOG(LogCursorUnreal, Error, TEXT("Tool '%s' completed off the game thread. Completion callbacks must run on the game thread."), *Self->Record->ToolName);
				}
				Self->Finish(Result, Result.bIsError ? ECursorCallStatus::Failed : ECursorCallStatus::Succeeded);
			});
		}
	};
}

namespace
{
	/** Empty when the arguments fit the tool's schema, or when the only problem is one the tool explains better itself. */
	FString DescribeMisnamedArguments(const ICursorTool& Tool, const FJsonObject& Args)
	{
		const TSharedRef<FJsonObject> Schema = Tool.GetInputSchema();
		const TSharedPtr<FJsonObject>* Properties = nullptr;
		if (!Schema->TryGetObjectField(TEXT("properties"), Properties) || !Properties || !Properties->IsValid())
		{
			return FString();
		}

		TArray<FString> Required;
		Schema->TryGetStringArrayField(TEXT("required"), Required);

		TArray<FString> Missing;
		for (const FString& Name : Required)
		{
			if (!Args.HasField(Name))
			{
				Missing.Add(Name);
			}
		}
		TArray<FString> Unknown;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Args.Values)
		{
			if (!(*Properties)->HasField(Pair.Key))
			{
				Unknown.Add(Pair.Key);
			}
		}
		if (Missing.Num() == 0 || Unknown.Num() == 0)
		{
			return FString();
		}

		TArray<FString> Accepted;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Properties)->Values)
		{
			Accepted.Add(Required.Contains(Pair.Key) ? Pair.Key + TEXT(" (required)") : Pair.Key);
		}
		return FString::Printf(TEXT("%s was called without its required argument%s '%s', and with '%s', which it does not take. Its arguments are: %s."),
			*Tool.GetName().ToString(), Missing.Num() > 1 ? TEXT("s") : TEXT(""), *FString::Join(Missing, TEXT("', '")),
			*FString::Join(Unknown, TEXT("', '")), *FString::Join(Accepted, TEXT(", ")));
	}
}

void FCursorToolRegistry::Invoke(FName Name, const TSharedPtr<FJsonObject>& InArgs, const FCursorToolContext& Ctx, FCursorToolCompletion OnComplete)
{
	check(IsInGameThread());

	FCursorActivity::Get().NoteActivity();

	const TSharedRef<FInvocation> Invocation = MakeShared<FInvocation>();
	Invocation->Args = InArgs.IsValid() ? InArgs : TSharedPtr<FJsonObject>(MakeShared<FJsonObject>());
	Invocation->Ctx = Ctx;
	Invocation->OnComplete = MoveTemp(OnComplete);
	Invocation->InvokedAt = FPlatformTime::Seconds();

	const TSharedRef<FCursorToolCallRecord> Record = MakeShared<FCursorToolCallRecord>();
	Record->Id = Ctx.CallId.IsEmpty() ? FGuid::NewGuid().ToString(EGuidFormats::Short) : Ctx.CallId;
	Record->ToolName = Name.ToString();
	Record->Caller = Ctx.Caller;
	Record->SessionId = Ctx.SessionId;
	// UTC, like every other timestamp the plugin records: the performance log writes it out as ISO 8601 with a Z.
	Record->StartedAt = FDateTime::UtcNow();
	Record->ArgsPreview = CursorJson::Truncate(CursorRedactor::Redact(CursorJson::ToString(Invocation->Args.ToSharedRef())), 4000);
	if (Ctx.ClientSentUnixMs > 0.0)
	{
		// Same machine, same clock. Discard anything implausible rather than report a wrong number.
		const double Delta = NowUnixMs() - Ctx.ClientSentUnixMs;
		if (Delta >= 0.0 && Delta < 60000.0)
		{
			Record->TransportMs = Delta;
		}
	}
	Invocation->Record = Record;
	FCursorActivityLog::Get().Add(Record);

	const TSharedPtr<ICursorTool> Tool = Find(Name);
	if (!Tool.IsValid())
	{
		Invocation->Finish(FCursorToolResult::Error(FString::Printf(TEXT("Unknown tool '%s'."), *Name.ToString())), ECursorCallStatus::Failed);
		return;
	}
	Invocation->Tool = Tool;
	Record->Risk = Tool->GetRisk();

	if (!UCursorUnrealSettings::Get()->IsPackEnabled(Tool->GetPack()))
	{
		Invocation->Finish(
			FCursorToolResult::Error(FString::Printf(TEXT("Tool '%s' belongs to the '%s' pack, which is switched off for this project."),
				*Name.ToString(), *Tool->GetPack().ToString())),
			ECursorCallStatus::Denied);
		return;
	}

	// Hiding a tool from a caller's list is not enough: it must not be callable by name either.
	if (Tool->GetAudience() == ECursorToolAudience::NativeAgentOnly && Ctx.Caller != CallerNative)
	{
		Invocation->Finish(
			FCursorToolResult::Error(FString::Printf(TEXT("Tool '%s' is only available to the in-editor agent. Use your own file tools instead."), *Name.ToString())),
			ECursorCallStatus::Denied);
		return;
	}

	// A required argument that is absent, next to one the tool does not take, is a misnamed argument
	// ("blueprint" where the tool wants "object"). Each tool would report only the half it knows about
	// ("Object path is empty"); say both halves, before anyone is asked to approve the call.
	const FString ArgumentProblem = DescribeMisnamedArguments(*Tool, *Invocation->Args);
	if (!ArgumentProblem.IsEmpty())
	{
		Invocation->Finish(FCursorToolResult::Error(ArgumentProblem), ECursorCallStatus::Failed);
		return;
	}

	if (Ctx.bPreApproved)
	{
		Invocation->Run();
		return;
	}

	Record->Status = ECursorCallStatus::AwaitingApproval;
	FCursorApprovalGate::Get().Evaluate(*Tool, Invocation->Args.ToSharedRef(), Ctx,
		[Invocation](bool bAllowed, const FString& DenyReason, double ApprovalMs)
		{
			Invocation->Record->ApprovalMs = ApprovalMs;
			if (!bAllowed)
			{
				Invocation->Finish(FCursorToolResult::Error(DenyReason), ECursorCallStatus::Denied);
				return;
			}
			Invocation->Run();
		});
}
