#pragma once

#include "CoreMinimal.h"

/** Where a secret was found. */
enum class ECursorSecretSource : uint8
{
	None,
	/** The OS credential store (Windows Credential Manager). */
	Store,
	/** An environment variable. Read-only from the plugin's point of view. */
	Environment
};

/** OS-backed storage for API keys and tokens. Secrets never go into ini files. */
class ISecretStore
{
public:
	virtual ~ISecretStore() = default;

	/** False when this platform has no backing store, in which case only environment variables work. */
	virtual bool IsAvailable() const = 0;
	virtual bool Get(const FString& Name, FString& OutValue) const = 0;
	virtual bool Set(const FString& Name, const FString& Value) = 0;
	virtual bool Delete(const FString& Name) = 0;
};

namespace CursorSecrets
{
	inline const TCHAR* AnthropicApiKey = TEXT("AnthropicApiKey");
	inline const TCHAR* CursorApiKey = TEXT("CursorApiKey");
	inline const TCHAR* McpToken = TEXT("McpToken");

	CURSORUNREALCORE_API ISecretStore& GetStore();

	/**
	 * Looks in the credential store, then in the named environment variable. Whatever is found is
	 * registered with the redactor so it cannot appear in logs or transcripts.
	 */
	CURSORUNREALCORE_API FString Resolve(const FString& Name, const TCHAR* EnvVar, ECursorSecretSource* OutSource = nullptr);

	CURSORUNREALCORE_API FString ResolveAnthropicKey(ECursorSecretSource* OutSource = nullptr);
	CURSORUNREALCORE_API FString ResolveCursorKey(ECursorSecretSource* OutSource = nullptr);

	/**
	 * A new 256-bit token as 64 hex characters, from the operating system's cryptographic random
	 * generator. Empty if that generator is unavailable; never falls back to a weaker source.
	 * Does not store anything.
	 */
	CURSORUNREALCORE_API FString GenerateToken();

	/**
	 * The bearer token MCP clients must present. Created on first use and kept in the credential store.
	 * Empty if no token could be created, in which case the MCP server must not start.
	 */
	CURSORUNREALCORE_API FString GetOrCreateMcpToken();
	/** Replaces the MCP token. Existing client configs stop working until updated. */
	CURSORUNREALCORE_API FString RegenerateMcpToken();

	/** Length-independent comparison, so a wrong token cannot be guessed from response timing. */
	CURSORUNREALCORE_API bool ConstantTimeEquals(const FString& A, const FString& B);
}

/** Scrubs secrets out of text before it is logged, shown or saved. */
namespace CursorRedactor
{
	/** Remember a secret value so every later Redact call removes it. */
	CURSORUNREALCORE_API void RegisterSecret(const FString& Value);

	/** Removes registered secrets and anything shaped like an API key or bearer token. */
	CURSORUNREALCORE_API FString Redact(const FString& Text);
}
