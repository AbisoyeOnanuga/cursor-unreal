#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * How much damage a tool can do. The approval gate and the undo wrapper both key off this.
 *  - ReadOnly:  inspects the editor, changes nothing. Always runs.
 *  - Mutating:  changes editor state inside an undo transaction.
 *  - Dangerous: can do things undo will not bring back (run arbitrary code, delete, write files).
 */
enum class ECursorToolRisk : uint8
{
	ReadOnly,
	Mutating,
	Dangerous
};

/** Which agents a tool is offered to. */
enum class ECursorToolAudience : uint8
{
	/** Offered to every caller. */
	All,
	/** Only the in-editor agent. Use for things Cursor agents already do themselves, such as reading and writing source files. */
	NativeAgentOnly
};

CURSORUNREALCORE_API const TCHAR* LexToString(ECursorToolRisk Risk);

/** What a tool hands back. Text is what the model reads. */
struct CURSORUNREALCORE_API FCursorToolResult
{
	bool bIsError = false;
	FString Text;
	TSharedPtr<FJsonObject> Structured;

	static FCursorToolResult Ok(const FString& InText);
	/** Text becomes the compact JSON; Structured keeps the object for callers that can use it. */
	static FCursorToolResult OkJson(const TSharedRef<FJsonObject>& Json);
	static FCursorToolResult Error(const FString& Message);
};

/** Who is calling and how the call arrived. */
struct FCursorToolContext
{
	/** "native" (in-editor agent), "cursor-sdk" (Node bridge) or "mcp" (Cursor IDE and other MCP clients). */
	FString Caller;
	FString SessionId;
	FString CallId;
	/** Wall-clock time the client sent the request, in Unix milliseconds. Zero when unknown. Used to measure transport time. */
	double ClientSentUnixMs = 0.0;
	/** Set by composite tools whose sub-calls were already approved as a group. */
	bool bPreApproved = false;
	bool bInBatch = false;
};

using FCursorToolCompletion = TFunction<void(const FCursorToolResult&)>;

/** One call inside a composite tool, reported to the approval gate so it can judge the whole group. */
struct FCursorSubCall
{
	FName ToolName;
	TSharedPtr<FJsonObject> Args;
};

/**
 * A capability offered to agents. Register one with FCursorToolRegistry and it shows up in the
 * in-editor agent, the Cursor bridge and the MCP server with no further wiring.
 */
class ICursorTool
{
public:
	virtual ~ICursorTool() = default;

	/** snake_case, unique across the registry. */
	virtual FName GetName() const = 0;
	/** Group this tool belongs to. Packs can be switched off per project. */
	virtual FName GetPack() const = 0;
	/** Shown to the model. Say what it does and when to use it. */
	virtual FText GetDescription() const = 0;
	/** JSON Schema (type: object) for the arguments. */
	virtual TSharedRef<FJsonObject> GetInputSchema() const = 0;
	virtual ECursorToolRisk GetRisk() const = 0;
	virtual ECursorToolAudience GetAudience() const { return ECursorToolAudience::All; }

	/** Composite tools list the calls they will make so approval covers all of them. */
	virtual void GetSubCalls(const TSharedRef<FJsonObject>& Args, TArray<FCursorSubCall>& OutCalls) const {}
	/** Return true to stop the registry from opening an undo transaction around Execute. */
	virtual bool ManagesOwnTransaction() const { return false; }

	/**
	 * Always invoked on the game thread. OnComplete must be called exactly once, either before
	 * returning or later from the game thread (compiles and builds finish later).
	 */
	virtual void Execute(const TSharedRef<FJsonObject>& Args, FCursorToolContext& Ctx, FCursorToolCompletion OnComplete) = 0;
};

/** Typed, forgiving access to tool arguments. Missing fields return the default. */
class CURSORUNREALCORE_API FCursorArgs
{
public:
	explicit FCursorArgs(const TSharedRef<FJsonObject>& InJson) : Json(InJson) {}

	/** True when the field exists and is not null. */
	bool Has(const TCHAR* Name) const;
	FString Str(const TCHAR* Name, const FString& Default = FString()) const;
	int32 Int(const TCHAR* Name, int32 Default = 0) const;
	double Num(const TCHAR* Name, double Default = 0.0) const;
	bool Bool(const TCHAR* Name, bool Default = false) const;
	TArray<FString> StrArray(const TCHAR* Name) const;
	TSharedPtr<FJsonObject> Obj(const TCHAR* Name) const;
	const TArray<TSharedPtr<FJsonValue>>* Arr(const TCHAR* Name) const;
	/** Accepts [x, y, z] or {x, y, z}. */
	bool Vec(const TCHAR* Name, FVector& Out) const;
	/** Accepts [pitch, yaw, roll] or {pitch, yaw, roll}. */
	bool Rot(const TCHAR* Name, FRotator& Out) const;

	const TSharedRef<FJsonObject>& Raw() const { return Json; }

private:
	TSharedRef<FJsonObject> Json;
};

/** Fluent JSON Schema builder for tool arguments. */
class CURSORUNREALCORE_API FCursorSchema
{
public:
	FCursorSchema();

	FCursorSchema& Str(const TCHAR* Name, const TCHAR* Description, bool bRequired = false);
	FCursorSchema& Int(const TCHAR* Name, const TCHAR* Description, bool bRequired = false);
	FCursorSchema& Num(const TCHAR* Name, const TCHAR* Description, bool bRequired = false);
	FCursorSchema& Bool(const TCHAR* Name, const TCHAR* Description, bool bRequired = false);
	FCursorSchema& Enum(const TCHAR* Name, const TCHAR* Description, std::initializer_list<const TCHAR*> Values, bool bRequired = false);
	/** Free-form object. */
	FCursorSchema& Obj(const TCHAR* Name, const TCHAR* Description, bool bRequired = false);
	FCursorSchema& StrArray(const TCHAR* Name, const TCHAR* Description, bool bRequired = false);
	FCursorSchema& ObjArray(const TCHAR* Name, const TCHAR* Description, bool bRequired = false);
	/** Array of three numbers, for locations, rotations and scales. */
	FCursorSchema& Vec3(const TCHAR* Name, const TCHAR* Description, bool bRequired = false);
	/** No type constraint. */
	FCursorSchema& Any(const TCHAR* Name, const TCHAR* Description, bool bRequired = false);

	TSharedRef<FJsonObject> Build() const;

private:
	FCursorSchema& Add(const TCHAR* Name, const TSharedRef<FJsonObject>& Property, const TCHAR* Description, bool bRequired);

	TSharedRef<FJsonObject> Properties;
	TArray<TSharedPtr<FJsonValue>> Required;
};

/**
 * The short way to define a tool: describe it, give it a schema and a lambda, register it.
 * Implement ICursorTool directly when a tool needs state or sub-calls.
 */
class CURSORUNREALCORE_API FCursorToolBuilder
{
public:
	using FSyncFn = TFunction<FCursorToolResult(const FCursorArgs& Args, const FCursorToolContext& Ctx)>;
	using FAsyncFn = TFunction<void(const FCursorArgs& Args, const FCursorToolContext& Ctx, FCursorToolCompletion Done)>;

	FCursorToolBuilder(const TCHAR* InName, const TCHAR* InPack);

	FCursorToolBuilder& Describe(const TCHAR* InDescription);
	FCursorToolBuilder& Risk(ECursorToolRisk InRisk);
	FCursorToolBuilder& Audience(ECursorToolAudience InAudience);
	FCursorToolBuilder& Schema(const FCursorSchema& InSchema);
	/** For tools that finish before returning. */
	FCursorToolBuilder& Run(FSyncFn InFn);
	/** For tools that finish later. Call Done exactly once, on the game thread. */
	FCursorToolBuilder& RunAsync(FAsyncFn InFn);

	TSharedRef<ICursorTool> Build() const;
	/** Builds and adds the tool to FCursorToolRegistry. */
	void Register() const;

private:
	FName Name;
	FName Pack;
	FString Description;
	ECursorToolRisk RiskLevel = ECursorToolRisk::ReadOnly;
	ECursorToolAudience AudienceValue = ECursorToolAudience::All;
	TSharedRef<FJsonObject> SchemaJson;
	FSyncFn SyncFn;
	FAsyncFn AsyncFn;
};
