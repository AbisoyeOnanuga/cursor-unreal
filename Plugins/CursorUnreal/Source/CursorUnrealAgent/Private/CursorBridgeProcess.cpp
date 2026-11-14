#include "CursorBridgeProcess.h"

#include "Containers/Ticker.h"
#include "CursorSecrets.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

FCursorBridgeProcess::FCursorBridgeProcess() = default;

FCursorBridgeProcess::~FCursorBridgeProcess()
{
	Stop();
}

bool FCursorBridgeProcess::Start(const FString& NodePath, const FString& ScriptPath, const TArray<FString>& ExtraArgs, FString& OutError)
{
	Stop();
	bStopping = false;
	bFinished = false;
	LineBuffer.Reset();
	StderrBuffer.Reset();

	if (!FPaths::FileExists(NodePath))
	{
		OutError = FString::Printf(TEXT("Node was not found at \"%s\"."), *NodePath);
		return false;
	}
	if (!FPaths::FileExists(ScriptPath))
	{
		OutError = FString::Printf(TEXT("The bridge script was not found at \"%s\"."), *ScriptPath);
		return false;
	}

	void* StdoutWrite = nullptr;
	void* StdinRead = nullptr;
	void* StderrWrite = nullptr;
	if (!FPlatformProcess::CreatePipe(StdoutRead, StdoutWrite)
		|| !FPlatformProcess::CreatePipe(StdinRead, StdinWrite, true)
		|| !FPlatformProcess::CreatePipe(StderrRead, StderrWrite))
	{
		OutError = TEXT("Could not create pipes for the Cursor bridge.");
		FPlatformProcess::ClosePipe(StdoutRead, StdoutWrite);
		FPlatformProcess::ClosePipe(StdinRead, StdinWrite);
		FPlatformProcess::ClosePipe(StderrRead, StderrWrite);
		StdoutRead = StdinWrite = StderrRead = nullptr;
		return false;
	}

	FString Args = FString::Printf(TEXT("\"%s\""), *ScriptPath);
	for (const FString& Extra : ExtraArgs)
	{
		Args += TEXT(" ");
		Args += Extra.Contains(TEXT(" ")) ? FString::Printf(TEXT("\"%s\""), *Extra) : Extra;
	}

	const FString WorkDir = FPaths::GetPath(ScriptPath);
	Process = FPlatformProcess::CreateProc(
		*NodePath,
		*Args,
		true,  // detached: do not die with a console
		true,  // hidden
		true,  // really hidden
		&ProcessId,
		0,
		*WorkDir,
		StdoutWrite,
		StdinRead,
		StderrWrite);

	// The child has its own copies. Closing ours is required so we see EOF when it exits.
	FPlatformProcess::ClosePipe(nullptr, StdoutWrite);
	FPlatformProcess::ClosePipe(StdinRead, nullptr);
	FPlatformProcess::ClosePipe(nullptr, StderrWrite);

	if (!Process.IsValid())
	{
		OutError = FString::Printf(TEXT("Could not start \"%s %s\"."), *NodePath, *Args);
		FPlatformProcess::ClosePipe(StdoutRead, nullptr);
		FPlatformProcess::ClosePipe(nullptr, StdinWrite);
		FPlatformProcess::ClosePipe(StderrRead, nullptr);
		StdoutRead = StdinWrite = StderrRead = nullptr;
		ProcessId = 0;
		return false;
	}

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[this](float)
		{
			Pump();
			return true;
		}), 0.0f);
	return true;
}

void FCursorBridgeProcess::Stop()
{
	bStopping = true;
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}

	// Closing stdin tells the bridge to shut down (it treats end-of-stdin as a stop signal).
	if (StdinWrite)
	{
		FPlatformProcess::ClosePipe(nullptr, StdinWrite);
		StdinWrite = nullptr;
	}

	if (Process.IsValid())
	{
		const double Deadline = FPlatformTime::Seconds() + 2.0;
		while (FPlatformProcess::IsProcRunning(Process) && FPlatformTime::Seconds() < Deadline)
		{
			Pump();
			FPlatformProcess::Sleep(0.02f);
		}
		if (FPlatformProcess::IsProcRunning(Process))
		{
			FPlatformProcess::TerminateProc(Process, true);
		}
		int32 Code = 0;
		FPlatformProcess::GetProcReturnCode(Process, &Code);
		FPlatformProcess::CloseProc(Process);
		Finish(Code);
	}
	else
	{
		Finish(-1);
	}

	if (StdoutRead)
	{
		FPlatformProcess::ClosePipe(StdoutRead, nullptr);
		StdoutRead = nullptr;
	}
	if (StderrRead)
	{
		FPlatformProcess::ClosePipe(StderrRead, nullptr);
		StderrRead = nullptr;
	}
	ProcessId = 0;
}

bool FCursorBridgeProcess::IsRunning()
{
	return Process.IsValid() && FPlatformProcess::IsProcRunning(Process) && !bFinished;
}

bool FCursorBridgeProcess::WriteLine(const FString& Line)
{
	if (!StdinWrite || bFinished)
	{
		return false;
	}
	// WritePipe(FString) already appends a newline.
	return FPlatformProcess::WritePipe(StdinWrite, Line);
}

void FCursorBridgeProcess::Pump()
{
	if (bFinished)
	{
		return;
	}

	for (;;)
	{
		TArray<uint8> Chunk;
		if (!StdoutRead || !FPlatformProcess::ReadPipeToArray(StdoutRead, Chunk) || Chunk.Num() == 0)
		{
			break;
		}
		Chunk.Add(0);
		HandleStdout(UTF8_TO_TCHAR(reinterpret_cast<const char*>(Chunk.GetData())));
	}

	if (StderrRead)
	{
		const FString Err = FPlatformProcess::ReadPipe(StderrRead);
		if (!Err.IsEmpty())
		{
			StderrBuffer += Err;
			// Keep the last few kilobytes only; it is for the next failure message, not a log.
			if (StderrBuffer.Len() > 8000)
			{
				StderrBuffer.RightChopInline(StderrBuffer.Len() - 8000);
			}
		}
	}

	if (Process.IsValid() && !FPlatformProcess::IsProcRunning(Process))
	{
		int32 Code = 0;
		FPlatformProcess::GetProcReturnCode(Process, &Code);
		FPlatformProcess::CloseProc(Process);
		if (StdinWrite)
		{
			FPlatformProcess::ClosePipe(nullptr, StdinWrite);
			StdinWrite = nullptr;
		}
		Finish(Code);
	}
}

void FCursorBridgeProcess::HandleStdout(const FString& Chunk)
{
	LineBuffer += Chunk;
	for (;;)
	{
		int32 Newline = INDEX_NONE;
		if (!LineBuffer.FindChar(TEXT('\n'), Newline))
		{
			break;
		}
		FString Line = LineBuffer.Left(Newline);
		LineBuffer.RightChopInline(Newline + 1);
		Line.TrimStartAndEndInline();
		if (Line.EndsWith(TEXT("\r")))
		{
			Line.LeftChopInline(1);
		}
		if (!Line.IsEmpty() && OnLine)
		{
			OnLine(Line);
		}
	}
}

void FCursorBridgeProcess::Finish(int32 ExitCode)
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	if (OnExit)
	{
		OnExit(ExitCode);
	}
}
