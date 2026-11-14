#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"

/**
 * A Node process talking NDJSON on stdin/stdout. The parent owns reading and writing; stderr is
 * kept for troubleshooting and never mixed into the protocol. All methods are called on the game
 * thread. Bytes arrive on a tick; complete lines are handed to OnLine.
 */
class FCursorBridgeProcess
{
public:
	FCursorBridgeProcess();
	~FCursorBridgeProcess();

	FCursorBridgeProcess(const FCursorBridgeProcess&) = delete;
	FCursorBridgeProcess& operator=(const FCursorBridgeProcess&) = delete;

	/** Starts node with the given script and arguments. Working directory is the Bridge folder. */
	bool Start(const FString& NodePath, const FString& ScriptPath, const TArray<FString>& ExtraArgs, FString& OutError);
	/** Asks the process to stop, waits briefly, then kills it if needed. */
	void Stop();

	bool IsRunning();
	uint32 GetProcessId() const { return ProcessId; }

	/** Writes one UTF-8 JSON line (a trailing newline is added). False when the pipe is closed. */
	bool WriteLine(const FString& Line);

	/** Called for each complete line of stdout, without the newline. */
	TFunction<void(const FString& Line)> OnLine;
	/** Called once when the process exits or the pipes fail. */
	TFunction<void(int32 ExitCode)> OnExit;

private:
	void Pump();
	void HandleStdout(const FString& Chunk);
	void Finish(int32 ExitCode);

	FProcHandle Process;
	uint32 ProcessId = 0;
	void* StdinWrite = nullptr;
	void* StdoutRead = nullptr;
	void* StderrRead = nullptr;
	FString LineBuffer;
	FString StderrBuffer;
	FTSTicker::FDelegateHandle TickHandle;
	bool bStopping = false;
	bool bFinished = false;
};
