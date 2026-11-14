#include "AgentHttpTransport.h"

#include "Async/Async.h"
#include "CursorUnrealCompat.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"

#include <atomic>

namespace
{
	const TCHAR* DescribeFailure(EHttpFailureReason Reason)
	{
		switch (Reason)
		{
		case EHttpFailureReason::ConnectionError: return TEXT("connection error");
		case EHttpFailureReason::Cancelled: return TEXT("cancelled");
		case EHttpFailureReason::TimedOut: return TEXT("timed out");
		default: return TEXT("request failed");
		}
	}

	/**
	 * One request on the engine's HTTP module. Body bytes arrive on the HTTP thread and are
	 * handed to the game thread in order; everything the caller sees happens on the game thread.
	 */
	class FEngineHttpCall final : public IAgentHttpCall, public TSharedFromThis<FEngineHttpCall, ESPMode::ThreadSafe>
	{
	public:
		FEngineHttpCall(FAgentHttpChunk InOnChunk, FAgentHttpComplete InOnComplete)
			: OnChunk(MoveTemp(InOnChunk))
			, OnComplete(MoveTemp(InOnComplete))
		{
		}

		void Start(const FAgentHttpRequest& In)
		{
			const TSharedRef<FEngineHttpCall, ESPMode::ThreadSafe> Self = AsShared();

			Request = FHttpModule::Get().CreateRequest();
			Request->SetVerb(In.Verb);
			Request->SetURL(In.Url);
			for (const TPair<FString, FString>& Header : In.Headers)
			{
				Request->SetHeader(Header.Key, Header.Value);
			}
			if (!In.Body.IsEmpty())
			{
				Request->SetContentAsString(In.Body);
			}
			// No overall limit: a long reply is not a failure. Silence is.
			Request->SetTimeout(0.0f);
			Request->SetActivityTimeout(In.ActivityTimeoutSeconds);

			if (In.bStream)
			{
#if CURSORUNREAL_HAS_HTTP_STREAM_DELEGATE_V2
				Request->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda(
					[Self](void* Ptr, int64& InOutLength)
					{
						Self->ReceiveOnHttpThread(Ptr, InOutLength);
					}));
#else
				Request->SetResponseBodyReceiveStreamDelegate(FHttpRequestStreamDelegate::CreateLambda(
					[Self](void* Ptr, int64 Length)
					{
						int64 InOutLength = Length;
						Self->ReceiveOnHttpThread(Ptr, InOutLength);
						return InOutLength == Length;
					}));
#endif
			}

			Request->OnProcessRequestComplete().BindLambda(
				[Self](FHttpRequestPtr, FHttpResponsePtr Response, bool bProcessedSuccessfully)
				{
					Self->HandleComplete(Response, bProcessedSuccessfully);
				});

			bInsideStart = true;
			const bool bStarted = Request->ProcessRequest();
			bInsideStart = false;

			if (!bStarted && !bFinished)
			{
				FAgentHttpResponse Failed;
				Failed.FailureReason = TEXT("the request could not be started");
				Finish(Failed);
			}
		}

		virtual void Cancel() override
		{
			check(IsInGameThread());
			bCancelled = true;
			OnChunk = nullptr;
			OnComplete = nullptr;
			if (Request.IsValid())
			{
				// Keep the request alive across the call: cancelling runs the completion delegate, which lets go of it.
				const FHttpRequestPtr Keep = Request;
				Keep->CancelRequest();
			}
		}

	private:
		void ReceiveOnHttpThread(void* Ptr, int64& InOutLength)
		{
			if (bCancelled)
			{
				// Zero tells the HTTP module to stop the transfer.
				InOutLength = 0;
				return;
			}
			bool bSchedule = false;
			{
				FScopeLock Lock(&PendingLock);
				PendingBytes.Append(static_cast<const uint8*>(Ptr), static_cast<int32>(InOutLength));
				if (!bDrainScheduled)
				{
					bDrainScheduled = true;
					bSchedule = true;
				}
			}
			if (bSchedule)
			{
				AsyncTask(ENamedThreads::GameThread, [Self = AsShared()]() { Self->Drain(); });
			}
		}

		void Drain()
		{
			check(IsInGameThread());
			TArray<uint8> Bytes;
			{
				FScopeLock Lock(&PendingLock);
				Bytes = MoveTemp(PendingBytes);
				PendingBytes.Reset();
				bDrainScheduled = false;
			}
			if (Bytes.Num() > 0 && !bCancelled && OnChunk)
			{
				// Call a copy: the listener may cancel this request from inside the callback, which clears OnChunk.
				const FAgentHttpChunk Chunk = OnChunk;
				Chunk(Bytes);
			}
		}

		void HandleComplete(FHttpResponsePtr Response, bool bProcessedSuccessfully)
		{
			check(IsInGameThread());

			FAgentHttpResponse Out;
			Out.bCompleted = bProcessedSuccessfully;
			if (Response.IsValid())
			{
				Out.Status = Response->GetResponseCode();
				for (const FString& Header : Response->GetAllHeaders())
				{
					FString Name;
					FString Value;
					if (Header.Split(TEXT(":"), &Name, &Value))
					{
						Out.Headers.Add(Name.TrimStartAndEnd().ToLower(), Value.TrimStartAndEnd());
					}
				}
				Out.Body = Response->GetContentAsString();
			}
			if (!bProcessedSuccessfully && Request.IsValid())
			{
				Out.FailureReason = DescribeFailure(Request->GetFailureReason());
			}

			if (bInsideStart)
			{
				// Failing before ProcessRequest returns would call back from inside Send. Report it next tick instead.
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
					[Self = AsShared(), Out](float)
					{
						Self->Finish(Out);
						return false;
					}));
				return;
			}
			Finish(Out);
		}

		void Finish(const FAgentHttpResponse& Out)
		{
			if (bFinished)
			{
				return;
			}
			bFinished = true;

			// Anything still queued belongs before the completion.
			Drain();

			const FAgentHttpComplete Complete = MoveTemp(OnComplete);
			OnChunk = nullptr;
			OnComplete = nullptr;
			// The request holds delegates that hold this object. Let go so both can be freed.
			Request.Reset();

			if (!bCancelled && Complete)
			{
				Complete(Out);
			}
		}

		FHttpRequestPtr Request;
		FAgentHttpChunk OnChunk;
		FAgentHttpComplete OnComplete;

		FCriticalSection PendingLock;
		TArray<uint8> PendingBytes;
		bool bDrainScheduled = false;

		std::atomic<bool> bCancelled{ false };
		bool bFinished = false;
		bool bInsideStart = false;
	};

	class FEngineHttpTransport final : public IAgentHttpTransport
	{
	public:
		virtual TSharedRef<IAgentHttpCall> Send(const FAgentHttpRequest& Request, FAgentHttpChunk OnChunk, FAgentHttpComplete OnComplete) override
		{
			check(IsInGameThread());
			const TSharedRef<FEngineHttpCall, ESPMode::ThreadSafe> Call = MakeShared<FEngineHttpCall, ESPMode::ThreadSafe>(MoveTemp(OnChunk), MoveTemp(OnComplete));
			Call->Start(Request);
			return StaticCastSharedRef<IAgentHttpCall>(Call);
		}
	};
}

TSharedRef<IAgentHttpTransport> CursorAgent::MakeEngineHttpTransport()
{
	return MakeShared<FEngineHttpTransport>();
}

// --- Fixture transport ---------------------------------------------------------------------------

struct FAgentFixtureTransport::FPending final : public IAgentHttpCall
{
	FAgentFixtureResponse Response;
	FAgentHttpChunk OnChunk;
	FAgentHttpComplete OnComplete;
	bool bStream = false;
	int32 Offset = 0;
	bool bCancelled = false;

	virtual void Cancel() override
	{
		bCancelled = true;
		OnChunk = nullptr;
		OnComplete = nullptr;
	}
};

FAgentFixtureTransport::~FAgentFixtureTransport()
{
	StopAutoPump();
}

void FAgentFixtureTransport::Enqueue(FAgentFixtureResponse Response)
{
	Queue.Add(MoveTemp(Response));
}

void FAgentFixtureTransport::EnqueueText(const FString& Body, int32 Status, int32 ChunkBytes)
{
	FAgentFixtureResponse Response;
	Response.Status = Status;
	Response.ChunkBytes = ChunkBytes;
	const FTCHARToUTF8 Utf8(*Body);
	Response.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Enqueue(MoveTemp(Response));
}

bool FAgentFixtureTransport::EnqueueFile(const FString& Path, int32 ChunkBytes, int32 Status)
{
	FAgentFixtureResponse Response;
	Response.Status = Status;
	Response.ChunkBytes = ChunkBytes;
	if (!FFileHelper::LoadFileToArray(Response.Body, *Path))
	{
		return false;
	}
	Enqueue(MoveTemp(Response));
	return true;
}

TSharedRef<IAgentHttpCall> FAgentFixtureTransport::Send(const FAgentHttpRequest& Request, FAgentHttpChunk OnChunk, FAgentHttpComplete OnComplete)
{
	Requests.Add(Request);

	const TSharedRef<FPending> Call = MakeShared<FPending>();
	Call->OnChunk = MoveTemp(OnChunk);
	Call->OnComplete = MoveTemp(OnComplete);
	Call->bStream = Request.bStream;
	if (Queue.Num() == 0 && LoopScript.Num() > 0)
	{
		Queue = LoopScript;
	}
	if (Queue.Num() > 0)
	{
		Call->Response = MoveTemp(Queue[0]);
		Queue.RemoveAt(0);
	}
	else
	{
		// Nothing scripted for this request. Fail it the way a dead connection would, so a test notices.
		Call->Response.bConnectionFailure = true;
	}
	Pending.Add(Call);
	return Call;
}

bool FAgentFixtureTransport::Step()
{
	if (Pending.Num() == 0)
	{
		return false;
	}
	const TSharedRef<FPending> Call = Pending[0];

	if (Call->bCancelled)
	{
		Pending.RemoveAt(0);
		return true;
	}

	const FAgentFixtureResponse& Response = Call->Response;
	if (!Response.bConnectionFailure && Call->bStream && Call->Offset < Response.Body.Num())
	{
		const int32 Remaining = Response.Body.Num() - Call->Offset;
		const int32 Count = Response.ChunkBytes > 0 ? FMath::Min(Response.ChunkBytes, Remaining) : Remaining;
		const TConstArrayView<uint8> Piece(Response.Body.GetData() + Call->Offset, Count);
		Call->Offset += Count;
		if (Call->OnChunk)
		{
			// Call a copy: the listener may cancel this request from inside the callback, which clears OnChunk.
			const FAgentHttpChunk Chunk = Call->OnChunk;
			Chunk(Piece);
		}
		return false;
	}

	Pending.RemoveAt(0);

	FAgentHttpResponse Out;
	if (Response.bConnectionFailure)
	{
		Out.FailureReason = TEXT("connection error");
	}
	else
	{
		Out.Status = Response.Status;
		Out.bCompleted = !Response.bDropAfterBody;
		if (Response.bDropAfterBody)
		{
			Out.FailureReason = TEXT("connection error");
		}
		for (const TPair<FString, FString>& Header : Response.Headers)
		{
			Out.Headers.Add(Header.Key.ToLower(), Header.Value);
		}
		if (!Call->bStream && Response.Body.Num() > 0)
		{
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Response.Body.GetData()), Response.Body.Num());
			Out.Body = FString(FStringView(Converted.Get(), Converted.Length()));
		}
	}

	const FAgentHttpComplete Complete = MoveTemp(Call->OnComplete);
	Call->OnChunk = nullptr;
	Call->OnComplete = nullptr;
	if (Complete)
	{
		Complete(Out);
	}
	return true;
}

int32 FAgentFixtureTransport::PumpAll()
{
	// Keep this object alive: a callback may drop the last outside reference to it.
	const TSharedRef<FAgentFixtureTransport> Self = AsShared();
	int32 Completed = 0;
	// The limit only guards against a backend that re-sends forever.
	for (int32 Guard = 0; Guard < 1000000 && Pending.Num() > 0; ++Guard)
	{
		if (Step())
		{
			++Completed;
		}
	}
	return Completed;
}

void FAgentFixtureTransport::StartAutoPump(float SecondsPerChunk)
{
	StopAutoPump();
	AutoPumpHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FAgentFixtureTransport::HandleAutoPump), SecondsPerChunk);
}

bool FAgentFixtureTransport::HandleAutoPump(float)
{
	const TSharedRef<FAgentFixtureTransport> Self = AsShared();
	Step();
	return true;
}

void FAgentFixtureTransport::StopAutoPump()
{
	if (AutoPumpHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(AutoPumpHandle);
		AutoPumpHandle.Reset();
	}
}
