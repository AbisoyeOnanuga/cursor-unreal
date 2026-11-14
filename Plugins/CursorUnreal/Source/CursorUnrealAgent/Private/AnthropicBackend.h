#pragma once

#include "CoreMinimal.h"
#include "AgentBackend.h"
#include "AgentHttpTransport.h"
#include "AnthropicStream.h"

/** Everything the backend takes from outside, so tests can replace each piece. */
struct FAnthropicBackendOptions
{
	/** Defaults to the engine HTTP transport. */
	TSharedPtr<IAgentHttpTransport> Transport;
	/** Returns the API key. Defaults to the OS credential store, then ANTHROPIC_API_KEY. */
	TFunction<FString()> KeyProvider;
	/** Runs a callback after a delay. Defaults to the engine ticker. */
	TFunction<void(float Seconds, TFunction<void()> Callback)> Scheduler;
	/** Keep the fetched model list on disk between editor runs. Tests switch this off. */
	bool bPersistModels = true;
	/** How many times a request that failed for a passing reason is sent again. */
	int32 MaxRetries = 2;

	// The three below are for the offline demo, which talks to scripted replies instead of the API.
	/** Use this model id instead of the one in the settings, and do not write the settings. */
	FString ModelOverride;
	/** Offer these models instead of asking the API for its list. */
	TArray<FAgentModelInfo> FixedModels;
	/** Shown in the chat panel. */
	FString Banner;
};

/**
 * Talks to the Anthropic Messages API directly from the editor and runs the tool loop in
 * process: the model asks for a tool, FCursorToolRegistry runs it on the game thread, the result
 * goes back, until the model stops asking. No bridge process and no IPC.
 */
class FAnthropicBackend final : public IAgentBackend, public TSharedFromThis<FAnthropicBackend>
{
public:
	explicit FAnthropicBackend(FAnthropicBackendOptions InOptions = FAnthropicBackendOptions());
	virtual ~FAnthropicBackend() override;

	virtual FString GetProviderId() const override { return TEXT("anthropic"); }
	virtual FString GetModelId() const override;
	virtual void SetModelId(const FString& ModelId) override;
	virtual FString GetSessionId() const override { return SessionId; }
	virtual FString GetBanner() const override { return Options.Banner; }
	virtual bool CheckReady(FText& OutWhyNot) const override;
	virtual bool IsBusy() const override { return Phase != EPhase::Idle; }
	virtual void SendTurn(const FAgentTurnRequest& Request, FAgentEventSink Sink) override;
	virtual bool CanRetry() const override;
	virtual void Retry(FAgentEventSink Sink) override;
	virtual void Cancel() override;
	virtual void ResetConversation() override;
	virtual TArray<FAgentModelInfo> GetCachedModels() const override { return Models; }
	virtual void RefreshModels(FAgentModelsCallback Callback) override;

	/** Number of messages in the conversation as it would be sent. For tests. */
	int32 NumHistoryMessages() const { return History.Num(); }
	/** Number of pictures in the conversation as it would be sent. For tests. */
	int32 NumHistoryImages() const;
	/** Reads a model list in the shape GET /v1/models returns. */
	static bool ParseModelList(const FString& Json, TArray<FAgentModelInfo>& OutModels, FString& OutNextCursor);

private:
	enum class EPhase : uint8
	{
		Idle,
		Requesting,
		RunningTools,
		WaitingToRetry
	};

	const FAgentModelInfo* FindModel(const FString& Id) const;
	FString ApiUrl(const TCHAR* Path) const;
	TMap<FString, FString> AuthHeaders() const;

	void BeginTurn(FAgentEventSink InSink);
	void StartConversationIfNeeded();
	void AppendUserContent(const TArray<TSharedPtr<FJsonValue>>& Blocks);
	FString BuildRequestBody(TArray<FString>& OutNotices) const;

	void StartRequest();
	void HandleResponse(const FAgentHttpResponse& Response);
	void CommitReply();
	void RetryOrFail(const FString& Reason, float RetryAfterSeconds);

	void RunTools();
	void RunNextTool();
	void HandleToolResult(int32 Index, bool bIsError, const FString& Text, const TArray<FCursorImage>& Images = TArray<FCursorImage>());
	/** Drops the oldest pictures from the conversation once it holds too many. */
	void PruneImages();
	void FinishTools();

	void Emit(const FAgentEvent& Event);
	/** Ends the turn and delivers its final event. */
	void EmitLast(const FAgentEvent& Event);
	void EmitNotice(const FString& Text);
	void EmitUsage();
	void Finish(const TCHAR* Outcome);
	void Fail(const FString& Message);

	static FString ModelCacheFile();
	void LoadModelCache();
	void SaveModelCache() const;
	void FetchModelPage(const FString& AfterId, TSharedRef<TArray<FAgentModelInfo>> Accumulated, FAgentModelsCallback Callback, int32 Page);

	FAnthropicBackendOptions Options;
	TArray<FAgentModelInfo> Models;
	FString SessionId;

	// --- Conversation ------------------------------------------------------------------------
	/** Message objects exactly as they are sent. */
	TArray<TSharedPtr<FJsonValue>> History;
	/** Fixed for the life of a conversation: changing either would invalidate the prompt cache and the model's earlier thinking. */
	FString SystemPrompt;
	TArray<TSharedPtr<FJsonValue>> ToolsJson;
	FString ToolsFingerprint;
	bool bConversationStarted = false;
	bool bWarnedToolsChanged = false;
	bool bWarnedEffort = false;

	// --- Turn in progress --------------------------------------------------------------------
	EPhase Phase = EPhase::Idle;
	/** Shared so that a listener which ends the turn from inside an event is not destroyed while it is still running. */
	TSharedPtr<FAgentEventSink> Sink;
	/** Bumped whenever a turn ends or is abandoned, so late callbacks from it can tell they are stale. */
	uint32 Generation = 0;
	int32 Iteration = 0;
	int32 Attempt = 0;
	bool bCancelRequested = false;
	FAgentUsage TurnUsage;
	/** Pictures tools have returned to the model this turn. */
	int32 ToolImagesThisTurn = 0;

	TSharedPtr<IAgentHttpCall> ActiveCall;
	FAnthropicStreamAssembler Assembler;
	/** Start of the raw response body, kept because an error reply is plain JSON rather than an event stream. */
	TArray<uint8> RawHead;

	TArray<FAnthropicToolUse> PendingTools;
	TArray<TSharedPtr<FJsonValue>> ToolResults;
	int32 NextTool = 0;
	/** Set when the reply that asked for tools was cut off, so the turn ends once their results are recorded. */
	const TCHAR* OutcomeAfterTools = nullptr;

	TSharedPtr<IAgentHttpCall> ModelsCall;
};
