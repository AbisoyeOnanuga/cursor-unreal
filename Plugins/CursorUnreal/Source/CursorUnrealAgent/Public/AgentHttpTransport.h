#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

/** What a model backend asks the network layer to send. */
struct FAgentHttpRequest
{
	FString Verb = TEXT("POST");
	FString Url;
	/** Includes credentials. Never log this map. */
	TMap<FString, FString> Headers;
	FString Body;
	/** Deliver the response body in pieces as it arrives instead of all at once at the end. */
	bool bStream = false;
	/** Give up when nothing is sent or received for this long. */
	float ActivityTimeoutSeconds = 120.0f;
};

struct FAgentHttpResponse
{
	/** HTTP status, or 0 when no response arrived at all. */
	int32 Status = 0;
	/** False when the connection failed or dropped. A streamed body may then be incomplete even if Status is 200. */
	bool bCompleted = false;
	/** Why it did not complete: "connection error", "timed out" and so on. */
	FString FailureReason;
	/** Header names in lower case. */
	TMap<FString, FString> Headers;
	/** The whole body. Empty for streamed requests, whose body went to the chunk callback. */
	FString Body;
};

using FAgentHttpChunk = TFunction<void(TConstArrayView<uint8>)>;
using FAgentHttpComplete = TFunction<void(const FAgentHttpResponse&)>;

/** A request in flight. */
class IAgentHttpCall
{
public:
	virtual ~IAgentHttpCall() = default;
	/** Stops the request. Neither callback fires after this returns. */
	virtual void Cancel() = 0;
};

/**
 * The seam between model backends and the network, so backends can be tested against recorded
 * or hand-written responses without a key, a connection or any cost.
 *
 * Both callbacks run on the game thread and never from inside Send.
 */
class IAgentHttpTransport
{
public:
	virtual ~IAgentHttpTransport() = default;
	virtual TSharedRef<IAgentHttpCall> Send(const FAgentHttpRequest& Request, FAgentHttpChunk OnChunk, FAgentHttpComplete OnComplete) = 0;
};

namespace CursorAgent
{
	/** The real thing, on the engine's HTTP module. */
	CURSORUNREALAGENT_API TSharedRef<IAgentHttpTransport> MakeEngineHttpTransport();
}

/** A scripted reply for FAgentFixtureTransport. */
struct FAgentFixtureResponse
{
	int32 Status = 200;
	TMap<FString, FString> Headers;
	TArray<uint8> Body;
	/** Deliver a streamed body this many bytes at a time. Zero delivers it in one piece. */
	int32 ChunkBytes = 0;
	/** Pretend the connection failed: no status, no body. */
	bool bConnectionFailure = false;
	/** Pretend the connection dropped after the body was delivered. */
	bool bDropAfterBody = false;
};

/**
 * Replays scripted responses in order, one per request, and records what was asked. Nothing
 * leaves the process. Used by the automation tests and by the panel's offline demo mode.
 */
class CURSORUNREALAGENT_API FAgentFixtureTransport : public IAgentHttpTransport, public TSharedFromThis<FAgentFixtureTransport>
{
public:
	virtual ~FAgentFixtureTransport() override;

	void Enqueue(FAgentFixtureResponse Response);
	void EnqueueText(const FString& Body, int32 Status = 200, int32 ChunkBytes = 0);
	/** Reads the body from a file. Returns false if it cannot be read. */
	bool EnqueueFile(const FString& Path, int32 ChunkBytes = 0, int32 Status = 200);

	/** Replays everything queued so far, over and over, once the queue runs out. For the offline demo. */
	void LoopQueuedResponses() { LoopScript = Queue; }

	/**
	 * Delivers every response that is waiting, including replies to requests made while
	 * delivering. Returns how many requests were completed. For tests, which have no ticking.
	 */
	int32 PumpAll();

	/** Deliver one piece per interval from the engine ticker, so the panel shows text arriving. */
	void StartAutoPump(float SecondsPerChunk);
	void StopAutoPump();

	/** Every request received, oldest first. */
	const TArray<FAgentHttpRequest>& GetRequests() const { return Requests; }
	int32 NumQueuedResponses() const { return Queue.Num(); }

	virtual TSharedRef<IAgentHttpCall> Send(const FAgentHttpRequest& Request, FAgentHttpChunk OnChunk, FAgentHttpComplete OnComplete) override;

private:
	struct FPending;
	/** Delivers one piece of the oldest pending call. Returns true when that call finished. */
	bool Step();
	bool HandleAutoPump(float DeltaSeconds);

	TArray<FAgentFixtureResponse> Queue;
	TArray<FAgentFixtureResponse> LoopScript;
	TArray<FAgentHttpRequest> Requests;
	TArray<TSharedRef<FPending>> Pending;
	FTSTicker::FDelegateHandle AutoPumpHandle;
};
