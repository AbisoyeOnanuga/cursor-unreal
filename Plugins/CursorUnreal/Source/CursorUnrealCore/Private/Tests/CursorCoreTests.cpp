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
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

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

	// Later calls can use what earlier ones returned.
	FCursorToolBuilder(TEXT("__make"), TestPack)
		.Run([](const FCursorArgs&, const FCursorToolContext&)
		{
			return FCursorToolResult::OkJson(Json(TEXT("{\"id\":\"Node_7\",\"count\":3,\"pins\":[{\"name\":\"execute\"},{\"name\":\"then\"}]}")).ToSharedRef());
		})
		.Register();
	auto ResultOf = [&](int32 Index) { return Last.Structured->GetArrayField(TEXT("results"))[Index]->AsObject()->GetStringField(TEXT("result")); };

	RunBatch(TEXT("{\"calls\":[{\"tool\":\"__make\",\"as\":\"first\"},")
		TEXT("{\"tool\":\"__a\",\"args\":{\"text\":\"$first.id\"}},")
		TEXT("{\"tool\":\"__a\",\"args\":{\"text\":\"$0.pins.1.name\"}},")
		TEXT("{\"tool\":\"__a\",\"args\":{\"text\":\"${first.id}.${first.pins.0.name} x${first.count}\"}},")
		TEXT("{\"tool\":\"__a\",\"args\":{\"text\":\"$$first.id\"}},")
		TEXT("{\"tool\":\"__a\",\"args\":{\"text\":\"$100 and ${HOME}\"}},")
		TEXT("{\"tool\":\"__a\",\"args\":{\"text\":\"$99\"}}]}"));
	TestFalse(TEXT("batch with references succeeds: ") + Last.Text, Last.bIsError);
	TestEqual(TEXT("a named result's field is passed on"), ResultOf(1), FString(TEXT("Node_7")));
	TestEqual(TEXT("a result can be named by position and reached into"), ResultOf(2), FString(TEXT("then")));
	TestEqual(TEXT("references inside longer text"), ResultOf(3), FString(TEXT("Node_7.execute x3")));
	TestEqual(TEXT("a doubled dollar is a literal one"), ResultOf(4), FString(TEXT("$first.id")));
	TestEqual(TEXT("text that names no call is left alone"), ResultOf(5), FString(TEXT("$100 and ${HOME}")));
	TestEqual(TEXT("a number past the end of the batch is text"), ResultOf(6), FString(TEXT("$99")));
	TestEqual(TEXT("the name is reported back"), Last.Structured->GetArrayField(TEXT("results"))[0]->AsObject()->GetStringField(TEXT("as")), FString(TEXT("first")));

	Order.Reset();
	RunBatch(TEXT("{\"calls\":[{\"tool\":\"__make\",\"as\":\"first\"},{\"tool\":\"__a\",\"args\":{\"text\":\"$first.guid\"}},{\"tool\":\"__b\"}]}"));
	TestTrue(TEXT("a field the result does not have is an error"), Last.bIsError);
	TestTrue(TEXT("the error lists the fields that exist: ") + ResultOf(1), ResultOf(1).Contains(TEXT("no 'guid'")) && ResultOf(1).Contains(TEXT("id, count, pins")));
	TestEqual(TEXT("the call with the bad reference never ran, nor the one after"), Order.Num(), 0);

	RunBatch(TEXT("{\"calls\":[{\"tool\":\"__a\",\"args\":{\"text\":\"$later.id\"}},{\"tool\":\"__make\",\"as\":\"later\"}]}"));
	TestTrue(TEXT("a reference to a later call is an error: ") + ResultOf(0), Last.bIsError && ResultOf(0).Contains(TEXT("runs later")));
	RunBatch(TEXT("{\"calls\":[{\"tool\":\"__a\",\"args\":{\"text\":\"hello\"},\"as\":\"plain\"},{\"tool\":\"__a\",\"args\":{\"text\":\"$plain.id\"}},{\"tool\":\"__a\",\"args\":{\"text\":\"$plain\"}}]}"));
	TestTrue(TEXT("a field of a plain-text result is an error: ") + ResultOf(1), Last.bIsError && ResultOf(1).Contains(TEXT("plain text")));
	RunBatch(TEXT("{\"stop_on_error\":false,\"calls\":[{\"tool\":\"__a\",\"args\":{\"fail\":true},\"as\":\"bad\"},{\"tool\":\"__a\",\"args\":{\"text\":\"$bad.id\"}},{\"tool\":\"__a\",\"args\":{\"text\":\"hello\"},\"as\":\"plain\"},{\"tool\":\"__a\",\"args\":{\"text\":\"$plain\"}}]}"));
	TestTrue(TEXT("a reference to a failed call is an error: ") + ResultOf(1), ResultOf(1).Contains(TEXT("failed")));
	TestEqual(TEXT("a whole plain-text result can be passed on"), ResultOf(3), FString(TEXT("hello")));
	RunBatch(TEXT("{\"calls\":[{\"tool\":\"__a\",\"as\":\"x\"},{\"tool\":\"__b\",\"as\":\"x\"}]}"));
	TestTrue(TEXT("a name used twice is refused"), Last.bIsError && Last.Text.Contains(TEXT("named 'x'")));
	RunBatch(TEXT("{\"calls\":[{\"tool\":\"__a\",\"as\":\"1st\"}]}"));
	TestTrue(TEXT("a name that is not an identifier is refused"), Last.bIsError);

	RunBatch(TEXT("{\"brief\":true,\"stop_on_error\":false,\"calls\":[{\"tool\":\"__make\"},{\"tool\":\"__a\",\"args\":{\"fail\":true}}]}"));
	const TArray<TSharedPtr<FJsonValue>>& BriefResults = Last.Structured->GetArrayField(TEXT("results"));
	TestFalse(TEXT("brief leaves out the result of a successful call"), BriefResults[0]->AsObject()->HasField(TEXT("result")));
	TestTrue(TEXT("brief keeps the result of a failed call"), BriefResults[1]->AsObject()->HasField(TEXT("result")));

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorImagesTest, "CursorUnreal.Core.Images", CursorCoreTests::Flags)
bool FCursorImagesTest::RunTest(const FString&)
{
	FString Error;

	// A render-sized picture is shrunk to the limit and sent as a JPEG.
	{
		constexpr int32 Width = 2000;
		constexpr int32 Height = 1000;
		TArray<FColor> Pixels;
		Pixels.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Pixels[Y * Width + X] = FColor(static_cast<uint8>(X * 255 / Width), static_cast<uint8>(Y * 255 / Height), 128, 0);
			}
		}
		FCursorImage Photo;
		TestTrue(TEXT("pixels become a picture"), CursorImages::FromPixels(Pixels, Width, Height, TEXT("render"), true, Photo, Error));
		TestEqual(TEXT("a render is a JPEG"), Photo.MimeType, FString(TEXT("image/jpeg")));
		TestEqual(TEXT("long edge shrunk to the limit"), Photo.Width, CursorImages::MaxLongEdge);
		TestEqual(TEXT("shape kept"), Photo.Height, CursorImages::MaxLongEdge / 2);
		TestTrue(TEXT("small enough to send"), Photo.NumBytes() > 0 && Photo.NumBytes() < CursorImages::MaxEncodedBytes);
		TestEqual(TEXT("a hash is forty hex digits"), Photo.Hash().Len(), 40);
		TestTrue(TEXT("described for people"), Photo.Describe().StartsWith(TEXT("render, 1568 x 784, ")));
		TestFalse(TEXT("base64 for sending"), Photo.ToBase64().IsEmpty());

		TArray<uint8> Shown;
		int32 ShownWidth = 0;
		int32 ShownHeight = 0;
		TestTrue(TEXT("decodes for the chat"), CursorImages::DecodeForDisplay(Photo, 256, Shown, ShownWidth, ShownHeight));
		TestTrue(TEXT("thumbnail size"), ShownWidth == 256 && ShownHeight == 128 && Shown.Num() == 256 * 128 * 4);
		// The alpha the pixels came with (zero, as a render target gives) must not make the picture invisible.
		TestEqual(TEXT("opaque"), static_cast<int32>(Shown[3]), 255);

		TestFalse(TEXT("pixel count must match the size"), CursorImages::FromPixels(Pixels, Width, Height + 1, TEXT("bad"), true, Photo, Error));
	}

	// A small PNG goes through byte for byte; other formats are converted.
	{
		const FCursorImage Small = CursorTest::MakeImage(1, 8, 6);
		TestEqual(TEXT("not a photo: PNG"), Small.MimeType, FString(TEXT("image/png")));
		FCursorImage Again;
		TestTrue(TEXT("an encoded PNG is read"), CursorImages::FromEncoded(Small.Data->GetData(), Small.Data->Num(), TEXT("again.png"), Again, Error));
		TestEqual(TEXT("and passed through untouched"), Again.Hash(), Small.Hash());
		TestTrue(TEXT("with its size read"), Again.Width == 8 && Again.Height == 6);
		TestNotEqual(TEXT("different pictures have different hashes"), CursorTest::MakeImage(2).Hash(), Small.Hash());

		FImage Source(5, 3, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		for (FColor& Pixel : Source.AsBGRA8())
		{
			Pixel = FColor(200, 100, 50, 255);
		}
		TArray64<uint8> Bmp;
		IImageWrapperModule& Wrappers = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TestTrue(TEXT("test BMP made"), Wrappers.CompressImage(Bmp, EImageFormat::BMP, Source));
		FCursorImage FromBmp;
		TestTrue(TEXT("a BMP is read"), CursorImages::FromEncoded(Bmp.GetData(), Bmp.Num(), TEXT("old.bmp"), FromBmp, Error));
		TestEqual(TEXT("and converted to PNG"), FromBmp.MimeType, FString(TEXT("image/png")));
		TestTrue(TEXT("same size"), FromBmp.Width == 5 && FromBmp.Height == 3);
	}

	// A picture copied to the Windows clipboard: a bitmap without a file header, bottom row first, fourth byte unused.
	{
		TArray<uint8> Dib;
		auto Put32 = [&Dib](uint32 Value) { Dib.Append(reinterpret_cast<const uint8*>(&Value), 4); };
		auto Put16 = [&Dib](uint16 Value) { Dib.Append(reinterpret_cast<const uint8*>(&Value), 2); };
		Put32(40); Put32(2); Put32(2); Put16(1); Put16(32); Put32(0); Put32(16); Put32(0); Put32(0); Put32(0); Put32(0);
		// Bottom row: blue, green. Top row: red, white. Bytes are B, G, R, unused.
		const uint8 Rows[16] = { 255, 0, 0, 0,   0, 255, 0, 0,   0, 0, 255, 0,   255, 255, 255, 0 };
		Dib.Append(Rows, 16);

		FCursorImage Pasted;
		TestTrue(FString::Printf(TEXT("a clipboard bitmap is read (%s)"), *Error), CursorImages::FromDib(Dib.GetData(), Dib.Num(), TEXT("pasted picture"), Pasted, Error));
		TestTrue(TEXT("size"), Pasted.Width == 2 && Pasted.Height == 2);
		TArray<uint8> Shown;
		int32 W = 0;
		int32 H = 0;
		if (CursorImages::DecodeForDisplay(Pasted, 16, Shown, W, H) && Shown.Num() == 16)
		{
			// Top-left pixel, as B, G, R, A.
			TestTrue(TEXT("top row first: red at the top left"), Shown[0] == 0 && Shown[1] == 0 && Shown[2] == 255);
			TestEqual(TEXT("the unused byte is not read as transparency"), static_cast<int32>(Shown[3]), 255);
			TestTrue(TEXT("blue at the bottom left"), Shown[8] == 255 && Shown[9] == 0 && Shown[10] == 0);
		}
		else
		{
			AddError(TEXT("the pasted picture did not decode"));
		}
		Dib.SetNum(30);
		TestFalse(TEXT("a cut-off bitmap is refused"), CursorImages::FromDib(Dib.GetData(), Dib.Num(), TEXT("pasted picture"), Pasted, Error));
	}

	// Things that are not pictures.
	{
		FCursorImage Out;
		const char* Text = "This is a text file, not a picture.";
		TestFalse(TEXT("text is refused"), CursorImages::FromEncoded(Text, FCStringAnsi::Strlen(Text), TEXT("notes.png"), Out, Error));
		TestTrue(TEXT("and the message names it"), Error.Contains(TEXT("notes.png")) && Error.Contains(TEXT("PNG, JPEG, BMP or TGA")));
		TestFalse(TEXT("empty is refused"), CursorImages::FromEncoded(nullptr, 0, TEXT("empty.png"), Out, Error));
		TestFalse(TEXT("a missing file is refused"), CursorImages::FromFile(FPaths::ProjectSavedDir() / TEXT("no-such-picture.png"), Out, Error));
		TestFalse(TEXT("a file that is not a picture is refused by name"), CursorImages::FromFile(TEXT("C:/model.fbx"), Out, Error));
		TestTrue(TEXT("file types"), CursorImages::IsImageFile(TEXT("a/b.PNG")) && CursorImages::IsImageFile(TEXT("c.jpeg")) && !CursorImages::IsImageFile(TEXT("d.webp")));
	}

	// Saved copies are named by what is in them, under Saved, and reading one back gives the same picture.
	{
		const FCursorImage Picture = CursorTest::MakeImage(77, 12, 9);
		const FString File = CursorImages::SaveAttachment(Picture);
		TestTrue(TEXT("saved"), !File.IsEmpty() && FPaths::FileExists(File));
		TestTrue(TEXT("under Saved/CursorUnreal/Attachments"), File.StartsWith(CursorImages::GetAttachmentDirectory()) && File.Contains(TEXT("/Saved/")));
		TestTrue(TEXT("named by hash"), FPaths::GetBaseFilename(File) == Picture.Hash().Left(20) && File.EndsWith(TEXT(".png")));
		TestEqual(TEXT("saving again gives the same file"), CursorImages::SaveAttachment(Picture), File);
		FCursorImage Read;
		TestTrue(TEXT("reads back"), CursorImages::FromFile(File, Read, Error));
		TestEqual(TEXT("identical"), Read.Hash(), Picture.Hash());
		IFileManager::Get().Delete(*File, false, true);
	}

	// A tool can return a picture, and a batch hands on the pictures of its calls.
	{
		CursorTest::FScopedPolicy Policy{ FName(TestPack) };
		FCursorToolBuilder(TEXT("__picture"), TestPack)
			.Schema(FCursorSchema().Int(TEXT("seed"), TEXT("Which picture.")))
			.Run([](const FCursorArgs& Args, const FCursorToolContext&)
			{
				FCursorToolResult Result = FCursorToolResult::Ok(TEXT("{\"taken\":true}"));
				Result.Images.Add(CursorTest::MakeImage(Args.Int(TEXT("seed"), 1)));
				return Result;
			})
			.Register();

		const FCursorToolResult One = CursorTest::Call(TEXT("__picture"), TEXT("{\"seed\":5}"));
		TestTrue(TEXT("the picture comes back with the result"), !One.bIsError && One.Images.Num() == 1 && One.Images[0].Hash() == CursorTest::MakeImage(5).Hash());

		const FCursorToolResult Batch = CursorTest::Call(TEXT("batch"),
			TEXT("{\"brief\":true,\"calls\":[{\"tool\":\"__picture\",\"args\":{\"seed\":5}},{\"tool\":\"__picture\",\"args\":{\"seed\":6}}]}"));
		TestTrue(TEXT("a batch hands on both pictures, in order"), !Batch.bIsError && Batch.Images.Num() == 2
			&& Batch.Images[0].Hash() == CursorTest::MakeImage(5).Hash() && Batch.Images[1].Hash() == CursorTest::MakeImage(6).Hash());
		TestTrue(TEXT("and says which calls had one"), Batch.Text.Contains(TEXT("\"images\":1")));
		TestFalse(TEXT("picture bytes are never in the text"), Batch.Text.Contains(CursorTest::MakeImage(5).ToBase64().Left(24)));
	}
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
		TEXT("unreal-animation"), TEXT("unreal-audio"), TEXT("unreal-editor-tools"), TEXT("unreal-optimisation"), TEXT("unreal-project-conventions"), TEXT("unreal-level-design") })
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
