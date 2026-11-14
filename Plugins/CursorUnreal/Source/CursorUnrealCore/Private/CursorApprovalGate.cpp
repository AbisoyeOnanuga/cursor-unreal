#include "CursorApprovalGate.h"

#include "CursorJson.h"
#include "CursorSecrets.h"
#include "CursorToolRegistry.h"
#include "HAL/PlatformTime.h"

FCursorApprovalGate& FCursorApprovalGate::Get()
{
	static FCursorApprovalGate Instance;
	return Instance;
}

void FCursorApprovalGate::SetHandler(FCursorApprovalHandler InHandler)
{
	Handler = MoveTemp(InHandler);
}

void FCursorApprovalGate::ClearHandler()
{
	Handler.Unbind();
}

ECursorApprovalVerdict FCursorApprovalGate::Classify(
	FName ToolName,
	ECursorToolRisk Risk,
	ECursorApprovalPolicy Policy,
	const TArray<FString>& AlwaysAllowed,
	const TSet<FName>& SessionAllowed)
{
	if (Risk == ECursorToolRisk::ReadOnly)
	{
		return ECursorApprovalVerdict::Allow;
	}
	// Read-only is a hard stop: allowlists do not override it.
	if (Policy == ECursorApprovalPolicy::ReadOnly)
	{
		return ECursorApprovalVerdict::Deny;
	}
	if (Policy == ECursorApprovalPolicy::AllowAll)
	{
		return ECursorApprovalVerdict::Allow;
	}
	if (SessionAllowed.Contains(ToolName) || AlwaysAllowed.Contains(ToolName.ToString()))
	{
		return ECursorApprovalVerdict::Allow;
	}
	if (Policy == ECursorApprovalPolicy::AskForChanges)
	{
		return ECursorApprovalVerdict::Ask;
	}
	// AskForDangerous
	return Risk == ECursorToolRisk::Dangerous ? ECursorApprovalVerdict::Ask : ECursorApprovalVerdict::Allow;
}

void FCursorApprovalGate::Evaluate(const ICursorTool& Tool, const TSharedRef<FJsonObject>& Args, const FCursorToolContext& Ctx, FOnDecided OnDecided)
{
	const UCursorUnrealSettings* Settings = UCursorUnrealSettings::Get();
	const ECursorApprovalPolicy Policy = Settings->ApprovalPolicy;

	FCursorApprovalRequest Request;
	Request.ToolName = Tool.GetName().ToString();
	Request.Risk = Tool.GetRisk();
	Request.Caller = Ctx.Caller;

	ECursorApprovalVerdict Verdict = ECursorApprovalVerdict::Allow;
	FString DenyReason;

	auto Consider = [&](FName Name, ECursorToolRisk Risk, const FString& Label)
	{
		const ECursorApprovalVerdict One = Classify(Name, Risk, Policy, Settings->AlwaysAllowedTools, SessionAllowed);
		if (One == ECursorApprovalVerdict::Deny)
		{
			Verdict = ECursorApprovalVerdict::Deny;
			DenyReason = FString::Printf(
				TEXT("'%s' is a %s tool and the approval policy is Read-only. Change the policy in Project Settings > Plugins > Cursor Unreal to allow it."),
				*Name.ToString(), LexToString(Risk));
		}
		else if (One == ECursorApprovalVerdict::Ask && Verdict != ECursorApprovalVerdict::Deny)
		{
			Verdict = ECursorApprovalVerdict::Ask;
			Request.ToolsNeedingApproval.AddUnique(Name);
			if (!Label.IsEmpty())
			{
				Request.SubCalls.Add(Label);
			}
			Request.Risk = FMath::Max(Request.Risk, Risk);
		}
	};

	TArray<FCursorSubCall> SubCalls;
	Tool.GetSubCalls(Args, SubCalls);
	if (SubCalls.Num() > 0)
	{
		// A composite tool is judged by what it contains, not by its own label.
		for (const FCursorSubCall& Sub : SubCalls)
		{
			const TSharedPtr<ICursorTool> SubTool = FCursorToolRegistry::Get().Find(Sub.ToolName);
			if (!SubTool.IsValid())
			{
				continue; // Execution reports unknown tools.
			}
			const FString SubArgs = Sub.Args.IsValid() ? CursorJson::ToString(Sub.Args.ToSharedRef()) : TEXT("{}");
			const FString Label = FString::Printf(TEXT("%s %s"), *Sub.ToolName.ToString(), *CursorJson::Truncate(CursorRedactor::Redact(SubArgs), 300));
			Consider(Sub.ToolName, SubTool->GetRisk(), Label);
		}
	}
	else
	{
		Consider(Tool.GetName(), Tool.GetRisk(), FString());
	}

	if (Verdict == ECursorApprovalVerdict::Allow)
	{
		OnDecided(true, FString(), 0.0);
		return;
	}
	if (Verdict == ECursorApprovalVerdict::Deny)
	{
		OnDecided(false, DenyReason, 0.0);
		return;
	}

	if (!Handler.IsBound())
	{
		OnDecided(false,
			FString::Printf(TEXT("'%s' needs approval, but no approval prompt is available (the editor is running without UI). ")
				TEXT("Add it to Always Allowed Tools in Project Settings > Plugins > Cursor Unreal to permit it."), *Request.ToolName),
			0.0);
		return;
	}

	Request.ArgsSummary = CursorJson::Truncate(CursorRedactor::Redact(CursorJson::ToString(Args, true)), 4000);

	const double AskedAt = FPlatformTime::Seconds();
	const TArray<FName> ToUnlock = Request.ToolsNeedingApproval;
	const FString ToolName = Request.ToolName;
	Handler.Execute(Request, [this, AskedAt, ToUnlock, ToolName, OnDecided = MoveTemp(OnDecided)](ECursorApprovalDecision Decision)
	{
		const double WaitedMs = (FPlatformTime::Seconds() - AskedAt) * 1000.0;
		if (Decision == ECursorApprovalDecision::Deny)
		{
			OnDecided(false, FString::Printf(TEXT("The user declined to run '%s'. Do not retry it; ask what they would like instead."), *ToolName), WaitedMs);
			return;
		}
		if (Decision == ECursorApprovalDecision::AllowForSession)
		{
			for (const FName Name : ToUnlock)
			{
				SessionAllowed.Add(Name);
			}
		}
		OnDecided(true, FString(), WaitedMs);
	});
}
