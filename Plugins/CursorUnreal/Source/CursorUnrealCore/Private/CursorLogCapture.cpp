#include "CursorLogCapture.h"

#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

FCursorLogCapture& FCursorLogCapture::Get()
{
	static FCursorLogCapture Instance;
	return Instance;
}

void FCursorLogCapture::Start()
{
	if (!bStarted && GLog)
	{
		GLog->AddOutputDevice(this);
		bStarted = true;
	}
}

void FCursorLogCapture::Stop()
{
	if (bStarted && GLog)
	{
		GLog->RemoveOutputDevice(this);
	}
	bStarted = false;
}

void FCursorLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock ScopeLock(&Lock);
	if (Lines.Num() >= MaxLines)
	{
		// Drop a block at a time so this is not an O(n) shift on every line.
		Lines.RemoveAt(0, MaxLines / 4, EAllowShrinking::No);
	}
	FCursorLogLine& Line = Lines.AddDefaulted_GetRef();
	Line.Sequence = NextSequence++;
	Line.Category = Category;
	Line.Verbosity = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
	Line.Message = V;
}

int64 FCursorLogCapture::GetLatestSequence() const
{
	FScopeLock ScopeLock(&Lock);
	return NextSequence - 1;
}

TArray<FCursorLogLine> FCursorLogCapture::GetLines(int64 AfterSequence, int32 InMaxLines, ELogVerbosity::Type MinVerbosity,
	const FString& CategoryContains, const FString& TextContains) const
{
	TArray<FCursorLogLine> Out;
	FScopeLock ScopeLock(&Lock);
	for (int32 i = Lines.Num() - 1; i >= 0 && Out.Num() < InMaxLines; --i)
	{
		const FCursorLogLine& Line = Lines[i];
		if (Line.Sequence <= AfterSequence)
		{
			break;
		}
		// Lower numeric verbosity is more severe (Fatal = 1, Error = 2, Warning = 3 ...).
		if (MinVerbosity != ELogVerbosity::All && Line.Verbosity > MinVerbosity)
		{
			continue;
		}
		if (!CategoryContains.IsEmpty() && !Line.Category.ToString().Contains(CategoryContains))
		{
			continue;
		}
		if (!TextContains.IsEmpty() && !Line.Message.Contains(TextContains))
		{
			continue;
		}
		Out.Add(Line);
	}
	Algo::Reverse(Out);
	return Out;
}

FString FCursorLogCapture::Format(const FCursorLogLine& Line)
{
	const TCHAR* Level = TEXT("");
	switch (Line.Verbosity)
	{
	case ELogVerbosity::Fatal: Level = TEXT("Fatal: "); break;
	case ELogVerbosity::Error: Level = TEXT("Error: "); break;
	case ELogVerbosity::Warning: Level = TEXT("Warning: "); break;
	default: break;
	}
	return FString::Printf(TEXT("%s: %s%s"), *Line.Category.ToString(), Level, *Line.Message);
}
