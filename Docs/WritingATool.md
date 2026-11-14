# Writing a tool

Tools are `ICursorTool` implementations registered with `FCursorToolRegistry`. Prefer `FCursorToolBuilder` for one-shot sync or async lambdas.

## Checklist

1. Pick a pack name (`blueprint`, `materials`, …). Packs can be disabled in project settings.
2. Name the tool `snake_case`, unique across the registry.
3. Set `ECursorToolRisk`: `ReadOnly`, `Mutating` (undo transaction), or `Dangerous`.
4. Write a clear description (when to use it) and a JSON Schema via `FCursorSchema`.
5. Resolve assets with `CursorToolHelpers` / `CursorPackSupport`. Refuse `/Engine` writes.
6. Return `FCursorToolResult::OkJson` for structured results the model can re-parse.
7. Register in module `StartupModule`, unregister the pack on shutdown.
8. Add a `CursorUnreal.Tools.*` automation test under `Private/Tests/`.

## Audience

Use `ECursorToolAudience::NativeAgentOnly` for capabilities Cursor already has (for example source file read/write). MCP and Cursor-SDK callers will not see those tools.

## Niagara / optional packs

Optional engine features belong in their own module (see `CursorUnrealToolsNiagara`) so a project can omit the dependency. Depend on `CursorUnrealTools` only for the public helpers in `CursorToolHelpers.h`.

## Worked example: `demo_ping`

A minimal ReadOnly tool in the `editor` pack. Add the registration next to the other editor tools (or a new `.cpp` with its own `Register`/`Unregister` pair).

```cpp
FCursorToolBuilder(TEXT("demo_ping"), TEXT("editor"))
	.Describe(TEXT("Return the open map name and engine version. Use to verify the tool path is alive."))
	.Risk(ECursorToolRisk::ReadOnly)
	.Schema(FCursorSchema()
		.Bool(TEXT("verbose"), TEXT("Include the caller id. Default false.")))
	.Run([](const FCursorArgs& Args, const FCursorToolContext& Ctx)
	{
		UWorld* World = CursorToolHelpers::GetEditorWorld();
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("map"), World ? World->GetMapName() : TEXT("(none)"));
		Out->SetStringField(TEXT("engine"), FEngineVersion::Current().ToString());
		if (Args.Bool(TEXT("verbose")))
		{
			Out->SetStringField(TEXT("caller"), Ctx.Caller);
		}
		return FCursorToolResult::OkJson(Out);
	})
	.Register();
```

### Test sketch

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDemoPingTest, "CursorUnreal.Tools.DemoPing", CursorTest::Flags)
bool FDemoPingTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	const FCursorToolResult R = CursorTest::Call(TEXT("demo_ping"), TEXT("{\"verbose\":true}"));
	TestFalse(TEXT("demo_ping succeeds"), R.bIsError);
	TestTrue(TEXT("reports engine"), !R.bIsError && R.Text.Contains(TEXT("engine")));
	return true;
}
```

### Naming for unity builds

Helpers in each pack `.cpp` should use a unique prefix (or live in an anonymous namespace with uniquely named free functions) so a unity build that merges several packs does not collide on `AsValue` / `LoadX` symbols.
