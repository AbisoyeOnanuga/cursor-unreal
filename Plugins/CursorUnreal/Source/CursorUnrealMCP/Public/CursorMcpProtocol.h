#pragma once

#include "CoreMinimal.h"

/**
 * The Model Context Protocol, minus the transport. Takes one JSON-RPC message and produces the
 * reply. Kept separate from the HTTP server so it can be tested without sockets.
 */
class CURSORUNREALMCP_API FCursorMcpProtocol
{
public:
	/** Unset for notifications, which get no reply. */
	using FOnResponse = TFunction<void(TOptional<FString> ResponseJson)>;

	/** Must be called on the game thread. OnResponse fires exactly once, possibly after this returns. */
	static void HandleMessage(const FString& RequestJson, FOnResponse OnResponse);

	/** Newest protocol revision this server speaks. Older clients are answered in their own revision. */
	static const TCHAR* LatestProtocolVersion;
	static const TArray<FString>& SupportedProtocolVersions();

	/** Key under params._meta where a client may put its send time (Unix ms) so transport time can be measured. */
	static const TCHAR* MetaSentAtKey;
	/** Key under result._meta holding the milliseconds the editor spent on the call. */
	static const TCHAR* MetaServerMsKey;
};
