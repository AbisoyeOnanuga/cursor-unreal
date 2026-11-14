#include "CursorMcpServer.h"

#include "CursorActivity.h"
#include "CursorJson.h"
#include "CursorMcpProtocol.h"
#include "CursorSecrets.h"
#include "CursorUnrealLog.h"
#include "CursorUnrealSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HttpPath.h"
#include "HttpServerModule.h"
#include "HttpServerRequest.h"
#include "HttpServerResponse.h"
#include "IHttpRouter.h"
#include "IPAddress.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
	/** Requests larger than this are refused. Tool arguments are small; this is generous. */
	constexpr int32 MaxRequestBodyBytes = 8 * 1024 * 1024;

	const TCHAR* ServerEntryName = TEXT("cursor-unreal");

	FString JoinHeader(const TMap<FString, TArray<FString>>& Headers, const TCHAR* LowercaseName)
	{
		// The engine splits header values on commas; put them back together.
		if (const TArray<FString>* Values = Headers.Find(LowercaseName))
		{
			return FString::Join(*Values, TEXT(",")).TrimStartAndEnd();
		}
		return FString();
	}

	/** "host", "host:port" or "[v6]:port" -> host without brackets, lowercased. */
	FString HostWithoutPort(const FString& HostHeader, FString* OutPort = nullptr)
	{
		FString Host = HostHeader.TrimStartAndEnd().ToLower();
		FString PortText;
		if (Host.StartsWith(TEXT("[")))
		{
			int32 Close = INDEX_NONE;
			if (Host.FindChar(TEXT(']'), Close))
			{
				if (Close + 1 < Host.Len() && Host[Close + 1] == TEXT(':'))
				{
					PortText = Host.Mid(Close + 2);
				}
				Host = Host.Mid(1, Close - 1);
			}
		}
		else
		{
			int32 Colon = INDEX_NONE;
			if (Host.FindLastChar(TEXT(':'), Colon))
			{
				PortText = Host.Mid(Colon + 1);
				Host = Host.Left(Colon);
			}
		}
		if (OutPort)
		{
			*OutPort = PortText;
		}
		return Host;
	}

	bool IsLoopbackHostName(const FString& Host)
	{
		return Host == TEXT("localhost") || Host == TEXT("127.0.0.1") || Host == TEXT("::1");
	}

	TUniquePtr<FHttpServerResponse> JsonResponse(const FString& Body, EHttpServerResponseCodes Code = EHttpServerResponseCodes::Ok)
	{
		TUniquePtr<FHttpServerResponse> Response = FHttpServerResponse::Create(Body, TEXT("application/json"));
		Response->Code = Code;
		return Response;
	}

	TUniquePtr<FHttpServerResponse> PlainError(EHttpServerResponseCodes Code, const FString& Message)
	{
		TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("error"), Message);
		return JsonResponse(CursorJson::ToString(Body), Code);
	}
}

FCursorMcpServer& FCursorMcpServer::Get()
{
	static FCursorMcpServer Instance;
	return Instance;
}

FString FCursorMcpServer::GetUrl() const
{
	// 127.0.0.1 rather than localhost: no name lookup, and no chance of resolving to IPv6 while we listen on IPv4.
	return FString::Printf(TEXT("http://127.0.0.1:%d/mcp"), Port > 0 ? Port : UCursorUnrealSettings::Get()->McpPort);
}

void FCursorMcpServer::SetStatus(const FString& Text)
{
	StatusText = Text;
	OnStatusChanged.Broadcast();
}

FString FCursorMcpServer::ResolveConfiguredBindAddress(int32 InPort)
{
	// Mirrors FHttpServerConfig::GetListenerConfig, which is private to the engine module.
	static const TCHAR* Section = TEXT("HTTPServer.Listeners");
	FString Address = TEXT("localhost");
	if (!GConfig)
	{
		return Address;
	}
	GConfig->GetString(Section, TEXT("DefaultBindAddress"), Address, GEngineIni);

	TArray<FString> Overrides;
	GConfig->GetArray(Section, TEXT("ListenerOverrides"), Overrides, GEngineIni);
	for (FString Override : Overrides)
	{
		Override.TrimStartAndEndInline();
		Override.ReplaceInline(TEXT("("), TEXT(""));
		Override.ReplaceInline(TEXT(")"), TEXT(""));
		uint32 ConfiguredPort = 0;
		if (FParse::Value(*Override, TEXT("Port="), ConfiguredPort) && ConfiguredPort == static_cast<uint32>(InPort))
		{
			FParse::Value(*Override, TEXT("BindAddress="), Address);
			break;
		}
	}
	return Address.TrimStartAndEnd();
}

bool FCursorMcpServer::IsLoopbackBindAddress(const FString& Address)
{
	const FString Lower = Address.TrimStartAndEnd().ToLower();
	return Lower == TEXT("localhost") || Lower == TEXT("127.0.0.1") || Lower == TEXT("::1");
}

int32 FCursorMcpServer::CheckRequestHeaders(const TMap<FString, TArray<FString>>& Headers, int32 InPort, const FString& ExpectedToken, FString& OutReason)
{
	// Host: a page on another site that tricks a browser into resolving its own name to
	// 127.0.0.1 (DNS rebinding) still sends its own name here.
	const FString HostHeader = JoinHeader(Headers, TEXT("host"));
	FString HostPort;
	const FString Host = HostWithoutPort(HostHeader, &HostPort);
	if (HostHeader.IsEmpty() || !IsLoopbackHostName(Host) || (!HostPort.IsEmpty() && FCString::Atoi(*HostPort) != InPort))
	{
		OutReason = TEXT("Host header must be this machine's loopback address.");
		return 403;
	}

	// Origin: only browsers send it. Anything that is not a local page is refused.
	const FString Origin = JoinHeader(Headers, TEXT("origin"));
	if (!Origin.IsEmpty())
	{
		FString Rest = Origin.ToLower();
		const bool bHttp = Rest.RemoveFromStart(TEXT("http://"));
		const bool bHttps = !bHttp && Rest.RemoveFromStart(TEXT("https://"));
		int32 Slash = INDEX_NONE;
		if (Rest.FindChar(TEXT('/'), Slash))
		{
			Rest.LeftInline(Slash);
		}
		if ((!bHttp && !bHttps) || !IsLoopbackHostName(HostWithoutPort(Rest)))
		{
			OutReason = TEXT("Origin is not allowed.");
			return 403;
		}
	}

	// Token. An empty expected token must never match.
	const FString Authorization = JoinHeader(Headers, TEXT("authorization"));
	FString Presented;
	if (Authorization.Len() > 7 && Authorization.Left(7).Equals(TEXT("Bearer "), ESearchCase::IgnoreCase))
	{
		Presented = Authorization.Mid(7).TrimStartAndEnd();
	}
	if (ExpectedToken.IsEmpty() || Presented.IsEmpty() || !CursorSecrets::ConstantTimeEquals(Presented, ExpectedToken))
	{
		OutReason = TEXT("Missing or wrong bearer token.");
		return 401;
	}
	return 0;
}

void FCursorMcpServer::ApplySettings()
{
	const UCursorUnrealSettings* Settings = UCursorUnrealSettings::Get();
	const bool bWant = Settings->bEnableMcpServer;
	if (!bWant)
	{
		Stop();
		return;
	}
	if (IsRunning() && Port == Settings->McpPort)
	{
		return;
	}
	Stop();
	FString Error;
	if (!Start(Error))
	{
		UE_LOG(LogCursorUnreal, Warning, TEXT("MCP server did not start: %s"), *Error);
	}
}

bool FCursorMcpServer::Start(FString& OutError)
{
	check(IsInGameThread());
	if (IsRunning())
	{
		return true;
	}

	const int32 WantedPort = UCursorUnrealSettings::Get()->McpPort;
	if (WantedPort < 1024 || WantedPort > 65535)
	{
		OutError = FString::Printf(TEXT("Port %d is out of range (1024-65535)."), WantedPort);
		SetStatus(TEXT("MCP server is off: ") + OutError);
		return false;
	}

	// The engine's HTTP server takes its bind address from the engine ini. If a project has set
	// that to "any" or a LAN address, starting would expose the editor to the network. Refuse.
	const FString BindAddress = ResolveConfiguredBindAddress(WantedPort);
	if (!IsLoopbackBindAddress(BindAddress))
	{
		OutError = FString::Printf(
			TEXT("The engine HTTP server is configured to bind '%s' for port %d ([HTTPServer.Listeners] in the engine ini). ")
			TEXT("The MCP server only runs on the loopback interface. Change the port or add ")
			TEXT("ListenerOverrides=(Port=%d,BindAddress=localhost)."),
			*BindAddress, WantedPort, WantedPort);
		SetStatus(TEXT("MCP server is off: ") + OutError);
		return false;
	}

	Token = CursorSecrets::GetOrCreateMcpToken();
	if (Token.IsEmpty())
	{
		OutError = TEXT("Could not create an access token.");
		SetStatus(TEXT("MCP server is off: ") + OutError);
		return false;
	}

	FHttpServerModule& HttpServer = FHttpServerModule::Get();
	// Listeners only bind once the module has been told to start them. Other plugins share the
	// module, and starting twice is harmless.
	HttpServer.StartAllListeners();

	Router = HttpServer.GetHttpRouter(static_cast<uint32>(WantedPort), /*bFailOnBindFailure*/ true);
	if (!Router.IsValid())
	{
		OutError = FString::Printf(TEXT("Could not listen on 127.0.0.1:%d. Another program may be using the port; change it in the plugin settings."), WantedPort);
		SetStatus(TEXT("MCP server is off: ") + OutError);
		return false;
	}

	Port = WantedPort;
	const EHttpServerRequestVerbs Verbs = EHttpServerRequestVerbs::VERB_POST | EHttpServerRequestVerbs::VERB_GET
		| EHttpServerRequestVerbs::VERB_DELETE | EHttpServerRequestVerbs::VERB_OPTIONS;
	RouteHandle = Router->BindRoute(FHttpPath(TEXT("/mcp")), Verbs,
		FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleRequest(Request, OnComplete);
		}));
	if (!RouteHandle.IsValid())
	{
		Router.Reset();
		Port = 0;
		OutError = TEXT("Could not register the /mcp route.");
		SetStatus(TEXT("MCP server is off: ") + OutError);
		return false;
	}

	UE_LOG(LogCursorUnreal, Log, TEXT("MCP server listening at %s (loopback only, token required)."), *GetUrl());
	SetStatus(FString::Printf(TEXT("MCP server running at %s"), *GetUrl()));
	return true;
}

void FCursorMcpServer::Stop()
{
	if (RouteHandle.IsValid() && Router.IsValid())
	{
		// The listener itself belongs to the engine module and may be shared; removing our route
		// is enough to make every request to it answer 404.
		Router->UnbindRoute(RouteHandle);
		UE_LOG(LogCursorUnreal, Log, TEXT("MCP server stopped."));
	}
	RouteHandle.Reset();
	Router.Reset();
	Port = 0;
	SetStatus(TEXT("MCP server is off."));
}

bool FCursorMcpServer::HandleRequest(const FHttpServerRequest& Request, const TFunction<void(TUniquePtr<FHttpServerResponse>&&)>& OnComplete)
{
	// Belt and braces: the socket is bound to loopback, so a remote peer should be impossible.
	if (Request.PeerAddress.IsValid())
	{
		const FString Peer = Request.PeerAddress->ToString(false);
		if (Peer != TEXT("127.0.0.1") && Peer != TEXT("::1") && Peer != TEXT("::ffff:127.0.0.1"))
		{
			UE_LOG(LogCursorUnreal, Warning, TEXT("MCP: refused a request from non-loopback address %s."), *Peer);
			OnComplete(PlainError(EHttpServerResponseCodes::Forbidden, TEXT("Only connections from this machine are accepted.")));
			return true;
		}
	}

	FString Reason;
	const int32 Status = CheckRequestHeaders(Request.Headers, Port, Token, Reason);
	if (Status != 0)
	{
		UE_LOG(LogCursorUnreal, Verbose, TEXT("MCP: refused a request (%d): %s"), Status, *Reason);
		TUniquePtr<FHttpServerResponse> Response = PlainError(static_cast<EHttpServerResponseCodes>(Status), Reason);
		if (Status == 401)
		{
			Response->Headers.Add(TEXT("WWW-Authenticate"), TArray<FString>{ FString(TEXT("Bearer")) });
		}
		OnComplete(MoveTemp(Response));
		return true;
	}

	if (Request.Verb != EHttpServerRequestVerbs::VERB_POST)
	{
		// No server-initiated stream and no sessions to delete.
		TUniquePtr<FHttpServerResponse> Response = PlainError(EHttpServerResponseCodes::BadMethod, TEXT("Use POST."));
		Response->Headers.Add(TEXT("Allow"), TArray<FString>{ FString(TEXT("POST")) });
		OnComplete(MoveTemp(Response));
		return true;
	}

	if (Request.Body.Num() > MaxRequestBodyBytes)
	{
		OnComplete(PlainError(EHttpServerResponseCodes::RequestTooLarge, TEXT("Request body is too large.")));
		return true;
	}

	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
	const FString Body = FString(FStringView(Converted.Get(), Converted.Length()));

	FCursorActivity::Get().NoteActivity();

	FCursorMcpProtocol::HandleMessage(Body, [OnComplete](TOptional<FString> ResponseJson)
	{
		if (ResponseJson.IsSet())
		{
			OnComplete(JsonResponse(ResponseJson.GetValue()));
		}
		else
		{
			// Notification: accepted, nothing to say.
			TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
			Response->Code = EHttpServerResponseCodes::Accepted;
			OnComplete(MoveTemp(Response));
		}
	});
	return true;
}

FString FCursorMcpServer::BuildClientConfigJson(bool bIncludeToken) const
{
	TSharedRef<FJsonObject> Headers = MakeShared<FJsonObject>();
	Headers->SetStringField(TEXT("Authorization"),
		bIncludeToken ? FString::Printf(TEXT("Bearer %s"), *CursorSecrets::GetOrCreateMcpToken()) : FString(TEXT("Bearer <token>")));

	TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("url"), GetUrl());
	Entry->SetObjectField(TEXT("headers"), Headers);

	TSharedRef<FJsonObject> Servers = MakeShared<FJsonObject>();
	Servers->SetObjectField(ServerEntryName, Entry);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetObjectField(TEXT("mcpServers"), Servers);
	return CursorJson::ToString(Root, /*bPretty*/ true);
}

FString FCursorMcpServer::GetUserCursorConfigPath()
{
	FString Home = FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE"));
	if (Home.IsEmpty())
	{
		Home = FPlatformMisc::GetEnvironmentVariable(TEXT("HOME"));
	}
	return Home.IsEmpty() ? FString() : FPaths::Combine(Home, TEXT(".cursor"), TEXT("mcp.json"));
}

bool FCursorMcpServer::InstallIntoUserCursorConfig(FString& OutMessage) const
{
	const FString Path = GetUserCursorConfigPath();
	if (Path.IsEmpty())
	{
		OutMessage = TEXT("Could not find your home folder.");
		return false;
	}

	TSharedPtr<FJsonObject> Root;
	if (FPaths::FileExists(Path))
	{
		FString Existing;
		if (!FFileHelper::LoadFileToString(Existing, *Path))
		{
			OutMessage = FString::Printf(TEXT("Could not read %s."), *Path);
			return false;
		}
		if (!Existing.TrimStartAndEnd().IsEmpty())
		{
			Root = CursorJson::Parse(Existing);
			if (!Root.IsValid())
			{
				// Do not replace a file we cannot understand; the user would lose their other servers.
				OutMessage = FString::Printf(TEXT("%s is not valid JSON, so it was left alone. Fix it, or copy the config and paste it in by hand."), *Path);
				return false;
			}
			const FString Backup = Path + TEXT(".bak-") + FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S"));
			if (!FFileHelper::SaveStringToFile(Existing, *Backup, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
			{
				OutMessage = FString::Printf(TEXT("Could not write a backup next to %s, so it was left alone."), *Path);
				return false;
			}
		}
	}
	if (!Root.IsValid())
	{
		Root = MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> Servers;
	const TSharedPtr<FJsonObject>* ServersPtr = nullptr;
	if (Root->TryGetObjectField(TEXT("mcpServers"), ServersPtr) && ServersPtr && ServersPtr->IsValid())
	{
		Servers = *ServersPtr;
	}
	else
	{
		Servers = MakeShared<FJsonObject>();
		Root->SetObjectField(TEXT("mcpServers"), Servers);
	}

	TSharedRef<FJsonObject> Headers = MakeShared<FJsonObject>();
	Headers->SetStringField(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *CursorSecrets::GetOrCreateMcpToken()));
	TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
	Entry->SetStringField(TEXT("url"), GetUrl());
	Entry->SetObjectField(TEXT("headers"), Headers);
	Servers->SetObjectField(ServerEntryName, Entry);

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
	if (!FFileHelper::SaveStringToFile(CursorJson::ToString(Root.ToSharedRef(), true), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutMessage = FString::Printf(TEXT("Could not write %s."), *Path);
		return false;
	}
	OutMessage = FString::Printf(TEXT("Added '%s' to %s. Restart Cursor or reload its MCP servers to pick it up."), ServerEntryName, *Path);
	return true;
}
