#include "CursorBridgeInstall.h"

#include "CursorUnrealSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	FString FirstLine(const FString& Text)
	{
		FString Line = Text;
		Line.ReplaceInline(TEXT("\r"), TEXT(""));
		int32 Newline = INDEX_NONE;
		if (Line.FindChar(TEXT('\n'), Newline))
		{
			Line.LeftInline(Newline);
		}
		return Line.TrimStartAndEnd();
	}

	bool NodeMeetsMinimum(const FString& Version)
	{
		// The SDK needs Node 22.13 or newer.
		TArray<FString> Parts;
		Version.ParseIntoArray(Parts, TEXT("."), true);
		if (Parts.Num() < 2)
		{
			return false;
		}
		const int32 Major = FCString::Atoi(*Parts[0]);
		const int32 Minor = FCString::Atoi(*Parts[1]);
		return Major > 22 || (Major == 22 && Minor >= 13);
	}
}

FString FCursorBridgeInstall::StatusLine() const
{
	if (bOk)
	{
		return FString::Printf(TEXT("Ready. Node %s, Cursor SDK %s."),
			NodeVersion.IsEmpty() ? TEXT("?") : *NodeVersion,
			SdkVersion.IsEmpty() ? TEXT("?") : *SdkVersion);
	}
	return Problem.IsEmpty() ? TEXT("The Cursor bridge is not ready.") : Problem;
}

FString CursorBridge::GetBridgeDirectory()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CursorUnreal"));
	if (!Plugin.IsValid())
	{
		return FString();
	}
	return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Bridge"));
}

FString CursorBridge::GetBridgeScriptPath()
{
	const FString Dir = GetBridgeDirectory();
	return Dir.IsEmpty() ? FString() : Dir / TEXT("bridge.mjs");
}

FString CursorBridge::FindNodeExecutable()
{
	const FString Configured = UCursorUnrealSettings::Get()->NodeExecutable.TrimStartAndEnd();
	if (!Configured.IsEmpty())
	{
		if (FPaths::FileExists(Configured))
		{
			return FPaths::ConvertRelativePathToFull(Configured);
		}
		return FString();
	}

#if PLATFORM_WINDOWS
	FString StdOut;
	FString StdErr;
	int32 ReturnCode = 1;
	FPlatformProcess::ExecProcess(TEXT("where"), TEXT("node"), &ReturnCode, &StdOut, &StdErr);
	if (ReturnCode == 0)
	{
		const FString Path = FirstLine(StdOut);
		if (!Path.IsEmpty() && FPaths::FileExists(Path))
		{
			return Path;
		}
	}
#else
	FString StdOut;
	FString StdErr;
	int32 ReturnCode = 1;
	FPlatformProcess::ExecProcess(TEXT("/usr/bin/which"), TEXT("node"), &ReturnCode, &StdOut, &StdErr);
	if (ReturnCode == 0)
	{
		const FString Path = FirstLine(StdOut);
		if (!Path.IsEmpty() && FPaths::FileExists(Path))
		{
			return Path;
		}
	}
#endif
	return FString();
}

FString CursorBridge::ReadNodeVersion(const FString& NodePath)
{
	if (NodePath.IsEmpty())
	{
		return FString();
	}
	FString StdOut;
	FString StdErr;
	int32 ReturnCode = 1;
	FPlatformProcess::ExecProcess(*NodePath, TEXT("--version"), &ReturnCode, &StdOut, &StdErr);
	if (ReturnCode != 0)
	{
		return FString();
	}
	FString Version = FirstLine(StdOut);
	if (Version.StartsWith(TEXT("v")) || Version.StartsWith(TEXT("V")))
	{
		Version.RightChopInline(1);
	}
	return Version;
}

FString CursorBridge::ReadSdkVersion()
{
	const FString Manifest = GetBridgeDirectory() / TEXT("node_modules") / TEXT("@cursor") / TEXT("sdk") / TEXT("package.json");
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Manifest))
	{
		return FString();
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return FString();
	}
	FString Version;
	Root->TryGetStringField(TEXT("version"), Version);
	return Version;
}

FCursorBridgeInstall CursorBridge::ProbeInstall()
{
	FCursorBridgeInstall Report;
	Report.BridgeScript = GetBridgeScriptPath();
	if (Report.BridgeScript.IsEmpty() || !FPaths::FileExists(Report.BridgeScript))
	{
		Report.Problem = TEXT("The Cursor bridge script is missing from the plugin's Bridge folder.");
		return Report;
	}

	Report.NodePath = FindNodeExecutable();
	if (Report.NodePath.IsEmpty())
	{
		Report.Problem = TEXT("Node.js was not found. Install Node 22.13 or newer, or set its path under Connections → Node Executable.");
		return Report;
	}

	Report.NodeVersion = ReadNodeVersion(Report.NodePath);
	if (Report.NodeVersion.IsEmpty())
	{
		Report.Problem = FString::Printf(TEXT("Could not run \"%s --version\"."), *Report.NodePath);
		return Report;
	}
	if (!NodeMeetsMinimum(Report.NodeVersion))
	{
		Report.Problem = FString::Printf(
			TEXT("Node.js %s is too old. The Cursor SDK needs 22.13 or newer."), *Report.NodeVersion);
		return Report;
	}

	Report.SdkVersion = ReadSdkVersion();
	if (Report.SdkVersion.IsEmpty())
	{
		Report.Problem = TEXT("The Cursor SDK is not installed in the plugin's Bridge folder. Open a terminal there and run: npm install");
		return Report;
	}

	Report.bOk = true;
	return Report;
}
