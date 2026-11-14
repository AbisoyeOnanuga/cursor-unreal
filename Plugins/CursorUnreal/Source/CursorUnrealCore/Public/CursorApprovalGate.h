#pragma once

#include "CoreMinimal.h"
#include "CursorTool.h"
#include "CursorUnrealSettings.h"

enum class ECursorApprovalDecision : uint8
{
	AllowOnce,
	/** Allow this tool until the editor closes. */
	AllowForSession,
	Deny
};

enum class ECursorApprovalVerdict : uint8
{
	Allow,
	Ask,
	Deny
};

/** What the user is shown when a call needs approval. */
struct FCursorApprovalRequest
{
	FString ToolName;
	ECursorToolRisk Risk = ECursorToolRisk::ReadOnly;
	FString Caller;
	/** Redacted, pretty-printed arguments. */
	FString ArgsSummary;
	/** For composite tools: the calls inside that are the reason approval is needed. */
	TArray<FString> SubCalls;
	/** Tool names that "allow for session" would unlock. */
	TArray<FName> ToolsNeedingApproval;
};

DECLARE_DELEGATE_TwoParams(FCursorApprovalHandler, const FCursorApprovalRequest& /*Request*/, TFunction<void(ECursorApprovalDecision)> /*Respond*/);

/**
 * Sits in front of the tool registry. Every caller goes through it: the in-editor agent, the
 * Cursor bridge and MCP clients. Cursor SDK custom tools skip Cursor's own approval step, so on
 * that path this is the only check there is.
 */
class CURSORUNREALCORE_API FCursorApprovalGate
{
public:
	static FCursorApprovalGate& Get();

	/** The editor module installs the UI prompt here. With no handler, anything that needs approval is refused. */
	void SetHandler(FCursorApprovalHandler InHandler);
	void ClearHandler();
	bool HasHandler() const { return Handler.IsBound(); }
	/** A copy of the current handler, so tests can put it back after substituting their own. */
	FCursorApprovalHandler GetHandler() const { return Handler; }

	using FOnDecided = TFunction<void(bool bAllowed, const FString& DenyReason, double ApprovalMs)>;

	/** Decides whether the call may run. OnDecided may fire before this returns, or later if the user is asked. */
	void Evaluate(const ICursorTool& Tool, const TSharedRef<FJsonObject>& Args, const FCursorToolContext& Ctx, FOnDecided OnDecided);

	/** The policy table, with no side effects. */
	static ECursorApprovalVerdict Classify(
		FName ToolName,
		ECursorToolRisk Risk,
		ECursorApprovalPolicy Policy,
		const TArray<FString>& AlwaysAllowed,
		const TSet<FName>& SessionAllowed);

	void ResetSessionAllowances() { SessionAllowed.Reset(); }

private:
	FCursorApprovalHandler Handler;
	TSet<FName> SessionAllowed;
};
