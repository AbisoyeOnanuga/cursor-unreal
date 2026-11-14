#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CursorUnrealSettings.generated.h"

UENUM()
enum class ECursorProvider : uint8
{
	/** Talks to the Anthropic API directly from the editor using your own API key. */
	Anthropic UMETA(DisplayName = "Anthropic (your API key)"),
	/** Runs a Cursor agent through the Node bridge using your Cursor API key. */
	Cursor UMETA(DisplayName = "Cursor (Cursor API key, needs Node.js)")
};

UENUM()
enum class ECursorApprovalPolicy : uint8
{
	/** Read-only and undoable tools run freely. Dangerous tools ask first. */
	AskForDangerous UMETA(DisplayName = "Ask before dangerous tools"),
	/** Anything that changes the project asks first. */
	AskForChanges UMETA(DisplayName = "Ask before any change"),
	/** Nothing asks. Use only in a project you can throw away or restore. */
	AllowAll UMETA(DisplayName = "Allow everything"),
	/** Only read-only tools run. Everything else is refused. */
	ReadOnly UMETA(DisplayName = "Read-only")
};

/**
 * How much work the model puts into a reply, thinking included. Sent as output_config.effort.
 * Not every model accepts every level; a level the chosen model does not list is left out of the
 * request, which gives that model's own default.
 */
UENUM()
enum class ECursorEffort : uint8
{
	/** Send nothing and let the model use its own default. */
	ModelDefault UMETA(DisplayName = "Model default"),
	Low,
	Medium,
	High,
	XHigh UMETA(DisplayName = "Extra high"),
	Max
};

/**
 * Which of Cursor's own tools (read, shell, …) the Cursor provider keeps. Editor tools from this
 * plugin are always offered. Cursor's tools run in the bridge and cannot ask for approval, so the
 * safer modes take the changing ones away instead.
 */
UENUM()
enum class ECursorBuiltinTools : uint8
{
	/** Read, search and edit project files. No shell, no deletes, no subagents. */
	Files UMETA(DisplayName = "Read and edit files"),
	/** Read and search only. */
	ReadOnly UMETA(DisplayName = "Read-only"),
	/** Editor tools only. Cursor's file tools are off. */
	None UMETA(DisplayName = "None (editor tools only)"),
	/** Everything, shell included. Only for a project you can throw away or restore. */
	All UMETA(DisplayName = "All (includes shell)")
};

CURSORUNREALCORE_API const TCHAR* LexToString(ECursorBuiltinTools Mode);

CURSORUNREALCORE_API const TCHAR* LexToString(ECursorEffort Effort);

/**
 * Per-user, per-project settings. Stored under Saved/Config, so nothing here is shared through
 * source control. API keys are never stored here: they live in the OS credential store.
 */
UCLASS(config = EditorPerProjectUserSettings, meta = (DisplayName = "Cursor Unreal"))
class CURSORUNREALCORE_API UCursorUnrealSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UCursorUnrealSettings();

	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	static const UCursorUnrealSettings* Get() { return GetDefault<UCursorUnrealSettings>(); }
	static UCursorUnrealSettings* GetMutable() { return GetMutableDefault<UCursorUnrealSettings>(); }

	// --- Model -------------------------------------------------------------------------------

	UPROPERTY(config, EditAnywhere, Category = "Model")
	ECursorProvider Provider = ECursorProvider::Anthropic;

	/** Model id sent to Anthropic. Pick from the list in the chat panel, which is fetched from the API. */
	UPROPERTY(config, EditAnywhere, Category = "Model")
	FString AnthropicModel;

	/** Model id sent to Cursor. Pick from the list in the chat panel. "default" lets Cursor choose (the list shows it as Auto); "auto" is read the same way. */
	UPROPERTY(config, EditAnywhere, Category = "Model")
	FString CursorModel = TEXT("default");

	/** Anthropic API endpoint. Must be https unless it points at this machine. */
	UPROPERTY(config, EditAnywhere, Category = "Model", AdvancedDisplay)
	FString AnthropicBaseUrl = TEXT("https://api.anthropic.com");

	/** Value of the anthropic-version header. Change it only for a gateway that needs an older one. */
	UPROPERTY(config, EditAnywhere, Category = "Model", AdvancedDisplay)
	FString AnthropicApiVersion = TEXT("2026-10-21");

	// --- Prompt parameters -------------------------------------------------------------------
	// There is no temperature setting: Anthropic API version 2026-10-21 rejects temperature,
	// top_p and top_k on every model.

	/**
	 * Upper limit on tokens the model may write per reply. Thinking counts towards it, so a low
	 * value can cut a reply short. Lowered automatically to the chosen model's own ceiling.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Prompt", meta = (ClampMin = 1024, ClampMax = 128000))
	int32 MaxTokens = 32000;

	/** How hard the model works on each reply. Lower is faster and cheaper. Anthropic provider only. */
	UPROPERTY(config, EditAnywhere, Category = "Prompt")
	ECursorEffort Effort = ECursorEffort::ModelDefault;

	/**
	 * Ask for a readable summary of the model's thinking and show it in the chat. Off is a little
	 * faster to the first word of the answer. Thinking is billed the same either way.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Prompt")
	bool bShowThinking = true;

	/** Added after the built-in system prompt. Use it for project rules you want in every conversation. */
	UPROPERTY(config, EditAnywhere, Category = "Prompt", meta = (MultiLine = true))
	FString ExtraSystemPrompt;

	/** Stops a runaway turn: the most model round trips allowed for one message. */
	UPROPERTY(config, EditAnywhere, Category = "Prompt", meta = (ClampMin = 1, ClampMax = 200))
	int32 MaxToolIterations = 40;

	// --- Safety ------------------------------------------------------------------------------

	UPROPERTY(config, EditAnywhere, Category = "Safety")
	ECursorApprovalPolicy ApprovalPolicy = ECursorApprovalPolicy::AskForDangerous;

	/** Tool names that never ask, whatever their risk. The Read-only policy still refuses them. */
	UPROPERTY(config, EditAnywhere, Category = "Safety")
	TArray<FString> AlwaysAllowedTools;

	/**
	 * Keep a copy of each conversation under Saved/CursorUnreal/Transcripts on this machine.
	 * API keys and tokens are scrubbed before anything is written.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Safety")
	bool bSaveTranscripts = true;

	// --- Tools and skills --------------------------------------------------------------------

	/** Tool packs to hide from agents in this project. Fewer tools means a smaller prompt. */
	UPROPERTY(config, EditAnywhere, Category = "Tools")
	TArray<FName> DisabledToolPacks;

	/** Skills switched off for the in-editor agent. */
	UPROPERTY(config, EditAnywhere, Category = "Tools")
	TArray<FString> DisabledSkills;

	/** Skills from ~/.cursor/skills switched on for the in-editor agent. They are off by default because most are not about Unreal. */
	UPROPERTY(config, EditAnywhere, Category = "Tools")
	TArray<FString> EnabledUserSkills;

	// --- Connections -------------------------------------------------------------------------

	/** Serve editor tools to Cursor IDE and other MCP clients on this machine. Not needed for the in-editor panel. */
	UPROPERTY(config, EditAnywhere, Category = "Connections")
	bool bEnableMcpServer = true;

	/** Loopback port for the MCP server. Each open project needs its own. */
	UPROPERTY(config, EditAnywhere, Category = "Connections", meta = (ClampMin = 1024, ClampMax = 65535))
	int32 McpPort = 18732;

	/** Keep the editor ticking at full rate while an agent is working, even when it is not the focused window. */
	UPROPERTY(config, EditAnywhere, Category = "Connections")
	bool bStayResponsiveInBackground = true;

	/**
	 * Which of Cursor's built-in tools the Cursor provider may use. Does not apply to the Anthropic
	 * provider, which only has this plugin's editor tools.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Connections")
	ECursorBuiltinTools CursorBuiltinTools = ECursorBuiltinTools::Files;

	/** Path to node.exe for the Cursor bridge. Leave empty to find it on PATH. */
	UPROPERTY(config, EditAnywhere, Category = "Connections", AdvancedDisplay)
	FString NodeExecutable;

	bool IsPackEnabled(FName Pack) const { return !DisabledToolPacks.Contains(Pack); }

	/** Empty when the URL is acceptable, otherwise the reason it is not. */
	static FString ValidateBaseUrl(const FString& Url);
};
