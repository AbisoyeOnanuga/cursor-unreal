#if WITH_DEV_AUTOMATION_TESTS

#include "CursorJson.h"
#include "CursorMcpProtocol.h"
#include "CursorMcpServer.h"
#include "CursorTool.h"
#include "CursorToolRegistry.h"
#include "Misc/AutomationTest.h"

namespace CursorMcpTests
{
	const TCHAR* TestPack = TEXT("__mcptest");
	constexpr auto Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter;

	struct FScopedTools
	{
		FScopedTools()
		{
			FCursorToolBuilder(TEXT("__mcp_echo"), TestPack)
				.Describe(TEXT("Echo for tests."))
				.Schema(FCursorSchema().Str(TEXT("text"), TEXT("Echoed back."), true).Bool(TEXT("fail"), TEXT("Return an error.")))
				.Run([](const FCursorArgs& Args, const FCursorToolContext& Ctx)
				{
					if (Args.Bool(TEXT("fail")))
					{
						return FCursorToolResult::Error(TEXT("asked to fail"));
					}
					TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
					Out->SetStringField(TEXT("text"), Args.Str(TEXT("text")));
					Out->SetStringField(TEXT("caller"), Ctx.Caller);
					return FCursorToolResult::OkJson(Out);
				})
				.Register();

			FCursorToolBuilder(TEXT("__mcp_native_only"), TestPack)
				.Audience(ECursorToolAudience::NativeAgentOnly)
				.Run([](const FCursorArgs&, const FCursorToolContext&) { return FCursorToolResult::Ok(TEXT("x")); })
				.Register();
		}
		~FScopedTools() { FCursorToolRegistry::Get().UnregisterPack(TestPack); }
	};

	/** Sends one message and returns the parsed reply, or null for "no reply". */
	TSharedPtr<FJsonObject> Send(const TCHAR* Request, int32* OutReplies = nullptr)
	{
		TSharedPtr<FJsonObject> Reply;
		int32 Replies = 0;
		FCursorMcpProtocol::HandleMessage(Request, [&](TOptional<FString> Json)
		{
			++Replies;
			if (Json.IsSet())
			{
				Reply = CursorJson::Parse(Json.GetValue());
			}
		});
		if (OutReplies)
		{
			*OutReplies = Replies;
		}
		return Reply;
	}

	int32 ErrorCode(const TSharedPtr<FJsonObject>& Reply)
	{
		const TSharedPtr<FJsonObject>* Error = nullptr;
		if (Reply.IsValid() && Reply->TryGetObjectField(TEXT("error"), Error))
		{
			return static_cast<int32>((*Error)->GetNumberField(TEXT("code")));
		}
		return 0;
	}

	TMap<FString, TArray<FString>> Headers(const TCHAR* Host, const TCHAR* Authorization, const TCHAR* Origin = nullptr)
	{
		TMap<FString, TArray<FString>> Out;
		if (Host) { Out.Add(TEXT("host"), TArray<FString>{ FString(Host) }); }
		if (Authorization) { Out.Add(TEXT("authorization"), TArray<FString>{ FString(Authorization) }); }
		if (Origin) { Out.Add(TEXT("origin"), TArray<FString>{ FString(Origin) }); }
		return Out;
	}
}

using namespace CursorMcpTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorMcpProtocolTest, "CursorUnreal.MCP.Protocol", CursorMcpTests::Flags)
bool FCursorMcpProtocolTest::RunTest(const FString&)
{
	FScopedTools Tools;

	// initialize: answers in the client's revision when we know it, ours when we do not.
	{
		TSharedPtr<FJsonObject> Reply = Send(TEXT("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}"));
		TestTrue(TEXT("initialize replies"), Reply.IsValid());
		if (Reply.IsValid())
		{
			const TSharedPtr<FJsonObject> Result = Reply->GetObjectField(TEXT("result"));
			TestEqual(TEXT("echoes a supported version"), Result->GetStringField(TEXT("protocolVersion")), FString(TEXT("2024-11-05")));
			TestTrue(TEXT("advertises tools"), Result->GetObjectField(TEXT("capabilities"))->HasField(TEXT("tools")));
			TestEqual(TEXT("server name"), Result->GetObjectField(TEXT("serverInfo"))->GetStringField(TEXT("name")), FString(TEXT("cursor-unreal")));
			TestEqual(TEXT("id is echoed"), static_cast<int32>(Reply->GetNumberField(TEXT("id"))), 1);
		}

		Reply = Send(TEXT("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"1999-01-01\"}}"));
		TestEqual(TEXT("unknown version falls back to ours"),
			Reply->GetObjectField(TEXT("result"))->GetStringField(TEXT("protocolVersion")), FString(FCursorMcpProtocol::LatestProtocolVersion));
	}

	// Notifications get no body.
	{
		int32 Replies = 0;
		TSharedPtr<FJsonObject> Reply = Send(TEXT("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"), &Replies);
		TestFalse(TEXT("notification has no reply body"), Reply.IsValid());
		TestEqual(TEXT("notification still completes once"), Replies, 1);
	}

	// tools/list
	{
		TSharedPtr<FJsonObject> Reply = Send(TEXT("{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":\"tools/list\"}"));
		TestEqual(TEXT("string ids are echoed"), Reply->GetStringField(TEXT("id")), FString(TEXT("abc")));
		bool bFoundEcho = false, bFoundNativeOnly = false, bFoundBatch = false;
		for (const TSharedPtr<FJsonValue>& Value : Reply->GetObjectField(TEXT("result"))->GetArrayField(TEXT("tools")))
		{
			const TSharedPtr<FJsonObject> Tool = Value->AsObject();
			const FString Name = Tool->GetStringField(TEXT("name"));
			if (Name == TEXT("__mcp_echo"))
			{
				bFoundEcho = true;
				TestEqual(TEXT("schema is an object schema"), Tool->GetObjectField(TEXT("inputSchema"))->GetStringField(TEXT("type")), FString(TEXT("object")));
				TestTrue(TEXT("read-only tools say so"), Tool->GetObjectField(TEXT("annotations"))->GetBoolField(TEXT("readOnlyHint")));
			}
			bFoundNativeOnly |= Name == TEXT("__mcp_native_only");
			bFoundBatch |= Name == TEXT("batch");
		}
		TestTrue(TEXT("lists registered tools"), bFoundEcho);
		TestTrue(TEXT("lists the batch tool"), bFoundBatch);
		TestFalse(TEXT("hides native-only tools from MCP clients"), bFoundNativeOnly);
	}

	// tools/call
	{
		TSharedPtr<FJsonObject> Reply = Send(TEXT("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"__mcp_echo\",\"arguments\":{\"text\":\"hi\"},\"_meta\":{\"cursorunreal/sentAtMs\":1}}}"));
		const TSharedPtr<FJsonObject> Result = Reply->GetObjectField(TEXT("result"));
		TestFalse(TEXT("call succeeds"), Result->GetBoolField(TEXT("isError")));
		const TSharedPtr<FJsonObject> Block = Result->GetArrayField(TEXT("content"))[0]->AsObject();
		TestEqual(TEXT("content is text"), Block->GetStringField(TEXT("type")), FString(TEXT("text")));
		TestTrue(TEXT("text carries the result"), Block->GetStringField(TEXT("text")).Contains(TEXT("\"hi\"")));
		TestEqual(TEXT("caller is recorded as mcp"), Result->GetObjectField(TEXT("structuredContent"))->GetStringField(TEXT("caller")), FString(TEXT("mcp")));
		TestTrue(TEXT("reports server time"), Result->GetObjectField(TEXT("_meta"))->HasField(FCursorMcpProtocol::MetaServerMsKey));

		// A tool failing is a normal result the model can read, not a protocol error.
		Reply = Send(TEXT("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"__mcp_echo\",\"arguments\":{\"text\":\"x\",\"fail\":true}}}"));
		TestEqual(TEXT("tool failure is not a JSON-RPC error"), ErrorCode(Reply), 0);
		TestTrue(TEXT("tool failure sets isError"), Reply->GetObjectField(TEXT("result"))->GetBoolField(TEXT("isError")));

		Reply = Send(TEXT("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"__no_such_tool\"}}"));
		TestTrue(TEXT("unknown tool is reported as a tool error"), Reply->GetObjectField(TEXT("result"))->GetBoolField(TEXT("isError")));

		// Native-only tools cannot be reached by name either.
		Reply = Send(TEXT("{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"tools/call\",\"params\":{\"name\":\"__mcp_native_only\"}}"));
		TestTrue(TEXT("native-only tool is refused over MCP"), Reply->GetObjectField(TEXT("result"))->GetBoolField(TEXT("isError")));

		Reply = Send(TEXT("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{}}"));
		TestEqual(TEXT("missing name is invalid params"), ErrorCode(Reply), -32602);
	}

	// Malformed input.
	TestEqual(TEXT("bad JSON"), ErrorCode(Send(TEXT("{not json"))), -32700);
	TestEqual(TEXT("batches are refused"), ErrorCode(Send(TEXT("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]"))), -32600);
	TestEqual(TEXT("unknown method"), ErrorCode(Send(TEXT("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"resources/list\"}"))), -32601);
	TestTrue(TEXT("ping answers"), Send(TEXT("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}"))->HasField(TEXT("result")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorMcpSecurityTest, "CursorUnreal.MCP.Security", CursorMcpTests::Flags)
bool FCursorMcpSecurityTest::RunTest(const FString&)
{
	const FString Token = TEXT("0123456789abcdef0123456789abcdef");
	const int32 Port = 18732;
	FString Reason;
	auto Check = [&](const TMap<FString, TArray<FString>>& H) { return FCursorMcpServer::CheckRequestHeaders(H, Port, Token, Reason); };

	TestEqual(TEXT("good request passes"), Check(Headers(TEXT("127.0.0.1:18732"), TEXT("Bearer 0123456789abcdef0123456789abcdef"))), 0);
	TestEqual(TEXT("localhost host passes"), Check(Headers(TEXT("localhost:18732"), TEXT("bearer 0123456789abcdef0123456789abcdef"))), 0);
	TestEqual(TEXT("local browser origin passes"), Check(Headers(TEXT("127.0.0.1:18732"), TEXT("Bearer 0123456789abcdef0123456789abcdef"), TEXT("http://localhost:3000"))), 0);

	TestEqual(TEXT("no token"), Check(Headers(TEXT("127.0.0.1:18732"), nullptr)), 401);
	TestEqual(TEXT("wrong token"), Check(Headers(TEXT("127.0.0.1:18732"), TEXT("Bearer 0123456789abcdef0123456789abcdeX"))), 401);
	TestEqual(TEXT("token prefix only"), Check(Headers(TEXT("127.0.0.1:18732"), TEXT("Bearer 0123456789abcdef"))), 401);
	TestEqual(TEXT("wrong scheme"), Check(Headers(TEXT("127.0.0.1:18732"), TEXT("Basic 0123456789abcdef0123456789abcdef"))), 401);
	TestEqual(TEXT("empty bearer"), Check(Headers(TEXT("127.0.0.1:18732"), TEXT("Bearer "))), 401);

	// An unset server token must not be matched by an empty one.
	TestEqual(TEXT("empty expected token never matches"),
		FCursorMcpServer::CheckRequestHeaders(Headers(TEXT("127.0.0.1:18732"), TEXT("Bearer ")), Port, FString(), Reason), 401);

	// DNS rebinding and cross-site requests are refused before the token is even looked at.
	TestEqual(TEXT("foreign host"), Check(Headers(TEXT("evil.example:18732"), TEXT("Bearer 0123456789abcdef0123456789abcdef"))), 403);
	TestEqual(TEXT("lookalike host"), Check(Headers(TEXT("127.0.0.1.evil.example"), TEXT("Bearer 0123456789abcdef0123456789abcdef"))), 403);
	TestEqual(TEXT("missing host"), Check(Headers(nullptr, TEXT("Bearer 0123456789abcdef0123456789abcdef"))), 403);
	TestEqual(TEXT("wrong port in host"), Check(Headers(TEXT("127.0.0.1:9"), TEXT("Bearer 0123456789abcdef0123456789abcdef"))), 403);
	TestEqual(TEXT("foreign origin"), Check(Headers(TEXT("127.0.0.1:18732"), TEXT("Bearer 0123456789abcdef0123456789abcdef"), TEXT("https://evil.example"))), 403);
	TestEqual(TEXT("lookalike origin"), Check(Headers(TEXT("127.0.0.1:18732"), TEXT("Bearer 0123456789abcdef0123456789abcdef"), TEXT("http://localhost.evil.example"))), 403);
	TestEqual(TEXT("null origin"), Check(Headers(TEXT("127.0.0.1:18732"), TEXT("Bearer 0123456789abcdef0123456789abcdef"), TEXT("null"))), 403);

	// Bind address: only loopback is acceptable.
	TestTrue(TEXT("localhost is loopback"), FCursorMcpServer::IsLoopbackBindAddress(TEXT("localhost")));
	TestTrue(TEXT("127.0.0.1 is loopback"), FCursorMcpServer::IsLoopbackBindAddress(TEXT(" 127.0.0.1 ")));
	TestFalse(TEXT("any is not loopback"), FCursorMcpServer::IsLoopbackBindAddress(TEXT("any")));
	TestFalse(TEXT("0.0.0.0 is not loopback"), FCursorMcpServer::IsLoopbackBindAddress(TEXT("0.0.0.0")));
	TestFalse(TEXT("LAN address is not loopback"), FCursorMcpServer::IsLoopbackBindAddress(TEXT("192.168.1.20")));
	TestFalse(TEXT("empty is not loopback"), FCursorMcpServer::IsLoopbackBindAddress(TEXT("")));

	// The shareable config never contains the token.
	const FString Shareable = FCursorMcpServer::Get().BuildClientConfigJson(/*bIncludeToken*/ false);
	TestTrue(TEXT("shareable config has a placeholder"), Shareable.Contains(TEXT("Bearer <token>")));
	TestTrue(TEXT("shareable config is valid JSON"), CursorJson::Parse(Shareable).IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
