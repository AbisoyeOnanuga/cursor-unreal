#pragma once

#include "CoreMinimal.h"
#include "HttpRouteHandle.h"

class IHttpRouter;
struct FHttpServerRequest;

/**
 * Serves the tool registry to MCP clients (Cursor IDE in particular) over HTTP on this machine.
 *
 * It only ever listens on the loopback interface, requires a bearer token on every request, and
 * rejects requests whose Host or Origin headers show they came through a browser pointed at some
 * other site. The in-editor agent and the Cursor bridge do not use it.
 */
class CURSORUNREALMCP_API FCursorMcpServer
{
public:
	static FCursorMcpServer& Get();

	/** Starts or stops the server to match the current settings. Safe to call repeatedly. */
	void ApplySettings();
	bool Start(FString& OutError);
	void Stop();

	bool IsRunning() const { return RouteHandle.IsValid(); }
	int32 GetPort() const { return Port; }
	FString GetUrl() const;
	/** One line for the UI: running and where, or why not. */
	FString GetStatusText() const { return StatusText; }

	/** The snippet to put in a Cursor mcp.json. Leave the token out for anything that will be shown or shared. */
	FString BuildClientConfigJson(bool bIncludeToken) const;

	/**
	 * Adds or updates the "cursor-unreal" entry in ~/.cursor/mcp.json, keeping everything else in
	 * the file. The file lives in the user's home folder, not the project, so the token is not
	 * committed to source control.
	 */
	bool InstallIntoUserCursorConfig(FString& OutMessage) const;
	static FString GetUserCursorConfigPath();

	/**
	 * The header checks, separated out for testing.
	 * @return 0 when the request may proceed, otherwise the HTTP status to answer with.
	 */
	static int32 CheckRequestHeaders(const TMap<FString, TArray<FString>>& Headers, int32 InPort, const FString& ExpectedToken, FString& OutReason);

	/** The address the engine's HTTP server would bind for this port, from the engine ini. */
	static FString ResolveConfiguredBindAddress(int32 InPort);
	static bool IsLoopbackBindAddress(const FString& Address);

	DECLARE_MULTICAST_DELEGATE(FOnStatusChanged);
	FOnStatusChanged OnStatusChanged;

private:
	bool HandleRequest(const FHttpServerRequest& Request, const TFunction<void(TUniquePtr<struct FHttpServerResponse>&&)>& OnComplete);
	void SetStatus(const FString& Text);

	TSharedPtr<IHttpRouter> Router;
	FHttpRouteHandle RouteHandle;
	int32 Port = 0;
	FString Token;
	FString StatusText = TEXT("MCP server is off.");
};
