#pragma once

#include "CoreMinimal.h"
#include "CursorTool.h"

enum class ECursorCallStatus : uint8
{
	Queued,
	AwaitingApproval,
	Running,
	Succeeded,
	Failed,
	Denied
};

CURSORUNREALCORE_API const TCHAR* LexToString(ECursorCallStatus Status);

/** One tool call, as shown in the Activity tab and written to the performance log. */
struct FCursorToolCallRecord
{
	FString Id;
	FString ToolName;
	FString Caller;
	FString SessionId;
	/** Redacted and truncated. */
	FString ArgsPreview;
	/** Redacted and truncated. */
	FString ResultPreview;
	ECursorToolRisk Risk = ECursorToolRisk::ReadOnly;
	ECursorCallStatus Status = ECursorCallStatus::Queued;
	/** UTC. */
	FDateTime StartedAt;

	/** Time between the call arriving and the tool starting, not counting time spent waiting for approval. */
	double QueueMs = 0.0;
	/** Time spent waiting for the user to approve. */
	double ApprovalMs = 0.0;
	/** Time from the tool starting to it reporting completion. */
	double ExecMs = 0.0;
	/** Time from the client sending the request to the editor receiving it. Negative when the client did not say. */
	double TransportMs = -1.0;

	/** True when the call left an entry in the editor's undo history. */
	bool bRecordedUndo = false;
	int32 ResultChars = 0;
};

/** Recent tool calls, newest last. The Activity tab reads this; nothing here is persisted. */
class CURSORUNREALCORE_API FCursorActivityLog
{
public:
	static FCursorActivityLog& Get();

	void Add(const TSharedRef<FCursorToolCallRecord>& Record);
	void NotifyUpdated(const TSharedRef<FCursorToolCallRecord>& Record);
	const TArray<TSharedRef<FCursorToolCallRecord>>& GetRecords() const { return Records; }
	void Clear();

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnRecord, const TSharedRef<FCursorToolCallRecord>&);
	FOnRecord OnAdded;
	FOnRecord OnUpdated;
	FSimpleMulticastDelegate OnCleared;

private:
	static constexpr int32 MaxRecords = 500;
	TArray<TSharedRef<FCursorToolCallRecord>> Records;
};

/**
 * Tracks whether an agent is working, and uses that to stop the editor dropping to 3 ticks per
 * second when it is not the focused window. Tools run on the game thread, so a throttled editor
 * makes every tool call wait for the next tick whichever way the call arrived.
 */
class CURSORUNREALCORE_API FCursorActivity
{
public:
	static FCursorActivity& Get();

	/** Call whenever a tool request arrives. Keeps the editor awake for a short window afterwards. */
	void NoteActivity();
	/** Bracket a running agent turn. */
	void BeginSession();
	void EndSession();

	/** True while a turn is running, and for GraceSeconds after the last tool request. */
	bool IsAgentActive() const;
	/** Turns running right now. */
	int32 NumActiveSessions() const { return ActiveSessions; }
	/** What the engine asks: should throttling be skipped right now? */
	bool ShouldStayResponsive() const;

	void InstallThrottleOverride();
	void RemoveThrottleOverride();

	/** Seconds the editor stays at full tick rate after the last tool request. */
	static constexpr double GraceSeconds = 30.0;

private:
	int32 ActiveSessions = 0;
	double LastActivitySeconds = -1.0e9;
	FDelegateHandle ThrottleHandle;
};
