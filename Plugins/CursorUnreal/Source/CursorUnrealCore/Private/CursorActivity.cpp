#include "CursorActivity.h"

#include "CursorUnrealLog.h"
#include "CursorUnrealSettings.h"
#include "Editor.h"
#include "HAL/PlatformTime.h"

const TCHAR* LexToString(ECursorCallStatus Status)
{
	switch (Status)
	{
	case ECursorCallStatus::Queued: return TEXT("queued");
	case ECursorCallStatus::AwaitingApproval: return TEXT("awaiting approval");
	case ECursorCallStatus::Running: return TEXT("running");
	case ECursorCallStatus::Succeeded: return TEXT("ok");
	case ECursorCallStatus::Failed: return TEXT("failed");
	default: return TEXT("denied");
	}
}

// ---------------------------------------------------------------------------------------------
// FCursorActivityLog

FCursorActivityLog& FCursorActivityLog::Get()
{
	static FCursorActivityLog Instance;
	return Instance;
}

void FCursorActivityLog::Add(const TSharedRef<FCursorToolCallRecord>& Record)
{
	if (Records.Num() >= MaxRecords)
	{
		Records.RemoveAt(0, Records.Num() - MaxRecords + 1);
	}
	Records.Add(Record);
	OnAdded.Broadcast(Record);
}

void FCursorActivityLog::NotifyUpdated(const TSharedRef<FCursorToolCallRecord>& Record)
{
	OnUpdated.Broadcast(Record);
}

void FCursorActivityLog::Clear()
{
	Records.Reset();
	OnCleared.Broadcast();
}

// ---------------------------------------------------------------------------------------------
// FCursorActivity

FCursorActivity& FCursorActivity::Get()
{
	static FCursorActivity Instance;
	return Instance;
}

void FCursorActivity::NoteActivity()
{
	LastActivitySeconds = FPlatformTime::Seconds();
}

void FCursorActivity::BeginSession()
{
	++ActiveSessions;
	NoteActivity();
}

void FCursorActivity::EndSession()
{
	ActiveSessions = FMath::Max(0, ActiveSessions - 1);
	NoteActivity();
}

bool FCursorActivity::IsAgentActive() const
{
	return ActiveSessions > 0 || (FPlatformTime::Seconds() - LastActivitySeconds) < GraceSeconds;
}

bool FCursorActivity::ShouldStayResponsive() const
{
	return UCursorUnrealSettings::Get()->bStayResponsiveInBackground && IsAgentActive();
}

void FCursorActivity::InstallThrottleOverride()
{
	if (GEditor == nullptr || ThrottleHandle.IsValid())
	{
		return;
	}
	UEditorEngine::FShouldDisableCPUThrottling Delegate =
		UEditorEngine::FShouldDisableCPUThrottling::CreateLambda([]() { return FCursorActivity::Get().ShouldStayResponsive(); });
	ThrottleHandle = Delegate.GetHandle();
	GEditor->ShouldDisableCPUThrottlingDelegates.Add(MoveTemp(Delegate));
	// Logged at startup so a log file shows which way a latency measurement was taken.
	UE_LOG(LogCursorUnreal, Log, TEXT("Background throttle override installed. Stay responsive in background: %s (applies only while an agent is active, or within %.0f s of its last request)."),
		UCursorUnrealSettings::Get()->bStayResponsiveInBackground ? TEXT("on") : TEXT("off"), GraceSeconds);
}

void FCursorActivity::RemoveThrottleOverride()
{
	if (GEditor != nullptr && ThrottleHandle.IsValid())
	{
		const FDelegateHandle Handle = ThrottleHandle;
		GEditor->ShouldDisableCPUThrottlingDelegates.RemoveAll(
			[Handle](const UEditorEngine::FShouldDisableCPUThrottling& Existing) { return Existing.GetHandle() == Handle; });
	}
	ThrottleHandle.Reset();
}
