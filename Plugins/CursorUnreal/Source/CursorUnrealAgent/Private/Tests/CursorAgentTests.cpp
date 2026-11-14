#if WITH_DEV_AUTOMATION_TESTS

#include "AgentBackend.h"
#include "AgentHttpTransport.h"
#include "AgentJsonUtil.h"
#include "AgentSession.h"
#include "AnthropicBackend.h"
#include "AnthropicStream.h"
#include "CursorActivity.h"
#include "CursorJson.h"
#include "CursorTestSupport.h"
#include "CursorTool.h"
#include "CursorToolRegistry.h"
#include "CursorUnrealSettings.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SseParser.h"

// Everything here runs against FAgentFixtureTransport. No request leaves the process, no API key
// is read, and nothing is billed. The fixtures are hand-written from Anthropic's documentation;
// see Resources/Fixtures/anthropic/README.md for what that does and does not prove.

namespace CursorAgentTests
{
	const TCHAR* TestPack = TEXT("__agenttest");
	const TCHAR* FakeKey = TEXT("fixture-key-not-a-real-secret");

	FString FixturePath(const TCHAR* Name)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CursorUnreal"));
		check(Plugin.IsValid());
		return Plugin->GetBaseDir() / TEXT("Resources") / TEXT("Fixtures") / TEXT("anthropic") / Name;
	}

	TArray<uint8> LoadFixture(const TCHAR* Name)
	{
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *FixturePath(Name));
		return Bytes;
	}

	/** Puts the user's model settings back when a test ends. */
	struct FScopedAgentSettings
	{
		FString Model;
		FString BaseUrl;
		FString ApiVersion;
		FString ExtraPrompt;
		int32 MaxTokens;
		int32 MaxIterations;
		ECursorEffort Effort;
		bool bShowThinking;

		FScopedAgentSettings()
		{
			UCursorUnrealSettings* S = UCursorUnrealSettings::GetMutable();
			Model = S->AnthropicModel;
			BaseUrl = S->AnthropicBaseUrl;
			ApiVersion = S->AnthropicApiVersion;
			ExtraPrompt = S->ExtraSystemPrompt;
			MaxTokens = S->MaxTokens;
			MaxIterations = S->MaxToolIterations;
			Effort = S->Effort;
			bShowThinking = S->bShowThinking;

			S->AnthropicModel = TEXT("claude-fixture-1");
			S->AnthropicBaseUrl = TEXT("https://fixture.invalid");
			S->AnthropicApiVersion = TEXT("2026-10-21");
			S->ExtraSystemPrompt = TEXT("Fixture project rule.");
			S->MaxTokens = 32000;
			S->MaxToolIterations = 40;
			S->Effort = ECursorEffort::ModelDefault;
			S->bShowThinking = true;
		}

		~FScopedAgentSettings()
		{
			UCursorUnrealSettings* S = UCursorUnrealSettings::GetMutable();
			S->AnthropicModel = Model;
			S->AnthropicBaseUrl = BaseUrl;
			S->AnthropicApiVersion = ApiVersion;
			S->ExtraSystemPrompt = ExtraPrompt;
			S->MaxTokens = MaxTokens;
			S->MaxToolIterations = MaxIterations;
			S->Effort = Effort;
			S->bShowThinking = bShowThinking;
		}
	};

	/** A backend wired to a fixture transport, with what it emitted and how long it asked to wait before retries. */
	struct FHarness
	{
		CursorTest::FScopedPolicy Policy{ FName(TestPack), ECursorApprovalPolicy::AllowAll };
		FScopedAgentSettings Settings;
		TSharedRef<FAgentFixtureTransport> Transport = MakeShared<FAgentFixtureTransport>();
		TSharedPtr<FAnthropicBackend> Backend;
		TArray<FAgentEvent> Events;
		TArray<float> RetryDelays;
		TArray<FString> ToolOrder;
		FString Key = FakeKey;

		FHarness()
		{
			FAnthropicBackendOptions Options;
			Options.Transport = Transport;
			Options.bPersistModels = false;
			Options.KeyProvider = [this]() { return Key; };
			// Retries fire at once; the fixture transport still delivers the reply only when pumped.
			Options.Scheduler = [this](float Seconds, TFunction<void()> Callback)
			{
				RetryDelays.Add(Seconds);
				Callback();
			};
			Backend = MakeShared<FAnthropicBackend>(MoveTemp(Options));

			FCursorToolBuilder(TEXT("__agent_echo"), TestPack)
				.Describe(TEXT("Test tool: returns its text argument."))
				.Risk(ECursorToolRisk::ReadOnly)
				.Schema(FCursorSchema().Str(TEXT("text"), TEXT("Echoed back.")).Bool(TEXT("fail"), TEXT("Return an error.")))
				.Run([this](const FCursorArgs& Args, const FCursorToolContext&)
				{
					ToolOrder.Add(Args.Str(TEXT("text")));
					if (Args.Bool(TEXT("fail")))
					{
						return FCursorToolResult::Error(TEXT("asked to fail"));
					}
					return FCursorToolResult::Ok(Args.Str(TEXT("text"), TEXT("ok")));
				})
				.Register();
		}

		void Queue(const TCHAR* Fixture, int32 ChunkBytes = 0, int32 Status = 200)
		{
			const bool bLoaded = Transport->EnqueueFile(FixturePath(Fixture), ChunkBytes, Status);
			check(bLoaded);
		}

		FAgentEventSink Sink()
		{
			return [this](const FAgentEvent& Event) { Events.Add(Event); };
		}

		void Send(const TCHAR* Text)
		{
			FAgentTurnRequest Request;
			Request.Text = Text;
			Backend->SendTurn(Request, Sink());
			Transport->PumpAll();
		}

		int32 Count(EAgentEventType Type) const
		{
			int32 N = 0;
			for (const FAgentEvent& Event : Events)
			{
				N += Event.Type == Type ? 1 : 0;
			}
			return N;
		}

		const FAgentEvent* First(EAgentEventType Type) const
		{
			return Events.FindByPredicate([Type](const FAgentEvent& Event) { return Event.Type == Type; });
		}

		FString Joined(EAgentEventType Type) const
		{
			FString Out;
			for (const FAgentEvent& Event : Events)
			{
				if (Event.Type == Type)
				{
					Out += Event.Text;
				}
			}
			return Out;
		}

		/** The outcome of the turn: the Done outcome, or "error". Empty if the turn never ended. */
		FString Outcome() const
		{
			if (Events.Num() == 0)
			{
				return FString();
			}
			const FAgentEvent& Last = Events.Last();
			return Last.Type == EAgentEventType::Done ? Last.Outcome : Last.Type == EAgentEventType::Error ? TEXT("error") : FString();
		}

		TSharedPtr<FJsonObject> RequestBody(int32 Index) const
		{
			return Transport->GetRequests().IsValidIndex(Index) ? CursorJson::Parse(Transport->GetRequests()[Index].Body) : nullptr;
		}

		/**
		 * A copy of an array field. The parsed request is a temporary, so a pointer into it would not outlive the
		 * statement that produced it; the copied entries keep their own values alive.
		 */
		static bool CopyArray(const TSharedPtr<FJsonObject>& Owner, const TCHAR* Field, TArray<TSharedPtr<FJsonValue>>& Out)
		{
			const TArray<TSharedPtr<FJsonValue>>* Found = AgentJson::Arr(Owner, Field);
			if (Found == nullptr)
			{
				return false;
			}
			Out = *Found;
			return true;
		}

		static TSharedPtr<FJsonObject> ObjectAt(const TArray<TSharedPtr<FJsonValue>>& Values, int32 Index)
		{
			return Values.IsValidIndex(Index) && Values[Index].IsValid() && Values[Index]->Type == EJson::Object ? Values[Index]->AsObject() : nullptr;
		}

		/** Message N of request Index, or null. */
		TSharedPtr<FJsonObject> Message(int32 RequestIndex, int32 MessageIndex) const
		{
			TArray<TSharedPtr<FJsonValue>> Messages;
			return CopyArray(RequestBody(RequestIndex), TEXT("messages"), Messages) ? ObjectAt(Messages, MessageIndex) : nullptr;
		}

		TSharedPtr<FJsonObject> Block(int32 RequestIndex, int32 MessageIndex, int32 BlockIndex) const
		{
			TArray<TSharedPtr<FJsonValue>> Content;
			return CopyArray(Message(RequestIndex, MessageIndex), TEXT("content"), Content) ? ObjectAt(Content, BlockIndex) : nullptr;
		}

		int32 NumMessages(int32 RequestIndex) const
		{
			TArray<TSharedPtr<FJsonValue>> Messages;
			return CopyArray(RequestBody(RequestIndex), TEXT("messages"), Messages) ? Messages.Num() : -1;
		}

		int32 NumBlocks(int32 RequestIndex, int32 MessageIndex) const
		{
			TArray<TSharedPtr<FJsonValue>> Content;
			return CopyArray(Message(RequestIndex, MessageIndex), TEXT("content"), Content) ? Content.Num() : -1;
		}
	};

	TArray<FSseEvent> ParseInPieces(const TArray<uint8>& Bytes, int32 PieceSize)
	{
		FSseParser Parser;
		TArray<FSseEvent> Events;
		for (int32 Offset = 0; Offset < Bytes.Num(); Offset += PieceSize)
		{
			Parser.Feed(Bytes.GetData() + Offset, FMath::Min(PieceSize, Bytes.Num() - Offset), Events);
		}
		return Events;
	}

	TArray<FSseEvent> ParseText(const char* Utf8)
	{
		FSseParser Parser;
		TArray<FSseEvent> Events;
		Parser.Feed(reinterpret_cast<const uint8*>(Utf8), FCStringAnsi::Strlen(Utf8), Events);
		return Events;
	}
}

using namespace CursorAgentTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAgentSseParserTest, "CursorUnreal.Agent.SseParser", CursorTest::Flags)
bool FCursorAgentSseParserTest::RunTest(const FString&)
{
	// Field syntax.
	{
		const TArray<FSseEvent> Events = ParseText(
			": a comment\n"
			"event: greeting\n"
			"data: first line\n"
			"data:second line\n"
			"id: 7\n"
			"unknown\n"
			"\n"
			"event: no-data\n"
			"\n"
			"data: plain\n"
			"\n");
		TestEqual(TEXT("an event with no data is not delivered"), Events.Num(), 2);
		if (Events.Num() == 2)
		{
			TestEqual(TEXT("event name"), Events[0].Event, FString(TEXT("greeting")));
			TestEqual(TEXT("data lines joined with a newline; one leading space dropped"), Events[0].Data, FString(TEXT("first line\nsecond line")));
			TestEqual(TEXT("event name does not carry over"), Events[1].Event, FString());
			TestEqual(TEXT("second event data"), Events[1].Data, FString(TEXT("plain")));
		}
	}

	// Line endings: LF, CRLF and lone CR all end a line.
	{
		const TArray<FSseEvent> Events = ParseText("data: a\r\n\r\ndata: b\r\rdata: c\n\n");
		TestEqual(TEXT("three events across mixed line endings"), Events.Num(), 3);
		if (Events.Num() == 3)
		{
			TestEqual(TEXT("crlf"), Events[0].Data, FString(TEXT("a")));
			TestEqual(TEXT("cr"), Events[1].Data, FString(TEXT("b")));
			TestEqual(TEXT("lf"), Events[2].Data, FString(TEXT("c")));
		}
	}

	// A CRLF split between two chunks must not count as two line endings (which would end the event early).
	{
		FSseParser Parser;
		TArray<FSseEvent> Events;
		const char* A = "event: x\r";
		const char* B = "\ndata: y\r";
		const char* C = "\n\r";
		const char* D = "\n";
		Parser.Feed(reinterpret_cast<const uint8*>(A), 9, Events);
		Parser.Feed(reinterpret_cast<const uint8*>(B), 9, Events);
		Parser.Feed(reinterpret_cast<const uint8*>(C), 2, Events);
		Parser.Feed(reinterpret_cast<const uint8*>(D), 1, Events);
		TestEqual(TEXT("one event from a stream split inside CRLF"), Events.Num(), 1);
		if (Events.Num() == 1)
		{
			TestEqual(TEXT("name survives the split"), Events[0].Event, FString(TEXT("x")));
			TestEqual(TEXT("data survives the split"), Events[0].Data, FString(TEXT("y")));
		}
	}

	// Byte order mark at the start, and an unfinished event at the end.
	{
		FSseParser Parser;
		TArray<FSseEvent> Events;
		const char* Text = "\xEF\xBB\xBF" "data: bom\n\ndata: unfinished\n";
		Parser.Feed(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text), Events);
		TestEqual(TEXT("BOM is skipped, unfinished event held back"), Events.Num(), 1);
		if (Events.Num() == 1)
		{
			TestEqual(TEXT("data after BOM"), Events[0].Data, FString(TEXT("bom")));
		}
		TestTrue(TEXT("pending data is reported"), Parser.HasPendingData());
	}

	// Chunk boundaries anywhere, including inside multi-byte characters, give the same result.
	{
		const TArray<uint8> Bytes = LoadFixture(TEXT("text-only.sse"));
		TestTrue(TEXT("fixture loaded"), Bytes.Num() > 0);
		const TArray<FSseEvent> Whole = ParseInPieces(Bytes, Bytes.Num());
		TestEqual(TEXT("events in the text-only fixture"), Whole.Num(), 9);

		for (const int32 PieceSize : { 1, 2, 3, 5, 7, 64 })
		{
			const TArray<FSseEvent> Pieces = ParseInPieces(Bytes, PieceSize);
			bool bSame = Pieces.Num() == Whole.Num();
			for (int32 Index = 0; bSame && Index < Whole.Num(); ++Index)
			{
				bSame = Pieces[Index].Event == Whole[Index].Event && Pieces[Index].Data == Whole[Index].Data;
			}
			TestTrue(FString::Printf(TEXT("same events when fed %d byte(s) at a time"), PieceSize), bSame);
		}

		FString Text;
		for (const FSseEvent& Event : Whole)
		{
			Text += AgentJson::Str(AgentJson::Obj(CursorJson::Parse(Event.Data), TEXT("delta")), TEXT("text"));
		}
		// Accented Latin, an em dash, two CJK characters and an emoji (2, 3, 3 and 4 bytes in UTF-8),
		// written as escapes so this source file's own encoding cannot change the expectation.
		const FString Expected = FString(TEXT("H\u00E9llo \u2014 \u4F60\u597D ")) + FString(TEXT("\U0001F3AE")) + TEXT("!");
		TestEqual(TEXT("multi-byte text survives"), Text, Expected);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAgentStreamTest, "CursorUnreal.Agent.StreamAssembler", CursorTest::Flags)
bool FCursorAgentStreamTest::RunTest(const FString&)
{
	const TArray<uint8> Bytes = LoadFixture(TEXT("tool-use.sse"));
	TestTrue(TEXT("fixture loaded"), Bytes.Num() > 0);

	for (const int32 PieceSize : { 1, 7, Bytes.Num() })
	{
		const FString Label = FString::Printf(TEXT("[%d-byte pieces] "), PieceSize);
		FAnthropicStreamAssembler Assembler;
		TArray<FAgentEvent> Events;
		Assembler.OnEvent = [&Events](const FAgentEvent& Event) { Events.Add(Event); };
		for (int32 Offset = 0; Offset < Bytes.Num(); Offset += PieceSize)
		{
			Assembler.Feed(TConstArrayView<uint8>(Bytes.GetData() + Offset, FMath::Min(PieceSize, Bytes.Num() - Offset)));
		}

		TestTrue(Label + TEXT("complete"), Assembler.IsComplete());
		TestFalse(Label + TEXT("no error"), Assembler.HasError());
		TestEqual(Label + TEXT("stop reason"), Assembler.GetStopReason(), FString(TEXT("tool_use")));
		TestEqual(Label + TEXT("input tokens"), Assembler.GetUsage().InputTokens, static_cast<int64>(40));
		TestEqual(Label + TEXT("output tokens come from the last event"), Assembler.GetUsage().OutputTokens, static_cast<int64>(60));
		TestEqual(Label + TEXT("cache read tokens"), Assembler.GetUsage().CacheReadTokens, static_cast<int64>(1200));

		const TArray<TSharedPtr<FJsonValue>> Content = Assembler.BuildContent();
		TestEqual(Label + TEXT("three content blocks"), Content.Num(), 3);
		if (Content.Num() == 3)
		{
			const TSharedPtr<FJsonObject> Thinking = Content[0]->AsObject();
			TestEqual(Label + TEXT("thinking type"), AgentJson::Str(Thinking, TEXT("type")), FString(TEXT("thinking")));
			TestEqual(Label + TEXT("thinking text"), AgentJson::Str(Thinking, TEXT("thinking")), FString(TEXT("The user wants an echo. I should call the tool.")));
			TestEqual(Label + TEXT("signature kept exactly"), AgentJson::Str(Thinking, TEXT("signature")), FString(TEXT("FIXTURE-SIGNATURE-not-a-real-one")));

			TestEqual(Label + TEXT("text block"), AgentJson::Str(Content[1]->AsObject(), TEXT("text")), FString(TEXT("Calling the echo tool.")));

			const TSharedPtr<FJsonObject> Tool = Content[2]->AsObject();
			TestEqual(Label + TEXT("tool id"), AgentJson::Str(Tool, TEXT("id")), FString(TEXT("toolu_fixture_01")));
			TestEqual(Label + TEXT("tool input assembled from partial JSON"), AgentJson::Str(AgentJson::Obj(Tool, TEXT("input")), TEXT("text")), FString(TEXT("from the model")));
		}

		TestEqual(Label + TEXT("one tool use"), Assembler.GetToolUses().Num(), 1);

		// thinking x2, text, tool start.
		TestEqual(Label + TEXT("events emitted"), Events.Num(), 4);
		if (Events.Num() == 4)
		{
			TestTrue(Label + TEXT("thinking first"), Events[0].Type == EAgentEventType::ThinkingDelta);
			TestTrue(Label + TEXT("then text"), Events[2].Type == EAgentEventType::TextDelta);
			TestTrue(Label + TEXT("then the tool call"), Events[3].Type == EAgentEventType::ToolCallStarted);
			TestEqual(Label + TEXT("tool name on the start event"), Events[3].ToolName, FString(TEXT("__agent_echo")));
		}
	}

	// A call whose arguments were cut off is reported but kept out of the content that goes back.
	{
		FAnthropicStreamAssembler Assembler;
		Assembler.Feed(LoadFixture(TEXT("max-tokens-mid-tool.sse")));
		TestEqual(TEXT("stop reason max_tokens"), Assembler.GetStopReason(), FString(TEXT("max_tokens")));
		TestEqual(TEXT("both calls seen"), Assembler.GetToolUses().Num(), 2);
		if (Assembler.GetToolUses().Num() == 2)
		{
			TestTrue(TEXT("complete call is usable"), Assembler.GetToolUses()[0].Input.IsValid());
			TestFalse(TEXT("cut-off call is not"), Assembler.GetToolUses()[1].Input.IsValid());
		}
		TestEqual(TEXT("cut-off call left out of the content"), Assembler.BuildContent().Num(), 2);
	}

	// An error event inside the stream.
	{
		FAnthropicStreamAssembler Assembler;
		Assembler.Feed(LoadFixture(TEXT("overloaded-midstream.sse")));
		TestTrue(TEXT("error seen"), Assembler.HasError());
		TestEqual(TEXT("error type"), Assembler.GetErrorType(), FString(TEXT("overloaded_error")));
		TestFalse(TEXT("not complete"), Assembler.IsComplete());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAgentToolLoopTest, "CursorUnreal.Agent.ToolLoop", CursorTest::Flags)
bool FCursorAgentToolLoopTest::RunTest(const FString&)
{
	FHarness H;
	H.Queue(TEXT("tool-use.sse"), 5);
	H.Queue(TEXT("after-tool.sse"), 11);

	FAgentTurnRequest Request;
	Request.Text = TEXT("  Echo something.  ");
	Request.Context.Add({ TEXT("Selection"), TEXT("Cube_3") });
	H.Backend->SendTurn(Request, H.Sink());
	TestTrue(TEXT("busy while the turn runs"), H.Backend->IsBusy());
	H.Transport->PumpAll();

	TestEqual(TEXT("turn finished normally"), H.Outcome(), FString(TEXT("done")));
	TestFalse(TEXT("idle afterwards"), H.Backend->IsBusy());
	TestEqual(TEXT("two model round trips"), H.Transport->GetRequests().Num(), 2);
	TestEqual(TEXT("two RequestStarted events"), H.Count(EAgentEventType::RequestStarted), 2);
	TestEqual(TEXT("thinking streamed"), H.Joined(EAgentEventType::ThinkingDelta), FString(TEXT("The user wants an echo. I should call the tool.")));
	TestEqual(TEXT("text streamed from both replies"), H.Joined(EAgentEventType::TextDelta), FString(TEXT("Calling the echo tool.The tool answered.")));

	TestEqual(TEXT("the tool ran once"), H.ToolOrder.Num(), 1);
	if (const FAgentEvent* Running = H.First(EAgentEventType::ToolCallRunning))
	{
		TestEqual(TEXT("running event carries the id"), Running->ToolCallId, FString(TEXT("toolu_fixture_01")));
		TestTrue(TEXT("running event carries the arguments"), Running->ArgsJson.Contains(TEXT("from the model")));
	}
	else
	{
		AddError(TEXT("no ToolCallRunning event"));
	}
	if (const FAgentEvent* Finished = H.First(EAgentEventType::ToolCallFinished))
	{
		TestEqual(TEXT("tool result reported"), Finished->Text, FString(TEXT("from the model")));
		TestFalse(TEXT("tool succeeded"), Finished->bIsError);
	}
	else
	{
		AddError(TEXT("no ToolCallFinished event"));
	}

	// Usage is summed over both round trips.
	const FAgentUsage& Usage = H.Events.Last().Usage;
	TestEqual(TEXT("input tokens summed"), Usage.InputTokens, static_cast<int64>(52));
	TestEqual(TEXT("output tokens summed"), Usage.OutputTokens, static_cast<int64>(68));
	TestEqual(TEXT("cache reads summed"), Usage.CacheReadTokens, static_cast<int64>(2440));
	TestEqual(TEXT("cache writes summed"), Usage.CacheWriteTokens, static_cast<int64>(90));

	// --- What was sent -------------------------------------------------------------------------
	const FAgentHttpRequest& First = H.Transport->GetRequests()[0];
	TestEqual(TEXT("endpoint"), First.Url, FString(TEXT("https://fixture.invalid/v1/messages")));
	TestEqual(TEXT("verb"), First.Verb, FString(TEXT("POST")));
	TestTrue(TEXT("streamed"), First.bStream);
	TestEqual(TEXT("key header"), First.Headers.FindRef(TEXT("x-api-key")), FString(FakeKey));
	TestEqual(TEXT("version header"), First.Headers.FindRef(TEXT("anthropic-version")), FString(TEXT("2026-10-21")));
	TestFalse(TEXT("the key is not in the body"), First.Body.Contains(FakeKey));

	const TSharedPtr<FJsonObject> Body = H.RequestBody(0);
	TestTrue(TEXT("first body parses"), Body.IsValid());
	TestEqual(TEXT("model"), AgentJson::Str(Body, TEXT("model")), FString(TEXT("claude-fixture-1")));
	TestTrue(TEXT("stream flag"), AgentJson::Bool(Body, TEXT("stream")));
	TestEqual(TEXT("max_tokens from settings when the model's ceiling is unknown"), static_cast<int32>(AgentJson::Num(Body, TEXT("max_tokens"))), 32000);
	TestEqual(TEXT("conversation cache point"), AgentJson::Str(AgentJson::Obj(Body, TEXT("cache_control")), TEXT("type")), FString(TEXT("ephemeral")));
	// API version 2026-10-21 rejects these on every model.
	TestFalse(TEXT("no temperature"), Body->HasField(TEXT("temperature")));
	TestFalse(TEXT("no top_p"), Body->HasField(TEXT("top_p")));
	TestFalse(TEXT("no top_k"), Body->HasField(TEXT("top_k")));
	TestFalse(TEXT("no forced tool choice"), Body->HasField(TEXT("tool_choice")));
	// Nothing is known about this model yet, so neither is sent.
	TestFalse(TEXT("no thinking field for a model of unknown capabilities"), Body->HasField(TEXT("thinking")));
	TestFalse(TEXT("no output_config for a model of unknown capabilities"), Body->HasField(TEXT("output_config")));

	const TArray<TSharedPtr<FJsonValue>>* System = AgentJson::Arr(Body, TEXT("system"));
	TestTrue(TEXT("system is one block"), System && System->Num() == 1);
	if (System && System->Num() == 1)
	{
		const TSharedPtr<FJsonObject> Block = (*System)[0]->AsObject();
		const FString Text = AgentJson::Str(Block, TEXT("text"));
		TestTrue(TEXT("system prompt names the engine"), Text.Contains(TEXT("Unreal Engine 5.")));
		TestTrue(TEXT("system prompt carries the project's own instructions"), Text.Contains(TEXT("Fixture project rule.")));
		TestEqual(TEXT("system block is a cache point"), AgentJson::Str(AgentJson::Obj(Block, TEXT("cache_control")), TEXT("type")), FString(TEXT("ephemeral")));
	}

	bool bHasEcho = false;
	bool bToolsWellFormed = true;
	if (const TArray<TSharedPtr<FJsonValue>>* Tools = AgentJson::Arr(Body, TEXT("tools")))
	{
		for (const TSharedPtr<FJsonValue>& Value : *Tools)
		{
			const TSharedPtr<FJsonObject> Tool = Value->AsObject();
			bHasEcho |= AgentJson::Str(Tool, TEXT("name")) == TEXT("__agent_echo");
			bToolsWellFormed &= !AgentJson::Str(Tool, TEXT("description")).IsEmpty()
				&& AgentJson::Str(AgentJson::Obj(Tool, TEXT("input_schema")), TEXT("type")) == TEXT("object");
		}
	}
	TestTrue(TEXT("registry tools are offered"), bHasEcho);
	TestTrue(TEXT("every tool has a description and an object schema"), bToolsWellFormed);

	TestEqual(TEXT("first request has one message"), H.NumMessages(0), 1);
	TestEqual(TEXT("it is the user's"), AgentJson::Str(H.Message(0, 0), TEXT("role")), FString(TEXT("user")));
	TestEqual(TEXT("context block and the message"), H.NumBlocks(0, 0), 2);
	TestTrue(TEXT("context comes first"), AgentJson::Str(H.Block(0, 0, 0), TEXT("text")).Contains(TEXT("Cube_3")));
	TestEqual(TEXT("the message is last, trimmed"), AgentJson::Str(H.Block(0, 0, 1), TEXT("text")), FString(TEXT("Echo something.")));

	// The second request returns the assistant's reply unchanged, then the tool result.
	TestEqual(TEXT("second request has three messages"), H.NumMessages(1), 3);
	TestEqual(TEXT("assistant message"), AgentJson::Str(H.Message(1, 1), TEXT("role")), FString(TEXT("assistant")));
	TestEqual(TEXT("assistant blocks: thinking, text, tool_use"), H.NumBlocks(1, 1), 3);
	TestEqual(TEXT("thinking goes back first"), AgentJson::Str(H.Block(1, 1, 0), TEXT("type")), FString(TEXT("thinking")));
	TestEqual(TEXT("with its signature untouched"), AgentJson::Str(H.Block(1, 1, 0), TEXT("signature")), FString(TEXT("FIXTURE-SIGNATURE-not-a-real-one")));
	TestEqual(TEXT("and its text untouched"), AgentJson::Str(H.Block(1, 1, 0), TEXT("thinking")), FString(TEXT("The user wants an echo. I should call the tool.")));
	TestEqual(TEXT("tool_use goes back with parsed input"), AgentJson::Str(AgentJson::Obj(H.Block(1, 1, 2), TEXT("input")), TEXT("text")), FString(TEXT("from the model")));

	TestEqual(TEXT("tool results travel as a user message"), AgentJson::Str(H.Message(1, 2), TEXT("role")), FString(TEXT("user")));
	TestEqual(TEXT("one result"), H.NumBlocks(1, 2), 1);
	TestEqual(TEXT("result type"), AgentJson::Str(H.Block(1, 2, 0), TEXT("type")), FString(TEXT("tool_result")));
	TestEqual(TEXT("result names its call"), AgentJson::Str(H.Block(1, 2, 0), TEXT("tool_use_id")), FString(TEXT("toolu_fixture_01")));
	TestEqual(TEXT("result content"), AgentJson::Str(H.Block(1, 2, 0), TEXT("content")), FString(TEXT("from the model")));
	TestFalse(TEXT("a successful result is not flagged as an error"), H.Block(1, 2, 0).IsValid() && H.Block(1, 2, 0)->HasField(TEXT("is_error")));

	// The system prompt and tools must be byte-identical between requests, or the cache and the model's earlier thinking are lost.
	const TSharedPtr<FJsonObject> Second = H.RequestBody(1);
	TestEqual(TEXT("system unchanged between round trips"),
		CursorJson::ToString(Body->GetArrayField(TEXT("system"))), CursorJson::ToString(Second->GetArrayField(TEXT("system"))));
	TestEqual(TEXT("tools unchanged between round trips"),
		CursorJson::ToString(Body->GetArrayField(TEXT("tools"))), CursorJson::ToString(Second->GetArrayField(TEXT("tools"))));

	TestEqual(TEXT("conversation holds user, assistant, results, assistant"), H.Backend->NumHistoryMessages(), 4);
	TestFalse(TEXT("nothing to retry after a finished turn"), H.Backend->CanRetry());

	// A second turn goes on from the same conversation.
	H.Events.Reset();
	H.Queue(TEXT("text-only.sse"));
	H.Send(TEXT("And again."));
	TestEqual(TEXT("second turn finished"), H.Outcome(), FString(TEXT("done")));
	TestEqual(TEXT("third request carries the whole conversation"), H.NumMessages(2), 5);

	// Two calls in one reply run in order, and both results go back in one message.
	H.Events.Reset();
	H.ToolOrder.Reset();
	H.Backend->ResetConversation();
	H.Queue(TEXT("two-tools.sse"));
	H.Queue(TEXT("after-tool.sse"));
	H.Send(TEXT("Two calls."));
	TestEqual(TEXT("two-call turn finished"), H.Outcome(), FString(TEXT("done")));
	TestTrue(TEXT("calls ran in the order the model gave them"), H.ToolOrder.Num() == 2 && H.ToolOrder[0] == TEXT("first") && H.ToolOrder[1] == TEXT("second"));
	const int32 Follow = H.Transport->GetRequests().Num() - 1;
	TestEqual(TEXT("after a reset the conversation starts over"), H.NumMessages(Follow), 3);
	TestEqual(TEXT("both results in one message"), H.NumBlocks(Follow, 2), 2);
	TestEqual(TEXT("first result"), AgentJson::Str(H.Block(Follow, 2, 0), TEXT("tool_use_id")), FString(TEXT("toolu_fixture_a")));
	TestTrue(TEXT("the failed call is flagged"), AgentJson::Bool(H.Block(Follow, 2, 1), TEXT("is_error")));
	TestEqual(TEXT("the failure text reaches the model"), AgentJson::Str(H.Block(Follow, 2, 1), TEXT("content")), FString(TEXT("asked to fail")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAgentRequestShapeTest, "CursorUnreal.Agent.RequestShape", CursorTest::Flags)
bool FCursorAgentRequestShapeTest::RunTest(const FString&)
{
	// Reading the model list.
	{
		FString Json;
		FFileHelper::LoadFileToString(Json, *FixturePath(TEXT("models.json")));
		TArray<FAgentModelInfo> Models;
		FString Cursor;
		TestTrue(TEXT("model list parses"), FAnthropicBackend::ParseModelList(Json, Models, Cursor));
		TestEqual(TEXT("retired models are left out"), Models.Num(), 2);
		TestTrue(TEXT("no next page"), Cursor.IsEmpty());
		if (Models.Num() == 2)
		{
			TestEqual(TEXT("display name"), Models[0].DisplayName, FString(TEXT("Claude Fixture 1")));
			TestEqual(TEXT("line"), Models[0].Line, FString(TEXT("fable")));
			TestTrue(TEXT("adaptive thinking"), Models[0].bAdaptiveThinking);
			TestEqual(TEXT("output ceiling"), Models[0].MaxOutputTokens, 20000);
			TestEqual(TEXT("all five effort levels"), Models[0].EffortLevels.Num(), 5);
			TestTrue(TEXT("deprecated flag"), Models[1].bDeprecated);
			TestFalse(TEXT("no adaptive thinking on the small model"), Models[1].bAdaptiveThinking);
			TestEqual(TEXT("no effort levels on the small model"), Models[1].EffortLevels.Num(), 0);
		}
		TArray<FAgentModelInfo> None;
		TestFalse(TEXT("something that is not a model list is refused"), FAnthropicBackend::ParseModelList(TEXT("{\"error\":1}"), None, Cursor));
	}

	FHarness H;
	UCursorUnrealSettings* Settings = UCursorUnrealSettings::GetMutable();

	// Fetch the list through the backend.
	H.Queue(TEXT("models.json"));
	bool bModelsOk = false;
	int32 NumModels = 0;
	H.Backend->RefreshModels([&](bool bOk, const TArray<FAgentModelInfo>& Models, const FString&)
	{
		bModelsOk = bOk;
		NumModels = Models.Num();
	});
	H.Transport->PumpAll();
	TestTrue(TEXT("model refresh succeeded"), bModelsOk);
	TestEqual(TEXT("two models"), NumModels, 2);
	TestEqual(TEXT("cached afterwards"), H.Backend->GetCachedModels().Num(), 2);
	const FAgentHttpRequest& ListRequest = H.Transport->GetRequests()[0];
	TestEqual(TEXT("list verb"), ListRequest.Verb, FString(TEXT("GET")));
	TestEqual(TEXT("list url"), ListRequest.Url, FString(TEXT("https://fixture.invalid/v1/models?limit=1000")));
	TestFalse(TEXT("list is not streamed"), ListRequest.bStream);

	// A model that takes adaptive thinking and every effort level.
	Settings->Effort = ECursorEffort::XHigh;
	Settings->MaxTokens = 50000;
	H.Queue(TEXT("text-only.sse"));
	H.Send(TEXT("Hello."));
	TestEqual(TEXT("turn finished"), H.Outcome(), FString(TEXT("done")));
	{
		const TSharedPtr<FJsonObject> Body = H.RequestBody(1);
		const TSharedPtr<FJsonObject> Thinking = AgentJson::Obj(Body, TEXT("thinking"));
		TestEqual(TEXT("adaptive thinking requested"), AgentJson::Str(Thinking, TEXT("type")), FString(TEXT("adaptive")));
		TestEqual(TEXT("thinking summary requested"), AgentJson::Str(Thinking, TEXT("display")), FString(TEXT("summarized")));
		TestFalse(TEXT("no thinking budget: 4.7+ models reject it"), Thinking.IsValid() && Thinking->HasField(TEXT("budget_tokens")));
		TestEqual(TEXT("effort sent"), AgentJson::Str(AgentJson::Obj(Body, TEXT("output_config")), TEXT("effort")), FString(TEXT("xhigh")));
		TestEqual(TEXT("max_tokens lowered to the model's ceiling"), static_cast<int32>(AgentJson::Num(Body, TEXT("max_tokens"))), 20000);
		TestEqual(TEXT("no notice needed"), H.Count(EAgentEventType::Notice), 0);
	}

	// Thinking summaries switched off.
	Settings->bShowThinking = false;
	H.Events.Reset();
	H.Backend->ResetConversation();
	H.Queue(TEXT("text-only.sse"));
	H.Send(TEXT("Hello."));
	TestEqual(TEXT("display omitted when summaries are off"),
		AgentJson::Str(AgentJson::Obj(H.RequestBody(2), TEXT("thinking")), TEXT("display")), FString(TEXT("omitted")));

	// A model with neither: nothing is sent, and the user is told why their effort setting did nothing.
	Settings->AnthropicModel = TEXT("claude-fixture-small");
	H.Events.Reset();
	H.Backend->ResetConversation();
	H.Queue(TEXT("text-only.sse"));
	H.Send(TEXT("Hello."));
	{
		const TSharedPtr<FJsonObject> Body = H.RequestBody(3);
		TestFalse(TEXT("no thinking field for a model without adaptive thinking"), Body->HasField(TEXT("thinking")));
		TestFalse(TEXT("no effort for a model without it"), Body->HasField(TEXT("output_config")));
		TestEqual(TEXT("settings value used when under the ceiling"), static_cast<int32>(AgentJson::Num(Body, TEXT("max_tokens"))), 50000);
		TestEqual(TEXT("one notice about effort"), H.Count(EAgentEventType::Notice), 1);
	}

	// The endpoint may be written with or without /v1 and a trailing slash.
	Settings->AnthropicBaseUrl = TEXT("http://localhost:8080/v1/");
	H.Events.Reset();
	H.Backend->ResetConversation();
	H.Queue(TEXT("text-only.sse"));
	H.Send(TEXT("Hello."));
	TestEqual(TEXT("endpoint normalised"), H.Transport->GetRequests().Last().Url, FString(TEXT("http://localhost:8080/v1/messages")));

	// Things that stop a turn before anything is sent.
	const int32 SentSoFar = H.Transport->GetRequests().Num();
	FText Why;

	Settings->AnthropicBaseUrl = TEXT("http://example.com");
	TestFalse(TEXT("plain http to a remote host is refused"), H.Backend->CheckReady(Why));
	Settings->AnthropicBaseUrl = TEXT("https://fixture.invalid");

	Settings->AnthropicModel = TEXT("");
	TestFalse(TEXT("no model, not ready"), H.Backend->CheckReady(Why));
	Settings->AnthropicModel = TEXT("claude-fixture-1");

	H.Key = TEXT("");
	TestFalse(TEXT("no key, not ready"), H.Backend->CheckReady(Why));
	H.Events.Reset();
	H.Send(TEXT("Hello."));
	TestEqual(TEXT("a turn without a key ends in an error"), H.Outcome(), FString(TEXT("error")));
	TestEqual(TEXT("and sends nothing"), H.Transport->GetRequests().Num(), SentSoFar);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAgentFailuresTest, "CursorUnreal.Agent.Failures", CursorTest::Flags)
bool FCursorAgentFailuresTest::RunTest(const FString&)
{
	// The backend logs a warning for each failed request; only errors fail a test.

	// 401: not retried, and the message says what to do.
	{
		FHarness H;
		H.Queue(TEXT("error-401.json"), 0, 401);
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("401 ends in an error"), H.Outcome(), FString(TEXT("error")));
		TestEqual(TEXT("401 is not retried"), H.Transport->GetRequests().Num(), 1);
		const FString Text = H.Events.Last().Text;
		TestTrue(TEXT("says the key was rejected"), Text.Contains(TEXT("401")) && Text.Contains(TEXT("API key")));
		TestTrue(TEXT("includes what the API said"), Text.Contains(TEXT("invalid x-api-key")));
		TestFalse(TEXT("never shows the key"), Text.Contains(FakeKey));
		TestTrue(TEXT("the message can be sent again once fixed"), H.Backend->CanRetry());

		H.Events.Reset();
		H.Queue(TEXT("text-only.sse"));
		H.Backend->Retry(H.Sink());
		H.Transport->PumpAll();
		TestEqual(TEXT("retry succeeds"), H.Outcome(), FString(TEXT("done")));
		TestEqual(TEXT("retry did not add a second copy of the message"), H.NumBlocks(1, 0), 1);
	}

	// 400: not retried.
	{
		FHarness H;
		H.Queue(TEXT("error-400.json"), 0, 400);
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("400 ends in an error"), H.Outcome(), FString(TEXT("error")));
		TestEqual(TEXT("400 is not retried"), H.Transport->GetRequests().Num(), 1);
		TestTrue(TEXT("400 shows the API's explanation"), H.Events.Last().Text.Contains(TEXT("fixture validation message")));
	}

	// 429 with retry-after, then success.
	{
		FHarness H;
		FAgentFixtureResponse Limited;
		Limited.Status = 429;
		Limited.Headers.Add(TEXT("Retry-After"), TEXT("3"));
		Limited.Headers.Add(TEXT("request-id"), TEXT("req_fixture_429"));
		Limited.Body = LoadFixture(TEXT("error-429.json"));
		H.Transport->Enqueue(MoveTemp(Limited));
		H.Queue(TEXT("text-only.sse"));
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("429 then success finishes"), H.Outcome(), FString(TEXT("done")));
		TestEqual(TEXT("two requests"), H.Transport->GetRequests().Num(), 2);
		TestTrue(TEXT("waited as long as the service asked"), H.RetryDelays.Num() == 1 && FMath::IsNearlyEqual(H.RetryDelays[0], 3.0f));
		TestEqual(TEXT("the user is told about the retry"), H.Count(EAgentEventType::Notice), 1);
		TestTrue(TEXT("the notice names the request"), H.Joined(EAgentEventType::Notice).Contains(TEXT("req_fixture_429")));
		TestEqual(TEXT("retried request is identical"), H.Transport->GetRequests()[0].Body, H.Transport->GetRequests()[1].Body);
	}

	// A service asking for a long wait is reported instead of waited out.
	{
		FHarness H;
		FAgentFixtureResponse Limited;
		Limited.Status = 429;
		Limited.Headers.Add(TEXT("retry-after"), TEXT("600"));
		Limited.Body = LoadFixture(TEXT("error-429.json"));
		H.Transport->Enqueue(MoveTemp(Limited));
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("long retry-after ends in an error"), H.Outcome(), FString(TEXT("error")));
		TestEqual(TEXT("no retry was scheduled"), H.RetryDelays.Num(), 0);
		TestTrue(TEXT("says how long"), H.Events.Last().Text.Contains(TEXT("600")));
	}

	// No connection at all: retried, then given up on.
	{
		FHarness H;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			FAgentFixtureResponse Dead;
			Dead.bConnectionFailure = true;
			H.Transport->Enqueue(MoveTemp(Dead));
		}
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("no connection ends in an error"), H.Outcome(), FString(TEXT("error")));
		TestEqual(TEXT("one try and two retries"), H.Transport->GetRequests().Num(), 3);
		TestTrue(TEXT("says it gave up"), H.Events.Last().Text.Contains(TEXT("Gave up after 3 attempts")));
		TestTrue(TEXT("backoff grows"), H.RetryDelays.Num() == 2 && H.RetryDelays[1] > H.RetryDelays[0]);
		TestTrue(TEXT("the message can be sent again"), H.Backend->CanRetry());
	}

	// Overloaded part-way through a reply: what was shown is taken back and the request sent again.
	{
		FHarness H;
		H.Queue(TEXT("overloaded-midstream.sse"), 9);
		H.Queue(TEXT("text-only.sse"), 9);
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("overload then success finishes"), H.Outcome(), FString(TEXT("done")));
		TestEqual(TEXT("one rewind"), H.Count(EAgentEventType::Rewind), 1);
		TestEqual(TEXT("conversation holds only the good reply"), H.Backend->NumHistoryMessages(), 2);

		int32 RewindAt = INDEX_NONE;
		int32 DiscardedAt = INDEX_NONE;
		for (int32 Index = 0; Index < H.Events.Num(); ++Index)
		{
			if (H.Events[Index].Type == EAgentEventType::Rewind)
			{
				RewindAt = Index;
			}
			if (H.Events[Index].Text.Contains(TEXT("thrown away")))
			{
				DiscardedAt = Index;
			}
		}
		TestTrue(TEXT("the rewind comes after the text it takes back"), DiscardedAt != INDEX_NONE && RewindAt > DiscardedAt);
	}

	// The connection drops before message_stop.
	{
		FHarness H;
		FAgentFixtureResponse Dropped;
		Dropped.Body = LoadFixture(TEXT("cut-off.sse"));
		Dropped.bDropAfterBody = true;
		H.Transport->Enqueue(MoveTemp(Dropped));
		H.Queue(TEXT("text-only.sse"));
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("dropped stream is retried and finishes"), H.Outcome(), FString(TEXT("done")));
		TestEqual(TEXT("one rewind"), H.Count(EAgentEventType::Rewind), 1);
		TestEqual(TEXT("half a reply is never kept"), H.Backend->NumHistoryMessages(), 2);
	}

	// A 200 that is not an event stream at all (a proxy's HTML page, say).
	{
		FHarness H;
		H.Transport->EnqueueText(TEXT("<html><body>Welcome to the proxy</body></html>"));
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("non-stream reply ends in an error"), H.Outcome(), FString(TEXT("error")));
		TestEqual(TEXT("and is not retried"), H.Transport->GetRequests().Num(), 1);
		TestTrue(TEXT("points at the endpoint setting"), H.Events.Last().Text.Contains(TEXT("endpoint")));
	}

	// Cut off by max_tokens in the middle of a tool call.
	{
		FHarness H;
		H.Queue(TEXT("max-tokens-mid-tool.sse"));
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("truncated outcome"), H.Outcome(), FString(TEXT("truncated")));
		TestEqual(TEXT("no tool ran: the reply may be missing calls that belonged with it"), H.ToolOrder.Num(), 0);
		TestEqual(TEXT("no further request"), H.Transport->GetRequests().Num(), 1);
		TestEqual(TEXT("the user is told"), H.Count(EAgentEventType::Notice), 1);
		// The complete call is in the conversation, so it must have an answer or the next request would be rejected.
		TestEqual(TEXT("conversation: user, assistant, results"), H.Backend->NumHistoryMessages(), 3);

		H.Events.Reset();
		H.Queue(TEXT("text-only.sse"));
		H.Send(TEXT("Carry on."));
		TestEqual(TEXT("the conversation is still usable"), H.Outcome(), FString(TEXT("done")));
		TestEqual(TEXT("assistant kept its text and the complete call only"), H.NumBlocks(1, 1), 2);
		TestEqual(TEXT("the complete call was answered"), AgentJson::Str(H.Block(1, 2, 0), TEXT("tool_use_id")), FString(TEXT("toolu_fixture_done")));
		TestTrue(TEXT("as not run"), AgentJson::Bool(H.Block(1, 2, 0), TEXT("is_error")));
		TestEqual(TEXT("the new message joins the results message, keeping roles alternating"), H.NumBlocks(1, 2), 2);
		TestEqual(TEXT("three messages"), H.NumMessages(1), 3);
	}

	// Refusal.
	{
		FHarness H;
		H.Queue(TEXT("refusal.sse"));
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("refused outcome"), H.Outcome(), FString(TEXT("refused")));
		TestTrue(TEXT("the notice carries the category"), H.Joined(EAgentEventType::Notice).Contains(TEXT("fixture_category")));
		TestEqual(TEXT("the refused reply is not kept"), H.Backend->NumHistoryMessages(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAgentCancelTest, "CursorUnreal.Agent.Cancel", CursorTest::Flags)
bool FCursorAgentCancelTest::RunTest(const FString&)
{
	// Stop while the reply is streaming.
	{
		FHarness H;
		H.Queue(TEXT("text-only.sse"), 16);
		FAgentTurnRequest Request;
		Request.Text = TEXT("First.");
		H.Backend->SendTurn(Request, [&H](const FAgentEvent& Event)
		{
			H.Events.Add(Event);
			if (Event.Type == EAgentEventType::TextDelta)
			{
				H.Backend->Cancel();
			}
		});
		H.Transport->PumpAll();
		TestEqual(TEXT("cancelled outcome"), H.Outcome(), FString(TEXT("cancelled")));
		TestEqual(TEXT("exactly one final event"), H.Count(EAgentEventType::Done), 1);
		TestEqual(TEXT("nothing after the stop"), H.Count(EAgentEventType::TextDelta), 1);
		TestFalse(TEXT("idle"), H.Backend->IsBusy());
		TestEqual(TEXT("the half reply is not kept"), H.Backend->NumHistoryMessages(), 1);

		// The next message joins the unanswered one, so the conversation still alternates.
		H.Events.Reset();
		H.Queue(TEXT("text-only.sse"));
		H.Send(TEXT("Second."));
		TestEqual(TEXT("next turn finishes"), H.Outcome(), FString(TEXT("done")));
		TestEqual(TEXT("one user message"), H.NumMessages(1), 1);
		TestEqual(TEXT("holding both texts"), H.NumBlocks(1, 0), 2);
	}

	// Stop while tools are running: the running one finishes, the rest are skipped, and every call still gets an answer.
	{
		FHarness H;
		H.Queue(TEXT("two-tools.sse"));
		FAgentTurnRequest Request;
		Request.Text = TEXT("Two calls.");
		H.Backend->SendTurn(Request, [&H](const FAgentEvent& Event)
		{
			H.Events.Add(Event);
			if (Event.Type == EAgentEventType::ToolCallRunning)
			{
				H.Backend->Cancel();
			}
		});
		H.Transport->PumpAll();
		TestEqual(TEXT("cancelled outcome"), H.Outcome(), FString(TEXT("cancelled")));
		TestTrue(TEXT("only the first tool ran"), H.ToolOrder.Num() == 1 && H.ToolOrder[0] == TEXT("first"));
		TestEqual(TEXT("both calls reported as finished"), H.Count(EAgentEventType::ToolCallFinished), 2);
		TestEqual(TEXT("no further request"), H.Transport->GetRequests().Num(), 1);
		TestEqual(TEXT("conversation: user, assistant, results"), H.Backend->NumHistoryMessages(), 3);

		H.Events.Reset();
		H.Queue(TEXT("text-only.sse"));
		H.Send(TEXT("Carry on."));
		TestEqual(TEXT("the conversation is still usable"), H.Outcome(), FString(TEXT("done")));
		TestEqual(TEXT("both results and the new message"), H.NumBlocks(1, 2), 3);
		TestFalse(TEXT("the call that ran has its real result"), AgentJson::Bool(H.Block(1, 2, 0), TEXT("is_error")));
		TestTrue(TEXT("the skipped call says it was not run"), AgentJson::Str(H.Block(1, 2, 1), TEXT("content")).Contains(TEXT("Not run")));
	}

	// The round-trip limit.
	{
		FHarness H;
		UCursorUnrealSettings::GetMutable()->MaxToolIterations = 1;
		H.Queue(TEXT("tool-use.sse"));
		H.Send(TEXT("Hello."));
		TestEqual(TEXT("limit outcome"), H.Outcome(), FString(TEXT("max_iterations")));
		TestEqual(TEXT("the tool still ran"), H.ToolOrder.Num(), 1);
		TestEqual(TEXT("no second request"), H.Transport->GetRequests().Num(), 1);
		TestEqual(TEXT("the user is told"), H.Count(EAgentEventType::Notice), 1);
	}

	// Starting over in the middle of a turn.
	{
		FHarness H;
		H.Queue(TEXT("text-only.sse"), 16);
		FAgentTurnRequest Request;
		Request.Text = TEXT("Hello.");
		H.Backend->SendTurn(Request, H.Sink());
		const FString SessionBefore = H.Backend->GetSessionId();
		H.Backend->ResetConversation();
		H.Transport->PumpAll();
		TestEqual(TEXT("the turn ends as cancelled"), H.Outcome(), FString(TEXT("cancelled")));
		TestEqual(TEXT("nothing arrives after the reset"), H.Count(EAgentEventType::TextDelta), 0);
		TestEqual(TEXT("conversation is empty"), H.Backend->NumHistoryMessages(), 0);
		TestNotEqual(TEXT("a new session id"), H.Backend->GetSessionId(), SessionBefore);
	}

	// A second turn cannot start while one is running.
	{
		FHarness H;
		H.Queue(TEXT("text-only.sse"));
		FAgentTurnRequest Request;
		Request.Text = TEXT("Hello.");
		H.Backend->SendTurn(Request, H.Sink());
		TArray<FAgentEvent> Second;
		H.Backend->SendTurn(Request, [&Second](const FAgentEvent& Event) { Second.Add(Event); });
		TestTrue(TEXT("the second turn is refused at once"), Second.Num() == 1 && Second[0].Type == EAgentEventType::Error);
		H.Transport->PumpAll();
		TestEqual(TEXT("the first turn is unaffected"), H.Outcome(), FString(TEXT("done")));
		TestEqual(TEXT("one request"), H.Transport->GetRequests().Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAgentSessionTest, "CursorUnreal.Agent.Session", CursorTest::Flags)
bool FCursorAgentSessionTest::RunTest(const FString&)
{
	FHarness H;
	const TSharedRef<FAgentSession> Session = MakeShared<FAgentSession>(H.Backend.ToSharedRef());
	Session->bWriteToDisk = false;

	int32 StateChanges = 0;
	Session->OnStateChanged.AddLambda([&StateChanges]() { ++StateChanges; });
	const int32 SessionsBefore = FCursorActivity::Get().NumActiveSessions();

	// Not ready: nothing is added to the chat.
	FText Why;
	H.Key = TEXT("");
	TestFalse(TEXT("send refused without a key"), Session->Send(TEXT("Hello."), {}, Why));
	TestFalse(TEXT("with a reason"), Why.IsEmpty());
	TestEqual(TEXT("chat still empty"), Session->GetItems().Num(), 0);
	H.Key = FakeKey;
	TestFalse(TEXT("empty message refused"), Session->Send(TEXT("   "), {}, Why));

	// A turn with thinking, text and a tool call.
	H.Queue(TEXT("tool-use.sse"), 13);
	H.Queue(TEXT("after-tool.sse"), 13);
	TestTrue(TEXT("send accepted"), Session->Send(TEXT("Echo something."), {}, Why));
	TestTrue(TEXT("busy"), Session->IsBusy());
	TestEqual(TEXT("the editor is kept awake while the agent works"), FCursorActivity::Get().NumActiveSessions(), SessionsBefore + 1);
	H.Transport->PumpAll();
	TestFalse(TEXT("idle"), Session->IsBusy());
	TestEqual(TEXT("state changed at start and end"), StateChanges, 2);

	const TArray<TSharedRef<FAgentItem>>& Items = Session->GetItems();
	TestEqual(TEXT("user, thinking, text, tool, text"), Items.Num(), 5);
	if (Items.Num() == 5)
	{
		TestTrue(TEXT("user first"), Items[0]->Kind == EAgentItemKind::User);
		TestEqual(TEXT("user text"), Items[0]->Text, FString(TEXT("Echo something.")));
		TestTrue(TEXT("thinking"), Items[1]->Kind == EAgentItemKind::Thinking);
		TestEqual(TEXT("thinking pieces joined into one item"), Items[1]->Text, FString(TEXT("The user wants an echo. I should call the tool.")));
		TestTrue(TEXT("assistant text"), Items[2]->Kind == EAgentItemKind::Assistant);
		TestTrue(TEXT("tool"), Items[3]->Kind == EAgentItemKind::Tool);
		TestEqual(TEXT("tool name"), Items[3]->ToolName, FString(TEXT("__agent_echo")));
		TestTrue(TEXT("tool finished"), Items[3]->ToolState == EAgentToolState::Finished);
		TestEqual(TEXT("tool result"), Items[3]->Text, FString(TEXT("from the model")));
		TestTrue(TEXT("tool arguments"), Items[3]->ArgsJson.Contains(TEXT("from the model")));
		TestEqual(TEXT("closing text is its own item"), Items[4]->Text, FString(TEXT("The tool answered.")));
	}

	const FCursorTurnPerf& Turn = Session->GetLastTurn();
	TestEqual(TEXT("provider recorded"), Turn.Provider, FString(TEXT("anthropic")));
	TestEqual(TEXT("model recorded"), Turn.Model, FString(TEXT("claude-fixture-1")));
	TestEqual(TEXT("outcome recorded"), Turn.Outcome, FString(TEXT("done")));
	TestEqual(TEXT("round trips recorded"), Turn.RoundTrips, 2);
	TestEqual(TEXT("tool calls recorded"), Turn.ToolCalls, 1);
	TestEqual(TEXT("output tokens recorded"), Turn.OutputTokens, static_cast<int64>(68));
	TestTrue(TEXT("time to first output recorded"), Turn.TimeToFirstTokenMs >= 0.0);
	TestEqual(TEXT("chat usage"), Session->GetUsage().OutputTokens, static_cast<int64>(68));

	// A retried request takes back what its first attempt showed, and nothing from earlier turns.
	H.Queue(TEXT("overloaded-midstream.sse"), 13);
	H.Queue(TEXT("text-only.sse"), 13);
	int32 Resets = 0;
	Session->OnItemsReset.AddLambda([&Resets]() { ++Resets; });
	TestTrue(TEXT("second send accepted"), Session->Send(TEXT("Again."), {}, Why));
	H.Transport->PumpAll();
	TestEqual(TEXT("list redrawn once"), Resets, 1);
	bool bDiscardedTextShown = false;
	for (const TSharedRef<FAgentItem>& Item : Session->GetItems())
	{
		bDiscardedTextShown |= Item->Text.Contains(TEXT("thrown away"));
	}
	TestFalse(TEXT("discarded text is gone"), bDiscardedTextShown);
	// 5 from the first turn, then user, retry notice, reply.
	TestEqual(TEXT("earlier turn untouched"), Session->GetItems().Num(), 8);
	if (Session->GetItems().Num() == 8)
	{
		TestTrue(TEXT("notice about the retry"), Session->GetItems()[6]->Kind == EAgentItemKind::Notice);
		TestTrue(TEXT("then the reply"), Session->GetItems()[7]->Kind == EAgentItemKind::Assistant);
	}
	TestEqual(TEXT("chat usage adds up across turns"), Session->GetUsage().OutputTokens, static_cast<int64>(83));

	// An error shows in the chat and can be retried.
	H.Queue(TEXT("error-401.json"), 0, 401);
	Session->Send(TEXT("Third."), {}, Why);
	H.Transport->PumpAll();
	TestTrue(TEXT("error item added"), Session->GetItems().Last()->Kind == EAgentItemKind::Error);
	TestEqual(TEXT("error outcome recorded"), Session->GetLastTurn().Outcome, FString(TEXT("error")));
	TestTrue(TEXT("can retry"), Session->CanRetry());
	H.Queue(TEXT("text-only.sse"));
	TestTrue(TEXT("retry accepted"), Session->Retry(Why));
	H.Transport->PumpAll();
	TestTrue(TEXT("retry produced a reply"), Session->GetItems().Last()->Kind == EAgentItemKind::Assistant);

	// Stop.
	H.Queue(TEXT("text-only.sse"), 16);
	Session->Send(TEXT("Fourth."), {}, Why);
	Session->Stop();
	H.Transport->PumpAll();
	TestFalse(TEXT("idle after stop"), Session->IsBusy());
	TestEqual(TEXT("stop outcome recorded"), Session->GetLastTurn().Outcome, FString(TEXT("cancelled")));
	TestTrue(TEXT("the chat says it stopped"), Session->GetItems().Last()->Kind == EAgentItemKind::Notice);

	// New chat.
	Session->NewChat();
	TestEqual(TEXT("chat cleared"), Session->GetItems().Num(), 0);
	TestEqual(TEXT("conversation cleared"), H.Backend->NumHistoryMessages(), 0);
	TestEqual(TEXT("usage cleared"), Session->GetUsage().OutputTokens, static_cast<int64>(0));

	TestEqual(TEXT("activity released once every turn has ended"), FCursorActivity::Get().NumActiveSessions(), SessionsBefore);
	TestTrue(TEXT("nothing was written to disk"), Session->GetTranscriptFile().IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
