#include "CursorMcpProtocol.h"

#include "CursorJson.h"
#include "CursorSkillLibrary.h"
#include "CursorTool.h"
#include "CursorToolRegistry.h"
#include "HAL/PlatformTime.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"

const TCHAR* FCursorMcpProtocol::LatestProtocolVersion = TEXT("2025-06-18");
const TCHAR* FCursorMcpProtocol::MetaSentAtKey = TEXT("cursorunreal/sentAtMs");
const TCHAR* FCursorMcpProtocol::MetaServerMsKey = TEXT("cursorunreal/serverMs");

const TArray<FString>& FCursorMcpProtocol::SupportedProtocolVersions()
{
	static const TArray<FString> Versions = { TEXT("2025-06-18"), TEXT("2025-03-26"), TEXT("2024-11-05") };
	return Versions;
}

namespace
{
	// JSON-RPC 2.0 error codes.
	constexpr int32 ParseError = -32700;
	constexpr int32 InvalidRequest = -32600;
	constexpr int32 MethodNotFound = -32601;
	constexpr int32 InvalidParams = -32602;

	FString RpcResult(const TSharedPtr<FJsonValue>& Id, const TSharedRef<FJsonObject>& Result)
	{
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Message->SetField(TEXT("id"), Id.IsValid() ? Id : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
		Message->SetObjectField(TEXT("result"), Result);
		return CursorJson::ToString(Message);
	}

	FString RpcError(const TSharedPtr<FJsonValue>& Id, int32 Code, const FString& Text)
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetNumberField(TEXT("code"), Code);
		Error->SetStringField(TEXT("message"), Text);

		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("jsonrpc"), TEXT("2.0"));
		Message->SetField(TEXT("id"), Id.IsValid() ? Id : TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>()));
		Message->SetObjectField(TEXT("error"), Error);
		return CursorJson::ToString(Message);
	}

	FString PluginVersion()
	{
		if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CursorUnreal")))
		{
			return Plugin->GetDescriptor().VersionName;
		}
		return TEXT("0.0.0");
	}

	TSharedRef<FJsonObject> BuildInitializeResult(const TSharedPtr<FJsonObject>& Params)
	{
		FString Requested;
		if (Params.IsValid())
		{
			Params->TryGetStringField(TEXT("protocolVersion"), Requested);
		}
		const FString Version = FCursorMcpProtocol::SupportedProtocolVersions().Contains(Requested)
			? Requested
			: FString(FCursorMcpProtocol::LatestProtocolVersion);

		TSharedRef<FJsonObject> ToolsCapability = MakeShared<FJsonObject>();
		ToolsCapability->SetBoolField(TEXT("listChanged"), false);
		TSharedRef<FJsonObject> Capabilities = MakeShared<FJsonObject>();
		Capabilities->SetObjectField(TEXT("tools"), ToolsCapability);

		TSharedRef<FJsonObject> ServerInfo = MakeShared<FJsonObject>();
		ServerInfo->SetStringField(TEXT("name"), TEXT("cursor-unreal"));
		ServerInfo->SetStringField(TEXT("title"), TEXT("Cursor Unreal"));
		ServerInfo->SetStringField(TEXT("version"), PluginVersion());

		FString Instructions = FString::Printf(
			TEXT("These tools act on the running Unreal Editor (project '%s'). Changes go into the editor's undo history; tools marked dangerous ")
			TEXT("ask the user for approval in the editor first, so a call can take as long as the user takes to answer. Asset paths look like ")
			TEXT("/Game/Folder/Name. Use the batch tool to run several known steps in one request. Edit C++ source with your own file tools, ")
			TEXT("then use cpp_build to compile and read the errors.\n"),
			FApp::GetProjectName());
		const FString SkillIndex = FCursorSkillLibrary::Get().BuildIndexForPrompt();
		if (!SkillIndex.IsEmpty())
		{
			Instructions += TEXT("\n") + SkillIndex;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("protocolVersion"), Version);
		Result->SetObjectField(TEXT("capabilities"), Capabilities);
		Result->SetObjectField(TEXT("serverInfo"), ServerInfo);
		Result->SetStringField(TEXT("instructions"), Instructions);
		return Result;
	}

	TSharedRef<FJsonObject> BuildToolsList()
	{
		TArray<TSharedPtr<FJsonValue>> Tools;
		for (const TSharedRef<ICursorTool>& Tool : FCursorToolRegistry::Get().GetTools(FCursorToolRegistry::CallerMcp))
		{
			const ECursorToolRisk Risk = Tool->GetRisk();
			TSharedRef<FJsonObject> Annotations = MakeShared<FJsonObject>();
			Annotations->SetBoolField(TEXT("readOnlyHint"), Risk == ECursorToolRisk::ReadOnly);
			Annotations->SetBoolField(TEXT("destructiveHint"), Risk == ECursorToolRisk::Dangerous);
			Annotations->SetBoolField(TEXT("openWorldHint"), false);

			TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("name"), Tool->GetName().ToString());
			Entry->SetStringField(TEXT("description"), Tool->GetDescription().ToString());
			Entry->SetObjectField(TEXT("inputSchema"), Tool->GetInputSchema());
			Entry->SetObjectField(TEXT("annotations"), Annotations);
			Tools.Add(MakeShared<FJsonValueObject>(Entry));
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("tools"), Tools);
		return Result;
	}
}

void FCursorMcpProtocol::HandleMessage(const FString& RequestJson, FOnResponse OnResponse)
{
	check(IsInGameThread());

	const TSharedPtr<FJsonValue> Parsed = CursorJson::ParseValue(RequestJson);
	if (!Parsed.IsValid())
	{
		OnResponse(RpcError(nullptr, ParseError, TEXT("Request body is not valid JSON.")));
		return;
	}
	if (Parsed->Type == EJson::Array)
	{
		OnResponse(RpcError(nullptr, InvalidRequest, TEXT("JSON-RPC batching is not supported. Send one message per request.")));
		return;
	}
	const TSharedPtr<FJsonObject>* MessagePtr = nullptr;
	if (!Parsed->TryGetObject(MessagePtr) || !MessagePtr || !MessagePtr->IsValid())
	{
		OnResponse(RpcError(nullptr, InvalidRequest, TEXT("Request must be a JSON object.")));
		return;
	}
	const TSharedPtr<FJsonObject> Message = *MessagePtr;

	const TSharedPtr<FJsonValue> Id = Message->TryGetField(TEXT("id"));
	const bool bIsNotification = !Id.IsValid() || Id->IsNull();

	FString Method;
	if (!Message->TryGetStringField(TEXT("method"), Method))
	{
		// A response from the client to something we never asked, or malformed input.
		if (bIsNotification || Message->HasField(TEXT("result")) || Message->HasField(TEXT("error")))
		{
			OnResponse(TOptional<FString>());
		}
		else
		{
			OnResponse(RpcError(Id, InvalidRequest, TEXT("Missing 'method'.")));
		}
		return;
	}

	if (bIsNotification)
	{
		// notifications/initialized, notifications/cancelled and friends: nothing to answer.
		OnResponse(TOptional<FString>());
		return;
	}

	TSharedPtr<FJsonObject> Params;
	{
		const TSharedPtr<FJsonObject>* ParamsPtr = nullptr;
		if (Message->TryGetObjectField(TEXT("params"), ParamsPtr) && ParamsPtr)
		{
			Params = *ParamsPtr;
		}
	}

	if (Method == TEXT("initialize"))
	{
		OnResponse(RpcResult(Id, BuildInitializeResult(Params)));
		return;
	}
	if (Method == TEXT("ping"))
	{
		OnResponse(RpcResult(Id, MakeShared<FJsonObject>()));
		return;
	}
	if (Method == TEXT("tools/list"))
	{
		OnResponse(RpcResult(Id, BuildToolsList()));
		return;
	}
	if (Method == TEXT("tools/call"))
	{
		FString ToolName;
		if (!Params.IsValid() || !Params->TryGetStringField(TEXT("name"), ToolName) || ToolName.IsEmpty())
		{
			OnResponse(RpcError(Id, InvalidParams, TEXT("tools/call needs params.name.")));
			return;
		}

		TSharedPtr<FJsonObject> Arguments;
		const TSharedPtr<FJsonObject>* ArgumentsPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("arguments"), ArgumentsPtr) && ArgumentsPtr)
		{
			Arguments = *ArgumentsPtr;
		}

		FCursorToolContext Ctx;
		Ctx.Caller = FCursorToolRegistry::CallerMcp;
		const TSharedPtr<FJsonObject>* MetaPtr = nullptr;
		if (Params->TryGetObjectField(TEXT("_meta"), MetaPtr) && MetaPtr)
		{
			(*MetaPtr)->TryGetNumberField(MetaSentAtKey, Ctx.ClientSentUnixMs);
		}

		const double StartedAt = FPlatformTime::Seconds();
		FCursorToolRegistry::Get().Invoke(FName(*ToolName), Arguments, Ctx,
			[Id, StartedAt, OnResponse = MoveTemp(OnResponse)](const FCursorToolResult& ToolResult)
			{
				TSharedRef<FJsonObject> TextBlock = MakeShared<FJsonObject>();
				TextBlock->SetStringField(TEXT("type"), TEXT("text"));
				TextBlock->SetStringField(TEXT("text"), ToolResult.Text);

				TSharedRef<FJsonObject> Meta = MakeShared<FJsonObject>();
				Meta->SetNumberField(MetaServerMsKey, (FPlatformTime::Seconds() - StartedAt) * 1000.0);

				// Tool failures are reported inside a successful JSON-RPC result, so the model can
				// read the message and try something else. Protocol errors are for malformed requests.
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Content;
				Content.Add(MakeShared<FJsonValueObject>(TextBlock));
				Result->SetArrayField(TEXT("content"), Content);
				Result->SetBoolField(TEXT("isError"), ToolResult.bIsError);
				if (ToolResult.Structured.IsValid() && !ToolResult.bIsError)
				{
					Result->SetObjectField(TEXT("structuredContent"), ToolResult.Structured);
				}
				Result->SetObjectField(TEXT("_meta"), Meta);
				OnResponse(RpcResult(Id, Result));
			});
		return;
	}

	OnResponse(RpcError(Id, MethodNotFound, FString::Printf(TEXT("Method '%s' is not supported by this server."), *Method)));
}
