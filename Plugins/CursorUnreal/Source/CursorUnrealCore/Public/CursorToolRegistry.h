#pragma once

#include "CoreMinimal.h"
#include "CursorTool.h"

/**
 * Every editor capability an agent can use lives here. All three ways of reaching the editor
 * (in-process calls from the in-editor agent, the stdio pipe to the Cursor bridge, and the MCP
 * server) end in Invoke, so approval, undo, timing and logging behave the same for all of them.
 */
class CURSORUNREALCORE_API FCursorToolRegistry
{
public:
	static FCursorToolRegistry& Get();

	void Register(const TSharedRef<ICursorTool>& Tool);
	void Unregister(FName Name);
	/** Removes every tool in a pack. Modules call this on shutdown. */
	void UnregisterPack(FName Pack);

	TSharedPtr<ICursorTool> Find(FName Name) const;

	/**
	 * Tools to offer a caller, sorted by name. Leaves out packs the project switched off and, for
	 * callers other than the in-editor agent, tools marked NativeAgentOnly.
	 */
	TArray<TSharedRef<ICursorTool>> GetTools(const FString& Caller) const;
	/** Every registered tool, whatever the settings say. */
	TArray<TSharedRef<ICursorTool>> GetAllTools() const;
	TArray<FName> GetPacks() const;

	/**
	 * Runs a tool. Must be called on the game thread. OnComplete always fires exactly once, with
	 * an error result if the tool is unknown, switched off, refused or misbehaves.
	 */
	void Invoke(FName Name, const TSharedPtr<FJsonObject>& Args, const FCursorToolContext& Ctx, FCursorToolCompletion OnComplete);

	FSimpleMulticastDelegate& OnToolsChanged() { return ToolsChanged; }

	/** Results longer than this are cut, with a note, so one call cannot flood the model's context. */
	static constexpr int32 MaxResultChars = 100000;

	static const FString CallerNative;
	static const FString CallerCursorSdk;
	static const FString CallerMcp;

private:
	TMap<FName, TSharedRef<ICursorTool>> Tools;
	FSimpleMulticastDelegate ToolsChanged;
};
