#if WITH_DEV_AUTOMATION_TESTS

#include "CursorActivity.h"
#include "CursorApprovalGate.h"
#include "CursorJson.h"
#include "CursorLogCapture.h"
#include "CursorPerfLog.h"
#include "CursorSecrets.h"
#include "CursorSkillLibrary.h"
#include "CursorTestSupport.h"
#include "CursorTool.h"
#include "CursorToolRegistry.h"
#include "CursorUnrealLog.h"
#include "CursorUnrealSettings.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace CursorCoreTests
{
	const TCHAR* TestPack = TEXT("__test");
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	/** Known policy and no approval UI for the test; everything is restored afterwards. */
	struct FScopedGateState : CursorTest::FScopedPolicy
	{
		FScopedGateState() : CursorTest::FScopedPolicy(TestPack) {}
	};

	void RegisterEcho(const TCHAR* Name, ECursorToolRisk Risk, TArray<FString>* Order = nullptr)
	{
		const FString NameCopy = Name;
		FCursorToolBuilder(Name, TestPack)
			.Describe(TEXT("Test tool."))
			.Risk(Risk)
			.Schema(FCursorSchema().Str(TEXT("text"), TEXT("Echoed back.")).Bool(TEXT("fail"), TEXT("Return an error.")))
			.Run([NameCopy, Order](const FCursorArgs& Args, const FCursorToolContext&)
			{
				if (Order)
				{
					Order->Add(NameCopy);
				}
				if (Args.Bool(TEXT("fail")))
				{
					return FCursorToolResult::Error(TEXT("asked to fail"));
				}
				return FCursorToolResult::Ok(Args.Str(TEXT("text"), TEXT("ok")));
			})
			.Register();
	}

	FCursorToolContext Ctx()
	{
		FCursorToolContext C;
		C.Caller = FCursorToolRegistry::CallerNative;
		C.SessionId = TEXT("test");
		return C;
	}

	TSharedPtr<FJsonObject> Json(const TCHAR* Text)
	{
		return CursorJson::Parse(Text);
	}

	FString MakeTempDir(const TCHAR* Label)
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectIntermediateDir() / TEXT("CursorUnrealTests") / (FString(Label) + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Short)));
		IFileManager::Get().MakeDirectory(*Dir, true);
		return Dir;
	}

	void WriteSkill(const FString& Root, const FString& Folder, const FString& Name, const FString& Description, const FString& Body)
	{
		const FString Text = FString::Printf(TEXT("---\nname: %s\ndescription: %s\n---\n%s\n"), *Name, *Description, *Body);
		FFileHelper::SaveStringToFile(Text, *(Root / Folder / TEXT("SKILL.md")));
	}
}

using namespace CursorCoreTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorRegistryTest, "CursorUnreal.Core.Registry", CursorCoreTests::Flags)
bool FCursorRegistryTest::RunTest(const FString&)
{
	FScopedGateState Scope;
	RegisterEcho(TEXT("__echo"), ECursorToolRisk::ReadOnly);

	int32 Completions = 0;
	FCursorToolResult Last;
	auto Capture = [&](const FCursorToolResult& R) { ++Completions; Last = R; };

	FCursorToolRegistry::Get().Invoke(TEXT("__echo"), Json(TEXT("{\"text\":\"hello\"}")), Ctx(), Capture);
	TestEqual(TEXT("completes once"), Completions, 1);
	TestFalse(TEXT("not an error"), Last.bIsError);
	TestEqual(TEXT("echoes text"), Last.Text, FString(TEXT("hello")));

	FCursorToolRegistry::Get().Invoke(TEXT("__missing"), nullptr, Ctx(), Capture);
	TestEqual(TEXT("unknown tool still completes"), Completions, 2);
	TestTrue(TEXT("unknown tool is an error"), Last.bIsError);

	UCursorUnrealSettings::GetMutable()->DisabledToolPacks.Add(TestPack);
	FCursorToolRegistry::Get().Invoke(TEXT("__echo"), nullptr, Ctx(), Capture);
	TestTrue(TEXT("disabled pack is refused"), Last.bIsError);
	TestFalse(TEXT("disabled pack is not listed"),
		FCursorToolRegistry::Get().GetTools(FCursorToolRegistry::CallerNative).ContainsByPredicate(
			[](const TSharedRef<ICursorTool>& T) { return T->GetName() == TEXT("__echo"); }));
	UCursorUnrealSettings::GetMutable()->DisabledToolPacks.Reset();

	// Native-only tools are hidden from external callers.
	FCursorToolBuilder(TEXT("__native_only"), TestPack).Audience(ECursorToolAudience::NativeAgentOnly)
		.Run([](const FCursorArgs&, const FCursorToolContext&) { return FCursorToolResult::Ok(TEXT("x")); }).Register();
	auto HasNativeOnly = [](const FString& Caller)
	{
		return FCursorToolRegistry::Get().GetTools(Caller).ContainsByPredicate(
			[](const TSharedRef<ICursorTool>& T) { return T->GetName() == TEXT("__native_only"); });
	};
	TestTrue(TEXT("native agent sees native-only tool"), HasNativeOnly(FCursorToolRegistry::CallerNative));
	TestFalse(TEXT("mcp does not see native-only tool"), HasNativeOnly(FCursorToolRegistry::CallerMcp));

	// A tool that completes later: nothing fires until it says so, and a second completion is ignored.
	FCursorToolCompletion Held;
	FCursorToolBuilder(TEXT("__deferred"), TestPack)
		.RunAsync([&Held](const FCursorArgs&, const FCursorToolContext&, FCursorToolCompletion Done) { Held = MoveTemp(Done); })
		.Register();
	Completions = 0;
	FCursorToolRegistry::Get().Invoke(TEXT("__deferred"), nullptr, Ctx(), Capture);
	TestEqual(TEXT("deferred tool has not completed yet"), Completions, 0);
	Held(FCursorToolResult::Ok(TEXT("later")));
	AddExpectedError(TEXT("reported completion more than once"), EAutomationExpectedErrorFlags::Contains, 1);
	Held(FCursorToolResult::Ok(TEXT("again")));
	TestEqual(TEXT("deferred tool completes exactly once"), Completions, 1);
	TestEqual(TEXT("first completion wins"), Last.Text, FString(TEXT("later")));

	// Oversized results are cut.
	FCursorToolBuilder(TEXT("__huge"), TestPack)
		.Run([](const FCursorArgs&, const FCursorToolContext&) { return FCursorToolResult::Ok(FString::ChrN(FCursorToolRegistry::MaxResultChars + 5000, TEXT('x'))); })
		.Register();
	FCursorToolRegistry::Get().Invoke(TEXT("__huge"), nullptr, Ctx(), Capture);
	TestTrue(TEXT("huge result is truncated"), Last.Text.Len() < FCursorToolRegistry::MaxResultChars + 500 && Last.Text.Contains(TEXT("truncated")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorApprovalTest, "CursorUnreal.Core.Approval", CursorCoreTests::Flags)
bool FCursorApprovalTest::RunTest(const FString&)
{
	using V = ECursorApprovalVerdict;
	using P = ECursorApprovalPolicy;
	using R = ECursorToolRisk;
	const TArray<FString> NoList;
	const TSet<FName> NoSession;
	const FName Tool(TEXT("t"));

	TestTrue(TEXT("read-only always allowed"), FCursorApprovalGate::Classify(Tool, R::ReadOnly, P::ReadOnly, NoList, NoSession) == V::Allow);
	TestTrue(TEXT("default: mutating allowed"), FCursorApprovalGate::Classify(Tool, R::Mutating, P::AskForDangerous, NoList, NoSession) == V::Allow);
	TestTrue(TEXT("default: dangerous asks"), FCursorApprovalGate::Classify(Tool, R::Dangerous, P::AskForDangerous, NoList, NoSession) == V::Ask);
	TestTrue(TEXT("ask-for-changes: mutating asks"), FCursorApprovalGate::Classify(Tool, R::Mutating, P::AskForChanges, NoList, NoSession) == V::Ask);
	TestTrue(TEXT("allow-all: dangerous allowed"), FCursorApprovalGate::Classify(Tool, R::Dangerous, P::AllowAll, NoList, NoSession) == V::Allow);
	TestTrue(TEXT("read-only policy denies mutating"), FCursorApprovalGate::Classify(Tool, R::Mutating, P::ReadOnly, NoList, NoSession) == V::Deny);
	TestTrue(TEXT("allowlist lets dangerous through"), FCursorApprovalGate::Classify(Tool, R::Dangerous, P::AskForDangerous, TArray<FString>{ FString(TEXT("t")) }, NoSession) == V::Allow);
	TestTrue(TEXT("allowlist does not beat read-only policy"), FCursorApprovalGate::Classify(Tool, R::Dangerous, P::ReadOnly, TArray<FString>{ FString(TEXT("t")) }, NoSession) == V::Deny);
	TestTrue(TEXT("session allowance lets dangerous through"), FCursorApprovalGate::Classify(Tool, R::Dangerous, P::AskForDangerous, NoList, TSet<FName>{ Tool }) == V::Allow);

	FScopedGateState Scope;
	int32 Runs = 0;
	FCursorToolBuilder(TEXT("__danger"), TestPack).Risk(ECursorToolRisk::Dangerous)
		.Run([&Runs](const FCursorArgs&, const FCursorToolContext&) { ++Runs; return FCursorToolResult::Ok(TEXT("ran")); }).Register();

	FCursorToolResult Last;
	auto Capture = [&](const FCursorToolResult& Result) { Last = Result; };

	// No prompt available: refused, and the tool body never runs.
	FCursorToolRegistry::Get().Invoke(TEXT("__danger"), nullptr, Ctx(), Capture);
	TestTrue(TEXT("dangerous tool refused without a prompt"), Last.bIsError);
	TestEqual(TEXT("refused tool did not run"), Runs, 0);

	int32 Prompts = 0;
	ECursorApprovalDecision Answer = ECursorApprovalDecision::Deny;
	FCursorApprovalGate::Get().SetHandler(FCursorApprovalHandler::CreateLambda(
		[&](const FCursorApprovalRequest&, TFunction<void(ECursorApprovalDecision)> Respond) { ++Prompts; Respond(Answer); }));

	FCursorToolRegistry::Get().Invoke(TEXT("__danger"), nullptr, Ctx(), Capture);
	TestTrue(TEXT("declined tool returns an error"), Last.bIsError);
	TestEqual(TEXT("declined tool did not run"), Runs, 0);

	Answer = ECursorApprovalDecision::AllowOnce;
	FCursorToolRegistry::Get().Invoke(TEXT("__danger"), nullptr, Ctx(), Capture);
	TestEqual(TEXT("approved tool ran"), Runs, 1);
	FCursorToolRegistry::Get().Invoke(TEXT("__danger"), nullptr, Ctx(), Capture);
	TestEqual(TEXT("allow-once asks again"), Prompts, 3);

	Answer = ECursorApprovalDecision::AllowForSession;
	FCursorToolRegistry::Get().Invoke(TEXT("__danger"), nullptr, Ctx(), Capture);
	FCursorToolRegistry::Get().Invoke(TEXT("__danger"), nullptr, Ctx(), Capture);
	TestEqual(TEXT("allow-for-session stops asking"), Prompts, 4);
	TestEqual(TEXT("all approved calls ran"), Runs, 4);

	// The gate applies to every caller, not just the in-editor agent.
	FCursorApprovalGate::Get().ResetSessionAllowances();
	Answer = ECursorApprovalDecision::Deny;
	FCursorToolContext McpCtx = Ctx();
	McpCtx.Caller = FCursorToolRegistry::CallerMcp;
	FCursorToolRegistry::Get().Invoke(TEXT("__danger"), nullptr, McpCtx, Capture);
	TestTrue(TEXT("mcp caller is gated too"), Last.bIsError);
	TestEqual(TEXT("mcp-denied tool did not run"), Runs, 4);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorBatchTest, "CursorUnreal.Core.Batch", CursorCoreTests::Flags)
bool FCursorBatchTest::RunTest(const FString&)
{
	FScopedGateState Scope;
	TArray<FString> Order;
	RegisterEcho(TEXT("__a"), ECursorToolRisk::ReadOnly, &Order);
	RegisterEcho(TEXT("__b"), ECursorToolRisk::ReadOnly, &Order);
	RegisterEcho(TEXT("__danger"), ECursorToolRisk::Dangerous, &Order);

	FCursorToolResult Last;
	auto Capture = [&](const FCursorToolResult& Result) { Last = Result; };
	auto RunBatch = [&](const TCHAR* ArgsJson) { FCursorToolRegistry::Get().Invoke(TEXT("batch"), Json(ArgsJson), Ctx(), Capture); };

	RunBatch(TEXT("{\"calls\":[{\"tool\":\"__a\",\"args\":{\"text\":\"1\"}},{\"tool\":\"__b\",\"args\":{\"text\":\"2\"}},{\"tool\":\"__a\"}]}"));
	TestFalse(TEXT("batch succeeds"), Last.bIsError);
	TestEqual(TEXT("runs in order"), FString::Join(Order, TEXT(",")), FString(TEXT("__a,__b,__a")));
	TestTrue(TEXT("returns structured results"), Last.Structured.IsValid() && Last.Structured->GetArrayField(TEXT("results")).Num() == 3);

	Order.Reset();
	RunBatch(TEXT("{\"calls\":[{\"tool\":\"__a\"},{\"tool\":\"__b\",\"args\":{\"fail\":true}},{\"tool\":\"__a\"}]}"));
	TestTrue(TEXT("failing batch is an error"), Last.bIsError);
	TestEqual(TEXT("stops at the failure"), Order.Num(), 2);
	TestEqual(TEXT("reports which call failed"), static_cast<int32>(Last.Structured->GetNumberField(TEXT("failed_index"))), 1);

	Order.Reset();
	RunBatch(TEXT("{\"stop_on_error\":false,\"calls\":[{\"tool\":\"__a\"},{\"tool\":\"__b\",\"args\":{\"fail\":true}},{\"tool\":\"__a\"}]}"));
	TestEqual(TEXT("stop_on_error=false keeps going"), Order.Num(), 3);

	RunBatch(TEXT("{\"calls\":[{\"tool\":\"batch\",\"args\":{\"calls\":[]}}]}"));
	TestTrue(TEXT("nested batch refused"), Last.bIsError);
	RunBatch(TEXT("{\"calls\":[]}"));
	TestTrue(TEXT("empty batch refused"), Last.bIsError);

	// A dangerous call hidden inside a batch still needs approval, asked once for the group.
	Order.Reset();
	RunBatch(TEXT("{\"calls\":[{\"tool\":\"__a\"},{\"tool\":\"__danger\"}]}"));
	TestTrue(TEXT("batch with a dangerous call is refused without a prompt"), Last.bIsError);
	TestEqual(TEXT("nothing in the refused batch ran"), Order.Num(), 0);

	int32 Prompts = 0;
	TArray<FString> Listed;
	FCursorApprovalGate::Get().SetHandler(FCursorApprovalHandler::CreateLambda(
		[&](const FCursorApprovalRequest& Request, TFunction<void(ECursorApprovalDecision)> Respond)
		{
			++Prompts;
			Listed = Request.SubCalls;
			Respond(ECursorApprovalDecision::AllowOnce);
		}));
	RunBatch(TEXT("{\"calls\":[{\"tool\":\"__a\"},{\"tool\":\"__danger\"},{\"tool\":\"__danger\"}]}"));
	TestEqual(TEXT("one prompt for the whole batch"), Prompts, 1);
	TestEqual(TEXT("prompt lists the calls that need approval"), Listed.Num(), 2);
	TestEqual(TEXT("approved batch ran everything"), Order.Num(), 3);

	// Deferred tools inside a batch: the batch waits, then carries on.
	FCursorToolCompletion Held;
	FCursorToolBuilder(TEXT("__deferred"), TestPack)
		.RunAsync([&Held](const FCursorArgs&, const FCursorToolContext&, FCursorToolCompletion Done) { Held = MoveTemp(Done); }).Register();
	Order.Reset();
	bool bDone = false;
	FCursorToolRegistry::Get().Invoke(TEXT("batch"), Json(TEXT("{\"calls\":[{\"tool\":\"__a\"},{\"tool\":\"__deferred\"},{\"tool\":\"__b\"}]}")), Ctx(),
		[&](const FCursorToolResult& Result) { bDone = true; Last = Result; });
	TestFalse(TEXT("batch waits for the deferred call"), bDone);
	TestEqual(TEXT("only the first call ran so far"), Order.Num(), 1);
	Held(FCursorToolResult::Ok(TEXT("deferred done")));
	TestTrue(TEXT("batch finishes after the deferred call"), bDone);
	TestEqual(TEXT("the call after the deferred one ran"), Order.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorSkillsTest, "CursorUnreal.Core.Skills", CursorCoreTests::Flags)
bool FCursorSkillsTest::RunTest(const FString&)
{
	// Frontmatter parsing, including the folded form Cursor's own skills use.
	{
		TMap<FString, FString> Front;
		FString Body;
		const bool bOk = FCursorSkillLibrary::ParseSkillFile(
			TEXT("---\r\nname: my-skill\r\ndescription: >-\r\n  First line\r\n  second line.\r\ndisable-model-invocation: true\r\n---\r\n# Title\r\n\r\nBody text.\r\n"),
			Front, Body);
		TestTrue(TEXT("frontmatter found"), bOk);
		TestEqual(TEXT("name parsed"), Front.FindRef(TEXT("name")), FString(TEXT("my-skill")));
		TestEqual(TEXT("folded description joined"), Front.FindRef(TEXT("description")), FString(TEXT("First line second line.")));
		TestEqual(TEXT("body has no frontmatter"), Body, FString(TEXT("# Title\n\nBody text.")));
		TestFalse(TEXT("file without frontmatter"), FCursorSkillLibrary::ParseSkillFile(TEXT("# Just text"), Front, Body));
	}

	FCursorSkillLibrary::FLayerDirs Dirs;
	Dirs.BuiltIn = MakeTempDir(TEXT("builtin"));
	Dirs.User = MakeTempDir(TEXT("user"));
	Dirs.Project = MakeTempDir(TEXT("project")) / TEXT(".cursor") / TEXT("skills");

	WriteSkill(Dirs.BuiltIn, TEXT("alpha"), TEXT("alpha"), TEXT("built-in alpha"), TEXT("ALPHA BUILTIN"));
	WriteSkill(Dirs.BuiltIn, TEXT("beta"), TEXT("beta"), TEXT("built-in beta"), TEXT("BETA BUILTIN"));
	WriteSkill(Dirs.BuiltIn, TEXT("gamma"), TEXT("gamma"), TEXT("built-in gamma"), TEXT("GAMMA BUILTIN"));
	FFileHelper::SaveStringToFile(TEXT("reference text"), *(Dirs.BuiltIn / TEXT("alpha") / TEXT("reference.md")));
	WriteSkill(Dirs.User, TEXT("beta"), TEXT("beta"), TEXT("user beta"), TEXT("BETA USER"));
	WriteSkill(Dirs.User, TEXT("gamma"), TEXT("gamma"), TEXT("user gamma"), TEXT("GAMMA USER"));
	WriteSkill(Dirs.Project, TEXT("gamma"), TEXT("gamma"), TEXT("project gamma"), TEXT("GAMMA PROJECT"));

	FCursorSkillLibrary Library(Dirs);
	TestEqual(TEXT("one entry per name"), Library.List().Num(), 3);

	auto LayerOf = [&](const TCHAR* Name) { const FCursorSkillInfo* I = Library.Find(Name); return I ? I->Layer : ECursorSkillLayer::BuiltIn; };
	TestTrue(TEXT("alpha comes from built-in"), LayerOf(TEXT("alpha")) == ECursorSkillLayer::BuiltIn);
	TestTrue(TEXT("user beats built-in"), LayerOf(TEXT("beta")) == ECursorSkillLayer::User);
	TestTrue(TEXT("project beats user and built-in"), LayerOf(TEXT("gamma")) == ECursorSkillLayer::Project);
	TestTrue(TEXT("override is flagged"), Library.Find(TEXT("gamma"))->bOverridesLowerLayer);

	FString Body, Error;
	TestTrue(TEXT("loads"), Library.Load(TEXT("gamma"), Body, Error));
	TestEqual(TEXT("loads the winning layer's body"), Body, FString(TEXT("GAMMA PROJECT")));
	TestTrue(TEXT("index lists skills"), Library.BuildIndexForPrompt().Contains(TEXT("- alpha: built-in alpha")));

	FString Text;
	TestTrue(TEXT("reads a reference file"), Library.ReadFile(TEXT("alpha"), TEXT("reference.md"), Text, Error) && Text == TEXT("reference text"));
	TestFalse(TEXT("refuses to leave the skill folder"), Library.ReadFile(TEXT("alpha"), TEXT("../beta/SKILL.md"), Text, Error));
	TestEqual(TEXT("lists reference files"), Library.ListFiles(TEXT("alpha")).Num(), 1);

	// Install: copies what is missing, leaves existing project skills alone.
	FCursorSkillInstallReport Report = Library.InstallBuiltInsToProject();
	TestEqual(TEXT("installs alpha (2 files) and beta"), Report.Installed.Num(), 3);
	TestEqual(TEXT("keeps the project's own gamma"), Report.SkippedEdited.Num(), 1);
	TestTrue(TEXT("alpha is now a project skill"), LayerOf(TEXT("alpha")) == ECursorSkillLayer::Project);
	TestTrue(TEXT("fresh install counts as unmodified"), Library.Find(TEXT("alpha"))->bInstalledUnmodified);
	Library.Load(TEXT("gamma"), Body, Error);
	TestEqual(TEXT("project gamma untouched"), Body, FString(TEXT("GAMMA PROJECT")));

	// The plugin updates a skill: unmodified project copies follow, edited ones do not.
	WriteSkill(Dirs.BuiltIn, TEXT("alpha"), TEXT("alpha"), TEXT("built-in alpha"), TEXT("ALPHA BUILTIN V2"));
	WriteSkill(Dirs.BuiltIn, TEXT("beta"), TEXT("beta"), TEXT("built-in beta"), TEXT("BETA BUILTIN V2"));
	WriteSkill(Dirs.Project, TEXT("beta"), TEXT("beta"), TEXT("built-in beta"), TEXT("BETA EDITED BY USER"));
	Report = Library.InstallBuiltInsToProject();
	TestEqual(TEXT("unmodified copy is updated"), Report.Updated.Num(), 1);
	Library.Load(TEXT("alpha"), Body, Error);
	TestEqual(TEXT("alpha picked up the new version"), Body, FString(TEXT("ALPHA BUILTIN V2")));
	Library.Load(TEXT("beta"), Body, Error);
	TestEqual(TEXT("edited copy is never overwritten"), Body, FString(TEXT("BETA EDITED BY USER")));
	TestFalse(TEXT("edited copy is flagged as modified"), Library.Find(TEXT("beta"))->bInstalledUnmodified);

	IFileManager::Get().DeleteDirectory(*(FPaths::ProjectIntermediateDir() / TEXT("CursorUnrealTests")), false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorShippedSkillsTest, "CursorUnreal.Core.ShippedSkills", CursorCoreTests::Flags)
bool FCursorShippedSkillsTest::RunTest(const FString&)
{
	// The skills in the plugin folder itself, read the way the editor reads them.
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CursorUnreal"));
	if (!TestTrue(TEXT("plugin found"), Plugin.IsValid()))
	{
		return false;
	}
	FCursorSkillLibrary::FLayerDirs Dirs;
	Dirs.BuiltIn = Plugin->GetBaseDir() / TEXT("Skills");
	FCursorSkillLibrary Library(Dirs);

	for (const TCHAR* Name : { TEXT("unreal-blueprints"), TEXT("unreal-cpp-gameplay"), TEXT("unreal-materials"), TEXT("unreal-niagara"),
		TEXT("unreal-animation"), TEXT("unreal-audio"), TEXT("unreal-editor-tools"), TEXT("unreal-optimisation"), TEXT("unreal-project-conventions") })
	{
		const FCursorSkillInfo* Info = Library.Find(Name);
		if (!TestNotNull(*FString::Printf(TEXT("%s is shipped"), Name), Info))
		{
			continue;
		}
		// A name that differs from its folder would install under one name and load under another.
		TestEqual(*FString::Printf(TEXT("%s: folder matches name"), Name), FPaths::GetCleanFilename(Info->Directory), FString(Name));
		TestTrue(*FString::Printf(TEXT("%s: has a description for the prompt index"), Name), Info->Description.Len() > 20);

		FString Body, Error;
		TestTrue(*FString::Printf(TEXT("%s: loads"), Name), Library.Load(Name, Body, Error));
		TestTrue(*FString::Printf(TEXT("%s: body is not empty"), Name), Body.Len() > 100);
		TestFalse(*FString::Printf(TEXT("%s: frontmatter is not left in the body"), Name), Body.StartsWith(TEXT("---")));
	}

	// The conventions skill is a template: it has to say so, and cover the four areas it promises.
	FString Body, Error;
	if (Library.Load(TEXT("unreal-project-conventions"), Body, Error))
	{
		TestTrue(TEXT("conventions: marks what is undecided"), Body.Contains(TEXT("TODO(project)")));
		for (const TCHAR* Heading : { TEXT("## Naming"), TEXT("## Folder layout"), TEXT("## Coding standard"), TEXT("## Module boundaries") })
		{
			TestTrue(*FString::Printf(TEXT("conventions: has '%s'"), Heading), Body.Contains(Heading));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorSecretsTest, "CursorUnreal.Core.Secrets", CursorCoreTests::Flags)
bool FCursorSecretsTest::RunTest(const FString&)
{
	// Redaction of registered values.
	const FString Secret = TEXT("zq-test-secret-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	CursorRedactor::RegisterSecret(Secret);
	const FString Redacted = CursorRedactor::Redact(FString::Printf(TEXT("before %s after"), *Secret));
	TestFalse(TEXT("registered secret removed"), Redacted.Contains(Secret));
	TestTrue(TEXT("surrounding text kept"), Redacted.StartsWith(TEXT("before ")) && Redacted.EndsWith(TEXT(" after")));

	// Redaction by shape, for secrets nobody registered.
	TestFalse(TEXT("anthropic-style key removed"),
		CursorRedactor::Redact(TEXT("key is sk-ant-api03-AbCdEfGhIjKlMnOpQrStUvWxYz012345 ok")).Contains(TEXT("AbCdEfGh")));
	const FString Bearer = CursorRedactor::Redact(TEXT("Authorization: Bearer abcdef0123456789abcdef0123456789"));
	TestFalse(TEXT("bearer token removed"), Bearer.Contains(TEXT("abcdef0123456789")));
	TestFalse(TEXT("json api key removed"),
		CursorRedactor::Redact(TEXT("{\"x-api-key\": \"0123456789abcdef0123456789\"}")).Contains(TEXT("0123456789abcdef")));
	TestEqual(TEXT("ordinary text untouched"), CursorRedactor::Redact(TEXT("Spawned BP_Door at 0,0,0")), FString(TEXT("Spawned BP_Door at 0,0,0")));

	TestTrue(TEXT("equal strings compare equal"), CursorSecrets::ConstantTimeEquals(TEXT("abc123"), TEXT("abc123")));
	TestFalse(TEXT("different strings differ"), CursorSecrets::ConstantTimeEquals(TEXT("abc123"), TEXT("abc124")));
	TestFalse(TEXT("prefix does not match"), CursorSecrets::ConstantTimeEquals(TEXT("abc"), TEXT("abc123")));

	// Access tokens come from the OS secure random generator. GenerateToken stores nothing, so this
	// does not touch the real MCP token in the credential store.
	{
		const FString TokenA = CursorSecrets::GenerateToken();
		const FString TokenB = CursorSecrets::GenerateToken();
		TestEqual(TEXT("token is 256 bits as hex"), TokenA.Len(), 64);
		bool bAllHex = !TokenA.IsEmpty();
		for (const TCHAR Ch : TokenA)
		{
			bAllHex &= (Ch >= TEXT('0') && Ch <= TEXT('9')) || (Ch >= TEXT('a') && Ch <= TEXT('f'));
		}
		TestTrue(TEXT("token is lower-case hex"), bAllHex);
		TestTrue(TEXT("two tokens differ"), !TokenA.IsEmpty() && TokenA != TokenB);
		// A stuck generator would produce a run of one repeated byte.
		TestFalse(TEXT("token is not one repeated byte"), TokenA == FString::ChrN(64, TokenA.IsEmpty() ? TEXT('0') : TokenA[0]));
	}

	// Round trip through the OS credential store, under a throwaway name.
	ISecretStore& Store = CursorSecrets::GetStore();
	if (Store.IsAvailable())
	{
		const FString Name = TEXT("__AutomationTest");
		const FString Value = TEXT("value-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FString Read;
		TestTrue(TEXT("store write"), Store.Set(Name, Value));
		TestTrue(TEXT("store read"), Store.Get(Name, Read) && Read == Value);
		ECursorSecretSource Source = ECursorSecretSource::None;
		TestEqual(TEXT("resolve finds the stored value"), CursorSecrets::Resolve(Name, nullptr, &Source), Value);
		TestTrue(TEXT("resolve reports the store as the source"), Source == ECursorSecretSource::Store);
		TestTrue(TEXT("store delete"), Store.Delete(Name));
		TestFalse(TEXT("gone after delete"), Store.Get(Name, Read));
	}
	else
	{
		AddInfo(TEXT("No credential store on this platform; round trip skipped."));
	}

	TestTrue(TEXT("https accepted"), UCursorUnrealSettings::ValidateBaseUrl(TEXT("https://api.anthropic.com")).IsEmpty());
	TestTrue(TEXT("http to localhost accepted"), UCursorUnrealSettings::ValidateBaseUrl(TEXT("http://localhost:8080/v1")).IsEmpty());
	TestFalse(TEXT("http to a remote host refused"), UCursorUnrealSettings::ValidateBaseUrl(TEXT("http://example.com")).IsEmpty());
	TestFalse(TEXT("localhost lookalike refused"), UCursorUnrealSettings::ValidateBaseUrl(TEXT("http://localhost.example.com")).IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorPlumbingTest, "CursorUnreal.Core.Plumbing", CursorCoreTests::Flags)
bool FCursorPlumbingTest::RunTest(const FString&)
{
	// Schema builder.
	const TSharedRef<FJsonObject> Schema = FCursorSchema()
		.Str(TEXT("path"), TEXT("A path."), true)
		.Int(TEXT("limit"), TEXT("A limit."))
		.Enum(TEXT("mode"), TEXT("A mode."), { TEXT("a"), TEXT("b") })
		.Vec3(TEXT("location"), TEXT("A location."))
		.Build();
	TestEqual(TEXT("schema is an object"), Schema->GetStringField(TEXT("type")), FString(TEXT("object")));
	TestEqual(TEXT("required lists only required fields"), Schema->GetArrayField(TEXT("required")).Num(), 1);
	TestEqual(TEXT("enum values kept"), Schema->GetObjectField(TEXT("properties"))->GetObjectField(TEXT("mode"))->GetArrayField(TEXT("enum")).Num(), 2);

	// Argument access, including the loose forms models produce.
	const FCursorArgs Args(Json(TEXT("{\"n\":\"42\",\"flag\":true,\"v\":[1,2,3],\"r\":{\"yaw\":90},\"one\":\"solo\",\"nothing\":null}")).ToSharedRef());
	TestEqual(TEXT("numeric string read as int"), Args.Int(TEXT("n")), 42);
	TestEqual(TEXT("missing int uses default"), Args.Int(TEXT("absent"), 7), 7);
	TestTrue(TEXT("bool read"), Args.Bool(TEXT("flag")));
	FVector V;
	TestTrue(TEXT("vector from array"), Args.Vec(TEXT("v"), V) && V == FVector(1, 2, 3));
	FRotator Rot;
	TestTrue(TEXT("rotator from object"), Args.Rot(TEXT("r"), Rot) && FMath::IsNearlyEqual(Rot.Yaw, 90.0));
	TestEqual(TEXT("single string accepted as a list"), Args.StrArray(TEXT("one")).Num(), 1);
	TestFalse(TEXT("null counts as absent"), Args.Has(TEXT("nothing")));

	// Performance log rollups and CSV, without touching disk.
	FCursorPerfLog Perf;
	Perf.bWriteToDisk = false;
	FCursorToolCallRecord Record;
	Record.ToolName = TEXT("asset_search");
	Record.Status = ECursorCallStatus::Succeeded;
	Record.ExecMs = 10.0;
	Record.QueueMs = 2.0;
	Record.TransportMs = 4.0;
	Perf.RecordTool(Record);
	Record.ExecMs = 30.0;
	Record.TransportMs = -1.0;
	Record.Status = ECursorCallStatus::Failed;
	Perf.RecordTool(Record);
	const TArray<FCursorToolAggregate> Aggregates = Perf.GetToolAggregates();
	TestEqual(TEXT("one aggregate per tool"), Aggregates.Num(), 1);
	TestEqual(TEXT("calls counted"), Aggregates[0].Calls, 2);
	TestEqual(TEXT("errors counted"), Aggregates[0].Errors, 1);
	TestEqual(TEXT("max exec tracked"), Aggregates[0].MaxExecMs, 30.0);
	TestEqual(TEXT("transport averaged over calls that reported it"), Aggregates[0].TransportSamples, 1);

	FCursorTurnPerf Turn;
	Turn.Provider = TEXT("anthropic");
	Turn.Model = TEXT("test-model");
	Turn.Outcome = TEXT("done");
	Turn.InputTokens = 1200;
	Turn.OutputTokens = 340;
	Perf.RecordTurn(Turn);
	const FString CsvPath = FPaths::ProjectIntermediateDir() / TEXT("CursorUnrealTests") / TEXT("perf.csv");
	FString Error, Csv;
	TestTrue(TEXT("csv export"), Perf.ExportCsv(CsvPath, Error));
	FFileHelper::LoadFileToString(Csv, *CsvPath);
	TestTrue(TEXT("csv has the turn"), Csv.Contains(TEXT("turn,")) && Csv.Contains(TEXT("test-model")) && Csv.Contains(TEXT(",1200,340,")));
	TestTrue(TEXT("csv has the tool rollup"), Csv.Contains(TEXT("tool,asset_search,2,1,")));
	IFileManager::Get().DeleteDirectory(*(FPaths::ProjectIntermediateDir() / TEXT("CursorUnrealTests")), false, true);

	// Log capture.
	const int64 Before = FCursorLogCapture::Get().GetLatestSequence();
	const FString Marker = TEXT("capture-marker-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	UE_LOG(LogCursorUnreal, Display, TEXT("%s"), *Marker);
	const TArray<FCursorLogLine> Lines = FCursorLogCapture::Get().GetLines(Before, 50, ELogVerbosity::All, FString(), Marker);
	TestEqual(TEXT("captured the line just logged"), Lines.Num(), 1);

	// Activity window that drives the background throttle override.
	FCursorActivity Activity;
	TestFalse(TEXT("idle to begin with"), Activity.IsAgentActive());
	Activity.BeginSession();
	TestTrue(TEXT("active during a session"), Activity.IsAgentActive());
	Activity.EndSession();
	TestTrue(TEXT("stays active through the grace window"), Activity.IsAgentActive());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
