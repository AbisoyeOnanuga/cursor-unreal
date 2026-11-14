#pragma once

#include "CoreMinimal.h"

/** Whether the Cursor bridge can be started on this machine, and why not when it cannot. */
struct CURSORUNREALAGENT_API FCursorBridgeInstall
{
	bool bOk = false;
	FString NodePath;
	FString NodeVersion;
	FString BridgeScript;
	FString SdkVersion;
	FString Problem;

	/** One short line for the Setup tab. */
	FString StatusLine() const;
};

namespace CursorBridge
{
	/** Absolute path of Plugins/CursorUnreal/Bridge. Empty when the plugin folder is missing. */
	CURSORUNREALAGENT_API FString GetBridgeDirectory();

	/** Absolute path of bridge.mjs. Empty when missing. */
	CURSORUNREALAGENT_API FString GetBridgeScriptPath();

	/**
	 * Finds node.exe: the path in the settings if it exists, otherwise the first `node` on PATH.
	 * Empty when none is found.
	 */
	CURSORUNREALAGENT_API FString FindNodeExecutable();

	/** What `node --version` prints, without a leading 'v' when present. Empty on failure. */
	CURSORUNREALAGENT_API FString ReadNodeVersion(const FString& NodePath);

	/** Version from Bridge/node_modules/@cursor/sdk/package.json. Empty when not installed. */
	CURSORUNREALAGENT_API FString ReadSdkVersion();

	/**
	 * Checks Node, the bridge script, and that `npm install` has been run in the Bridge folder.
	 * Does not start a process.
	 */
	CURSORUNREALAGENT_API FCursorBridgeInstall ProbeInstall();
}
