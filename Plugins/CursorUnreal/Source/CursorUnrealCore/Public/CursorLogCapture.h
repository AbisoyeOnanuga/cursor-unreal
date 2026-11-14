#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

struct FCursorLogLine
{
	/** Increases by one per captured line, so callers can ask for "everything after N". */
	int64 Sequence = 0;
	FName Category;
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FString Message;
};

/**
 * Keeps the most recent editor log lines in memory so tools can show an agent what just
 * happened (compile errors, warnings from the thing it just changed) without reading log files.
 */
class CURSORUNREALCORE_API FCursorLogCapture : public FOutputDevice
{
public:
	static FCursorLogCapture& Get();

	void Start();
	void Stop();

	/** Sequence number of the most recent line. Take this before an operation to read only what it logged. */
	int64 GetLatestSequence() const;

	/**
	 * Lines after AfterSequence, oldest first, at most MaxLines (the newest ones when there are more).
	 * MinVerbosity keeps lines at least that severe; ELogVerbosity::All keeps everything.
	 */
	TArray<FCursorLogLine> GetLines(int64 AfterSequence, int32 MaxLines, ELogVerbosity::Type MinVerbosity = ELogVerbosity::All,
		const FString& CategoryContains = FString(), const FString& TextContains = FString()) const;

	static FString Format(const FCursorLogLine& Line);

	// FOutputDevice
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

private:
	static constexpr int32 MaxLines = 4000;

	mutable FCriticalSection Lock;
	TArray<FCursorLogLine> Lines;
	int64 NextSequence = 1;
	bool bStarted = false;
};
