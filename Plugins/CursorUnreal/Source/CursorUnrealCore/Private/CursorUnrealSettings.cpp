#include "CursorUnrealSettings.h"

UCursorUnrealSettings::UCursorUnrealSettings()
{
	SectionName = TEXT("Cursor Unreal");
}

const TCHAR* LexToString(ECursorEffort Effort)
{
	switch (Effort)
	{
	case ECursorEffort::Low: return TEXT("low");
	case ECursorEffort::Medium: return TEXT("medium");
	case ECursorEffort::High: return TEXT("high");
	case ECursorEffort::XHigh: return TEXT("xhigh");
	case ECursorEffort::Max: return TEXT("max");
	default: return TEXT("");
	}
}

const TCHAR* LexToString(ECursorBuiltinTools Mode)
{
	switch (Mode)
	{
	case ECursorBuiltinTools::All: return TEXT("all");
	case ECursorBuiltinTools::ReadOnly: return TEXT("read-only");
	case ECursorBuiltinTools::None: return TEXT("none");
	default: return TEXT("files");
	}
}

FString UCursorUnrealSettings::ValidateBaseUrl(const FString& Url)
{
	const FString Trimmed = Url.TrimStartAndEnd();
	if (Trimmed.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
	{
		return FString();
	}
	if (Trimmed.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase))
	{
		// Plain http would send the API key in the clear, so allow it only to this machine.
		FString Host = Trimmed.Mid(7);
		int32 Slash = INDEX_NONE;
		if (Host.FindChar(TEXT('/'), Slash))
		{
			Host.LeftInline(Slash);
		}
		int32 Colon = INDEX_NONE;
		if (Host.FindLastChar(TEXT(':'), Colon) && !Host.EndsWith(TEXT("]")))
		{
			Host.LeftInline(Colon);
		}
		if (Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) || Host == TEXT("127.0.0.1") || Host == TEXT("[::1]"))
		{
			return FString();
		}
		return TEXT("The endpoint must use https unless it points at localhost, because the API key is sent with every request.");
	}
	return TEXT("The endpoint must start with https://.");
}
