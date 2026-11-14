#include "CursorPerfLog.h"

#include "CursorJson.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

FCursorPerfLog& FCursorPerfLog::Get()
{
	static FCursorPerfLog Instance;
	return Instance;
}

FString FCursorPerfLog::GetLogDirectory()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("CursorUnreal") / TEXT("Logs"));
}

FString FCursorPerfLog::GetCurrentLogFile() const
{
	return GetLogDirectory() / FString::Printf(TEXT("perf-%s.jsonl"), *FDateTime::Now().ToString(TEXT("%Y%m%d")));
}

void FCursorPerfLog::AppendLine(const FString& Line) const
{
	if (!bWriteToDisk)
	{
		return;
	}
	IFileManager::Get().MakeDirectory(*GetLogDirectory(), true);
	FFileHelper::SaveStringToFile(Line + TEXT("\n"), *GetCurrentLogFile(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
		&IFileManager::Get(), FILEWRITE_Append);
}

void FCursorPerfLog::RecordTool(const FCursorToolCallRecord& Record)
{
	FCursorToolAggregate& Aggregate = Aggregates.FindOrAdd(Record.ToolName);
	Aggregate.ToolName = Record.ToolName;
	Aggregate.Calls++;
	if (Record.Status != ECursorCallStatus::Succeeded)
	{
		Aggregate.Errors++;
	}
	Aggregate.TotalQueueMs += Record.QueueMs;
	Aggregate.TotalExecMs += Record.ExecMs;
	Aggregate.MaxExecMs = FMath::Max(Aggregate.MaxExecMs, Record.ExecMs);
	if (Record.TransportMs >= 0.0)
	{
		Aggregate.TotalTransportMs += Record.TransportMs;
		Aggregate.TransportSamples++;
	}

	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("type"), TEXT("tool"));
	Json->SetStringField(TEXT("time"), Record.StartedAt.ToIso8601());
	Json->SetStringField(TEXT("tool"), Record.ToolName);
	Json->SetStringField(TEXT("caller"), Record.Caller);
	Json->SetStringField(TEXT("session"), Record.SessionId);
	Json->SetStringField(TEXT("status"), LexToString(Record.Status));
	Json->SetNumberField(TEXT("queue_ms"), Record.QueueMs);
	Json->SetNumberField(TEXT("approval_ms"), Record.ApprovalMs);
	Json->SetNumberField(TEXT("exec_ms"), Record.ExecMs);
	if (Record.TransportMs >= 0.0)
	{
		Json->SetNumberField(TEXT("transport_ms"), Record.TransportMs);
	}
	Json->SetNumberField(TEXT("result_chars"), Record.ResultChars);
	AppendLine(CursorJson::ToString(Json));

	OnChanged.Broadcast();
}

void FCursorPerfLog::RecordTurn(const FCursorTurnPerf& Turn)
{
	if (Turns.Num() >= MaxTurns)
	{
		Turns.RemoveAt(0, Turns.Num() - MaxTurns + 1);
	}
	Turns.Add(Turn);

	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("type"), TEXT("turn"));
	Json->SetStringField(TEXT("time"), Turn.StartedAt.ToIso8601());
	Json->SetStringField(TEXT("provider"), Turn.Provider);
	Json->SetStringField(TEXT("model"), Turn.Model);
	Json->SetStringField(TEXT("session"), Turn.SessionId);
	Json->SetStringField(TEXT("outcome"), Turn.Outcome);
	Json->SetNumberField(TEXT("ttft_ms"), Turn.TimeToFirstTokenMs);
	Json->SetNumberField(TEXT("total_ms"), Turn.TotalMs);
	Json->SetNumberField(TEXT("model_ms"), Turn.ModelMs);
	Json->SetNumberField(TEXT("tool_ms"), Turn.ToolMs);
	Json->SetNumberField(TEXT("round_trips"), Turn.RoundTrips);
	Json->SetNumberField(TEXT("tool_calls"), Turn.ToolCalls);
	Json->SetNumberField(TEXT("input_tokens"), static_cast<double>(Turn.InputTokens));
	Json->SetNumberField(TEXT("output_tokens"), static_cast<double>(Turn.OutputTokens));
	Json->SetNumberField(TEXT("cache_read_tokens"), static_cast<double>(Turn.CacheReadTokens));
	Json->SetNumberField(TEXT("cache_write_tokens"), static_cast<double>(Turn.CacheWriteTokens));
	AppendLine(CursorJson::ToString(Json));

	OnChanged.Broadcast();
}

TArray<FCursorToolAggregate> FCursorPerfLog::GetToolAggregates() const
{
	TArray<FCursorToolAggregate> Out;
	Aggregates.GenerateValueArray(Out);
	Out.Sort([](const FCursorToolAggregate& A, const FCursorToolAggregate& B) { return A.TotalExecMs > B.TotalExecMs; });
	return Out;
}

bool FCursorPerfLog::ExportCsv(const FString& Path, FString& OutError) const
{
	FString Csv;
	Csv += TEXT("section,time,provider,model,outcome,ttft_ms,total_ms,model_ms,tool_ms,round_trips,tool_calls,input_tokens,output_tokens,cache_read_tokens,cache_write_tokens\n");
	for (const FCursorTurnPerf& T : Turns)
	{
		Csv += FString::Printf(TEXT("turn,%s,%s,%s,%s,%.1f,%.1f,%.1f,%.1f,%d,%d,%lld,%lld,%lld,%lld\n"),
			*T.StartedAt.ToIso8601(), *T.Provider, *T.Model, *T.Outcome,
			T.TimeToFirstTokenMs, T.TotalMs, T.ModelMs, T.ToolMs, T.RoundTrips, T.ToolCalls,
			T.InputTokens, T.OutputTokens, T.CacheReadTokens, T.CacheWriteTokens);
	}

	Csv += TEXT("\nsection,tool,calls,errors,avg_queue_ms,avg_exec_ms,max_exec_ms,avg_transport_ms\n");
	for (const FCursorToolAggregate& A : GetToolAggregates())
	{
		const double Calls = FMath::Max(1, A.Calls);
		const FString Transport = A.TransportSamples > 0
			? FString::Printf(TEXT("%.2f"), A.TotalTransportMs / A.TransportSamples)
			: FString();
		Csv += FString::Printf(TEXT("tool,%s,%d,%d,%.2f,%.2f,%.2f,%s\n"),
			*A.ToolName, A.Calls, A.Errors, A.TotalQueueMs / Calls, A.TotalExecMs / Calls, A.MaxExecMs, *Transport);
	}

	if (!FFileHelper::SaveStringToFile(Csv, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = FString::Printf(TEXT("Could not write %s"), *Path);
		return false;
	}
	return true;
}

void FCursorPerfLog::Clear()
{
	Turns.Reset();
	Aggregates.Reset();
	OnChanged.Broadcast();
}
