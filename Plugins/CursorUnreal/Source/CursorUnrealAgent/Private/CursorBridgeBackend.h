#pragma once

#include "CoreMinimal.h"
#include "AgentBackend.h"
#include "CursorBridgeProcess.h"

/** Everything the backend takes from outside, so tests can replace each piece. */
struct FCursorBridgeBackendOptions
{
	/** Returns the Cursor API key. Defaults to the OS credential store, then CURSOR_API_KEY. */
	TFunction<FString()> KeyProvider;
	/** Absolute path of a stand-in for @cursor/sdk. Empty in production. Tests point this at fake-sdk.mjs. */
	FString SdkModuleOverride;
	/** Skip the install probe when the override above is set. */
	bool bSkipInstallProbe = false;
	/** Keep the fetched model list under Saved/ between editor runs. Tests turn it off so they do not replace the real one. */
	bool bPersistModels = true;
};

/**
 * Runs a Cursor agent through the Node bridge. The bridge stays warm between turns; the Cursor API
 * key is sent in the first stdin message and never on the command line.
 */
class FCursorBridgeBackend final : public IAgentBackend, public TSharedFromThis<FCursorBridgeBackend>
{
public:
	explicit FCursorBridgeBackend(FCursorBridgeBackendOptions InOptions = FCursorBridgeBackendOptions());
	virtual ~FCursorBridgeBackend() override;

	virtual FString GetProviderId() const override { return TEXT("cursor"); }
	virtual FString GetModelId() const override;
	virtual void SetModelId(const FString& ModelId) override;
	virtual FString GetSessionId() const override { return SessionId; }
	virtual bool CheckReady(FText& OutWhyNot) const override;
	virtual bool IsBusy() const override { return Phase != EPhase::Idle; }
	virtual void SendTurn(const FAgentTurnRequest& Request, FAgentEventSink Sink) override;
	virtual bool CanRetry() const override;
	virtual void Retry(FAgentEventSink Sink) override;
	virtual void Cancel() override;
	virtual void ResetConversation() override;
	virtual TArray<FAgentModelInfo> GetCachedModels() const override { return Models; }
	virtual void RefreshModels(FAgentModelsCallback Callback) override;

	/** Last install / process status line, for the Setup tab. */
	FString GetBridgeStatusLine() const { return StatusLine; }

private:
	enum class EPhase : uint8
	{
		Idle,
		Starting,
		Requesting,
		RunningTools
	};

	bool EnsureProcess(FString& OutError);
	bool SendInit(FString& OutError);
	void HandleLine(const FString& Line);
	void HandleMessage(const TSharedPtr<FJsonObject>& Message);
	void HandleProcessExit(int32 ExitCode);

	void BeginTurn(FAgentEventSink InSink);
	void Emit(const FAgentEvent& Event);
	void EmitLast(const FAgentEvent& Event);
	void EmitNotice(const FString& Text);
	void Finish(const TCHAR* Outcome);
	void Fail(const FString& Message);

	void RunEditorTool(const FString& CallId, const FString& Name, const TSharedPtr<FJsonObject>& Args, double SentAtMs);
	TArray<TSharedPtr<FJsonValue>> DescribeTools() const;
	FString BuildUserText(const FAgentTurnRequest& Request) const;
	/** The "send" message for the turn that is starting, pictures included. */
	TSharedRef<FJsonObject> BuildSendMessage(const FAgentTurnRequest& Request) const;
	FString NextTurnId();
	FString NextModelsId();

	/** True when the bridge was set up with a different key from the one stored now. */
	bool KeyChangedSinceInit() const;
	bool SendListModels();
	/** Answers everyone waiting for the model list. */
	void FinishModels(bool bOk, const FString& Error);

	FCursorBridgeBackendOptions Options;
	TUniquePtr<FCursorBridgeProcess> Process;
	FString SessionId;
	FString StatusLine;
	TArray<FAgentModelInfo> Models;

	bool bHelloSeen = false;
	bool bReady = false;
	bool bInitInFlight = false;
	FString InitFingerprint;
	/** Digest of the key the bridge was last set up with. Never the key itself. */
	FString InitKeyDigest;

	EPhase Phase = EPhase::Idle;
	TSharedPtr<FAgentEventSink> Sink;
	uint32 Generation = 0;
	FString ActiveTurnId;
	FString LastUserText;
	TArray<FAgentContextBlock> LastContext;
	TArray<FCursorImage> LastImages;
	/** Pictures tools have returned to the model this turn. */
	int32 ToolImagesThisTurn = 0;
	bool bCanRetry = false;
	bool bCancelRequested = false;
	FAgentUsage TurnUsage;

	/** The list request the bridge is working on. Answers carrying another id are from before a key change and are dropped. */
	FString ModelsRequestId;
	/** Everyone waiting for the model list. One request to the bridge answers them all. */
	TArray<FAgentModelsCallback> ModelsCallbacks;

	/** Tool calls the bridge asked for that have not been answered yet. */
	TSet<FString> PendingToolCalls;
};
