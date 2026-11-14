#include "SseParser.h"

void FSseParser::Reset()
{
	Line.Reset();
	EventName.Reset();
	Data.Reset();
	bHasData = false;
	bLastByteWasCR = false;
	bAtStreamStart = true;
	bOverflowed = false;
}

FString FSseParser::Decode(const TArray<uint8>& Utf8)
{
	if (Utf8.Num() == 0)
	{
		return FString();
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Utf8.GetData()), Utf8.Num());
	return FString(FStringView(Converted.Get(), Converted.Length()));
}

void FSseParser::Feed(const uint8* Bytes, int32 NumBytes, TArray<FSseEvent>& OutEvents)
{
	for (int32 Index = 0; Index < NumBytes && !bOverflowed; ++Index)
	{
		const uint8 Byte = Bytes[Index];

		// A line ends at CR, LF or CRLF. The LF of a CRLF may arrive in the next chunk.
		if (bLastByteWasCR)
		{
			bLastByteWasCR = false;
			if (Byte == '\n')
			{
				continue;
			}
		}

		if (Byte == '\r')
		{
			bLastByteWasCR = true;
			EndLine(OutEvents);
		}
		else if (Byte == '\n')
		{
			EndLine(OutEvents);
		}
		else
		{
			Line.Add(Byte);
			if (Line.Num() > MaxEventBytes)
			{
				bOverflowed = true;
			}
		}
	}
}

void FSseParser::EndLine(TArray<FSseEvent>& OutEvents)
{
	if (bAtStreamStart)
	{
		bAtStreamStart = false;
		// A byte order mark is allowed once, at the very start of the stream.
		if (Line.Num() >= 3 && Line[0] == 0xEF && Line[1] == 0xBB && Line[2] == 0xBF)
		{
			Line.RemoveAt(0, 3, EAllowShrinking::No);
		}
	}

	if (Line.Num() == 0)
	{
		// A blank line dispatches the event. One with no data is discarded, as the SSE spec says.
		if (bHasData)
		{
			// Every data line added a trailing newline; the last one is not part of the value.
			if (Data.Num() > 0 && Data.Last() == '\n')
			{
				Data.Pop(EAllowShrinking::No);
			}
			FSseEvent& Event = OutEvents.AddDefaulted_GetRef();
			Event.Event = Decode(EventName);
			Event.Data = Decode(Data);
		}
		EventName.Reset();
		Data.Reset();
		bHasData = false;
		return;
	}

	// A line starting with a colon is a comment, often used as a keep-alive.
	if (Line[0] != ':')
	{
		int32 Colon = INDEX_NONE;
		Line.Find(static_cast<uint8>(':'), Colon);

		const int32 NameLength = Colon == INDEX_NONE ? Line.Num() : Colon;
		int32 ValueStart = Colon == INDEX_NONE ? Line.Num() : Colon + 1;
		// One space after the colon is part of the syntax, not of the value.
		if (ValueStart < Line.Num() && Line[ValueStart] == ' ')
		{
			++ValueStart;
		}
		const int32 ValueLength = Line.Num() - ValueStart;

		auto NameIs = [this, NameLength](const ANSICHAR* Name, int32 Length)
		{
			return NameLength == Length && FMemory::Memcmp(Line.GetData(), Name, Length) == 0;
		};

		if (NameIs("data", 4))
		{
			if (Data.Num() + ValueLength + 1 > MaxEventBytes)
			{
				bOverflowed = true;
			}
			else
			{
				Data.Append(Line.GetData() + ValueStart, ValueLength);
				Data.Add('\n');
				bHasData = true;
			}
		}
		else if (NameIs("event", 5))
		{
			EventName.Reset();
			EventName.Append(Line.GetData() + ValueStart, ValueLength);
		}
		// "id", "retry" and anything unknown are ignored.
	}

	Line.Reset();
}
