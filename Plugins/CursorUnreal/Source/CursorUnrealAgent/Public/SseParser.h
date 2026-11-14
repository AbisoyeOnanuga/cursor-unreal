#pragma once

#include "CoreMinimal.h"

/** One server-sent event: the value of its "event:" field and its "data:" lines joined with newlines. */
struct FSseEvent
{
	FString Event;
	FString Data;
};

/**
 * Incremental parser for a text/event-stream body.
 *
 * Works on bytes, and only turns them into text once a whole event has arrived. Network chunks
 * can end anywhere, including half-way through a multi-byte UTF-8 character or between the CR
 * and LF of a line ending, and decoding chunk by chunk would corrupt both.
 */
class CURSORUNREALAGENT_API FSseParser
{
public:
	/** Appends bytes and adds every event they complete to OutEvents. */
	void Feed(const uint8* Bytes, int32 NumBytes, TArray<FSseEvent>& OutEvents);
	void Feed(TConstArrayView<uint8> Bytes, TArray<FSseEvent>& OutEvents) { Feed(Bytes.GetData(), Bytes.Num(), OutEvents); }

	/** True when the stream stopped part-way through a line or an event. Whatever is pending is never delivered. */
	bool HasPendingData() const { return Line.Num() > 0 || bHasData || EventName.Num() > 0; }

	/** True once a line or an event grew past MaxEventBytes. Later input is ignored. */
	bool HasOverflowed() const { return bOverflowed; }

	void Reset();

	/** Far above anything a model API sends in one event; stops a broken or hostile endpoint filling memory. */
	static constexpr int32 MaxEventBytes = 32 * 1024 * 1024;

private:
	void EndLine(TArray<FSseEvent>& OutEvents);
	static FString Decode(const TArray<uint8>& Utf8);

	TArray<uint8> Line;
	TArray<uint8> EventName;
	TArray<uint8> Data;
	bool bHasData = false;
	bool bLastByteWasCR = false;
	bool bAtStreamStart = true;
	bool bOverflowed = false;
};
