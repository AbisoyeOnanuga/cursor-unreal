#include "CursorSecrets.h"

#include "CursorUnrealLog.h"
#include "HAL/PlatformMisc.h"
#include "Internationalization/Regex.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <wincred.h>
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"
#elif PLATFORM_UNIX || PLATFORM_MAC
#include <stdio.h>
#endif

namespace
{
#if PLATFORM_WINDOWS
	/** Windows Credential Manager, generic credentials, visible to the current user only. */
	class FWindowsSecretStore final : public ISecretStore
	{
	public:
		virtual bool IsAvailable() const override { return true; }

		virtual bool Get(const FString& Name, FString& OutValue) const override
		{
			PCREDENTIALW Credential = nullptr;
			if (!::CredReadW(*TargetName(Name), CRED_TYPE_GENERIC, 0, &Credential) || Credential == nullptr)
			{
				return false;
			}
			const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Credential->CredentialBlob), static_cast<int32>(Credential->CredentialBlobSize));
			OutValue = FString(FStringView(Converted.Get(), Converted.Length()));
			::CredFree(Credential);
			return !OutValue.IsEmpty();
		}

		virtual bool Set(const FString& Name, const FString& Value) override
		{
			const FString Target = TargetName(Name);
			const FTCHARToUTF8 Utf8(*Value);
			FString UserName = TEXT("CursorUnreal");

			CREDENTIALW Credential;
			FMemory::Memzero(Credential);
			Credential.Type = CRED_TYPE_GENERIC;
			Credential.TargetName = const_cast<LPWSTR>(*Target);
			Credential.UserName = const_cast<LPWSTR>(*UserName);
			Credential.CredentialBlobSize = static_cast<DWORD>(Utf8.Length());
			Credential.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<ANSICHAR*>(Utf8.Get()));
			Credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
			return ::CredWriteW(&Credential, 0) != 0;
		}

		virtual bool Delete(const FString& Name) override
		{
			return ::CredDeleteW(*TargetName(Name), CRED_TYPE_GENERIC, 0) != 0;
		}

	private:
		static FString TargetName(const FString& Name)
		{
			return FString::Printf(TEXT("CursorUnreal/%s"), *Name);
		}
	};
#endif

	/** Platforms without an implementation yet: nothing is stored, environment variables still work. */
	class FNullSecretStore final : public ISecretStore
	{
	public:
		virtual bool IsAvailable() const override { return false; }
		virtual bool Get(const FString&, FString&) const override { return false; }
		virtual bool Set(const FString&, const FString&) override { return false; }
		virtual bool Delete(const FString&) override { return false; }
	};

	FCriticalSection GRedactorLock;
	TArray<FString> GRegisteredSecrets;

	/** Used only when there is no credential store, so the token lasts for one editor session. */
	FString GSessionMcpToken;

	/**
	 * Bytes from the operating system's cryptographic random number generator.
	 *
	 * Deliberately not FGuid::NewGuid(): GUIDs are made to be unique, not secret. On Linux the
	 * engine falls back to a generator built from timers and rand() when getrandom is unavailable,
	 * and its own source says that fallback has far fewer than 128 bits of randomness.
	 * Returns false rather than falling back to anything weaker.
	 */
	bool FillWithSecureRandom(uint8* Buffer, int32 NumBytes)
	{
#if PLATFORM_WINDOWS
		return ::BCryptGenRandom(nullptr, Buffer, static_cast<ULONG>(NumBytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
#elif PLATFORM_UNIX || PLATFORM_MAC
		bool bOk = false;
		if (FILE* Source = fopen("/dev/urandom", "rb"))
		{
			bOk = fread(Buffer, 1, static_cast<size_t>(NumBytes), Source) == static_cast<size_t>(NumBytes);
			fclose(Source);
		}
		return bOk;
#else
		return false;
#endif
	}
}

namespace CursorSecrets
{
	ISecretStore& GetStore()
	{
#if PLATFORM_WINDOWS
		static FWindowsSecretStore Store;
#else
		static FNullSecretStore Store;
#endif
		return Store;
	}

	FString Resolve(const FString& Name, const TCHAR* EnvVar, ECursorSecretSource* OutSource)
	{
		FString Value;
		ECursorSecretSource Source = ECursorSecretSource::None;

		if (GetStore().Get(Name, Value) && !Value.IsEmpty())
		{
			Source = ECursorSecretSource::Store;
		}
		else if (EnvVar != nullptr)
		{
			Value = FPlatformMisc::GetEnvironmentVariable(EnvVar).TrimStartAndEnd();
			if (!Value.IsEmpty())
			{
				Source = ECursorSecretSource::Environment;
			}
		}

		if (OutSource)
		{
			*OutSource = Source;
		}
		if (!Value.IsEmpty())
		{
			CursorRedactor::RegisterSecret(Value);
		}
		return Value;
	}

	FString ResolveAnthropicKey(ECursorSecretSource* OutSource)
	{
		return Resolve(AnthropicApiKey, TEXT("ANTHROPIC_API_KEY"), OutSource);
	}

	FString ResolveCursorKey(ECursorSecretSource* OutSource)
	{
		return Resolve(CursorApiKey, TEXT("CURSOR_API_KEY"), OutSource);
	}

	FString GenerateToken()
	{
		// 32 bytes = 256 bits, written as 64 hex characters.
		uint8 Bytes[32];
		if (!FillWithSecureRandom(Bytes, UE_ARRAY_COUNT(Bytes)))
		{
			return FString();
		}
		const FString Token = BytesToHex(Bytes, UE_ARRAY_COUNT(Bytes)).ToLower();
		FMemory::Memzero(Bytes, sizeof(Bytes));
		return Token;
	}

	FString GetOrCreateMcpToken()
	{
		FString Token;
		if (GetStore().Get(McpToken, Token) && !Token.IsEmpty())
		{
			CursorRedactor::RegisterSecret(Token);
			return Token;
		}
		if (!GSessionMcpToken.IsEmpty())
		{
			return GSessionMcpToken;
		}

		Token = GenerateToken();
		if (Token.IsEmpty())
		{
			// Fail closed: callers treat an empty token as "do not start", and an empty token never matches a request.
			UE_LOG(LogCursorUnreal, Error, TEXT("The operating system's secure random generator is unavailable, so no MCP token was created and the MCP server will not start."));
			return FString();
		}
		CursorRedactor::RegisterSecret(Token);
		if (!GetStore().Set(McpToken, Token))
		{
			// No credential store: the token lasts for this editor session only.
			UE_LOG(LogCursorUnreal, Warning, TEXT("No credential store available. The MCP token will change every time the editor starts."));
			GSessionMcpToken = Token;
		}
		return Token;
	}

	FString RegenerateMcpToken()
	{
		// Forget both copies, or a platform without a credential store would hand the old token straight back.
		GetStore().Delete(McpToken);
		GSessionMcpToken.Reset();
		return GetOrCreateMcpToken();
	}

	bool ConstantTimeEquals(const FString& A, const FString& B)
	{
		const FTCHARToUTF8 Ua(*A);
		const FTCHARToUTF8 Ub(*B);
		const int32 LenA = Ua.Length();
		const int32 LenB = Ub.Length();
		uint32 Diff = static_cast<uint32>(LenA ^ LenB);
		const int32 Max = FMath::Max(LenA, LenB);
		for (int32 i = 0; i < Max; ++i)
		{
			const uint8 Ca = i < LenA ? static_cast<uint8>(Ua.Get()[i]) : 0;
			const uint8 Cb = i < LenB ? static_cast<uint8>(Ub.Get()[i]) : 0;
			Diff |= static_cast<uint32>(Ca ^ Cb);
		}
		return Diff == 0;
	}
}

namespace CursorRedactor
{
	void RegisterSecret(const FString& Value)
	{
		// Very short values would shred ordinary text if redacted.
		if (Value.Len() < 8)
		{
			return;
		}
		FScopeLock Lock(&GRedactorLock);
		GRegisteredSecrets.AddUnique(Value);
	}

	FString Redact(const FString& Text)
	{
		if (Text.IsEmpty())
		{
			return Text;
		}

		FString Out = Text;
		{
			FScopeLock Lock(&GRedactorLock);
			for (const FString& Secret : GRegisteredSecrets)
			{
				Out.ReplaceInline(*Secret, TEXT("[REDACTED]"), ESearchCase::CaseSensitive);
			}
		}

		// Things shaped like credentials, in case one arrives that was never registered
		// (pasted into a prompt, echoed by a tool, printed by a script).
		static const TCHAR* Patterns[] = {
			TEXT("sk-ant-[A-Za-z0-9_\\-]{16,}"),
			TEXT("sk-[A-Za-z0-9_\\-]{32,}"),
			TEXT("(?:key|crsr|cursor)_[A-Za-z0-9]{24,}"),
			TEXT("(?i)(bearer\\s+)[A-Za-z0-9._\\-]{16,}"),
			TEXT("(?i)((?:x-api-key|api[_-]?key|authorization)\"?\\s*[:=]\\s*\"?)[A-Za-z0-9._\\-]{16,}"),
		};

		for (const TCHAR* PatternText : Patterns)
		{
			const FRegexPattern Pattern(PatternText);
			FRegexMatcher Matcher(Pattern, Out);
			FString Rebuilt;
			int32 Cursor = 0;
			bool bAny = false;
			while (Matcher.FindNext())
			{
				bAny = true;
				const int32 Begin = Matcher.GetMatchBeginning();
				const int32 End = Matcher.GetMatchEnding();
				Rebuilt += Out.Mid(Cursor, Begin - Cursor);
				// Keep a captured prefix such as "Bearer " so the text still reads sensibly.
				const int32 PrefixBegin = Matcher.GetCaptureGroupBeginning(1);
				const int32 PrefixEnd = Matcher.GetCaptureGroupEnding(1);
				if (PrefixBegin != INDEX_NONE && PrefixEnd > PrefixBegin)
				{
					Rebuilt += Out.Mid(PrefixBegin, PrefixEnd - PrefixBegin);
				}
				Rebuilt += TEXT("[REDACTED]");
				Cursor = End;
			}
			if (bAny)
			{
				Rebuilt += Out.Mid(Cursor);
				Out = MoveTemp(Rebuilt);
			}
		}
		return Out;
	}
}
