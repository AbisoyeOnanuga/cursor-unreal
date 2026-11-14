#pragma once

#include "CoreMinimal.h"
#include "CursorActivity.h"

/** Timing and token counts for one model turn (one user message, however many round trips it took). */
struct FCursorTurnPerf
{
	/** UTC. */
	FDateTime StartedAt;
	FString Provider;
	FString Model;
	FString SessionId;
	/** "done", "cancelled", "error" or "max_iterations". */
	FString Outcome;

	/** Time from sending the first request to the first streamed token. */
	double TimeToFirstTokenMs = -1.0;
	/** Wall-clock time for the whole turn. */
	double TotalMs = 0.0;
	/** Time spent waiting on the model, summed over round trips. */
	double ModelMs = 0.0;
	/** Time spent running tools, summed. */
	double ToolMs = 0.0;

	int32 RoundTrips = 0;
	int32 ToolCalls = 0;
	int64 InputTokens = 0;
	int64 OutputTokens = 0;
	int64 CacheReadTokens = 0;
	int64 CacheWriteTokens = 0;
};

/** Per-tool rollup for the Performance tab. */
struct FCursorToolAggregate
{
	FString ToolName;
	int32 Calls = 0;
	int32 Errors = 0;
	double TotalQueueMs = 0.0;
	double TotalExecMs = 0.0;
	double MaxExecMs = 0.0;
	double TotalTransportMs = 0.0;
	int32 TransportSamples = 0;
};

/**
 * Performance log. Every tool call and model turn is appended to
 * Saved/CursorUnreal/Logs/perf-YYYYMMDD.jsonl and kept in memory for the Performance tab.
 * Records hold names, timings and counts only: no prompts, arguments or results.
 */
class CURSORUNREALCORE_API FCursorPerfLog
{
public:
	static FCursorPerfLog& Get();

	void RecordTool(const FCursorToolCallRecord& Record);
	void RecordTurn(const FCursorTurnPerf& Turn);

	const TArray<FCursorTurnPerf>& GetTurns() const { return Turns; }
	TArray<FCursorToolAggregate> GetToolAggregates() const;

	/** Writes this session's turns and tool rollups as CSV. */
	bool ExportCsv(const FString& Path, FString& OutError) const;

	static FString GetLogDirectory();
	FString GetCurrentLogFile() const;

	void Clear();

	FSimpleMulticastDelegate OnChanged;

	/** Set false in tests to keep them from writing to disk. */
	bool bWriteToDisk = true;

private:
	void AppendLine(const FString& Line) const;

	static constexpr int32 MaxTurns = 500;
	TArray<FCursorTurnPerf> Turns;
	TMap<FString, FCursorToolAggregate> Aggregates;
};
