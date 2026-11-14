#include "CursorTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AgentSession.h"
#include "CursorEditorState.h"
#include "CursorMarkdown.h"
#include "CursorPerfLog.h"
#include "Framework/Application/SlateApplication.h"
#include "SCursorChatView.h"
#include "SCursorUnrealPanel.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorEditorMarkdownTest, "CursorUnreal.Editor.Markdown", CursorTest::Flags)
bool FCursorEditorMarkdownTest::RunTest(const FString&)
{
	using namespace CursorMarkdown;

	// Splitting at code fences.
	{
		const TArray<FSegment> Segments = Split(TEXT("Before.\n\n```cpp\nint A = 1;\nint B = 2;\n```\n\nAfter."));
		TestEqual(TEXT("prose, code, prose"), Segments.Num(), 3);
		if (Segments.Num() == 3)
		{
			TestFalse(TEXT("first is prose"), Segments[0].bCode);
			TestEqual(TEXT("prose keeps its text, without the blank line"), Segments[0].Text, FString(TEXT("Before.")));
			TestTrue(TEXT("second is code"), Segments[1].bCode);
			TestEqual(TEXT("language"), Segments[1].Language, FString(TEXT("cpp")));
			TestEqual(TEXT("code keeps its line breaks"), Segments[1].Text, FString(TEXT("int A = 1;\nint B = 2;")));
			TestEqual(TEXT("prose after"), Segments[2].Text, FString(TEXT("After.")));
		}
	}

	// A reply that is still arriving: the fence is open.
	{
		const TArray<FSegment> Segments = Split(TEXT("Here:\n```\nhalf a li"));
		TestEqual(TEXT("prose and an open block"), Segments.Num(), 2);
		if (Segments.Num() == 2)
		{
			TestTrue(TEXT("the open block counts as code"), Segments[1].bCode);
			TestEqual(TEXT("with what has arrived"), Segments[1].Text, FString(TEXT("half a li")));
		}
		const TArray<FSegment> JustOpened = Split(TEXT("Here:\n```cpp"));
		TestEqual(TEXT("an empty open block is still shown"), JustOpened.Num(), 2);
	}

	// Nothing but text, and nothing at all.
	TestEqual(TEXT("plain text is one segment"), Split(TEXT("Just words.")).Num(), 1);
	TestEqual(TEXT("empty text has no segments"), Split(TEXT("")).Num(), 0);
	TestEqual(TEXT("blank lines alone have no segments"), Split(TEXT("\n\n")).Num(), 0);

	// Markup.
	TestEqual(TEXT("bold"), ToRichText(TEXT("a **b** c")), FString(TEXT("a <b>b</> c")));
	TestEqual(TEXT("inline code"), ToRichText(TEXT("call `Foo()` now")), FString(TEXT("call <code>Foo()</> now")));
	TestEqual(TEXT("heading"), ToRichText(TEXT("## Title")), FString(TEXT("<h>Title</>")));
	TestEqual(TEXT("heading drops inner markers, since tags cannot nest"), ToRichText(TEXT("# The `Tick` **cost**")), FString(TEXT("<h>The Tick cost</>")));
	TestEqual(TEXT("bullet"), ToRichText(TEXT("- one")), FString(TEXT("  \u2022 one")));
	TestEqual(TEXT("nested bullet keeps its indent"), ToRichText(TEXT("  - two")), FString(TEXT("    \u2022 two")));
	TestEqual(TEXT("lines stay separate"), ToRichText(TEXT("a\nb")), FString(TEXT("a\nb")));

	// Text that looks like markup must not become markup.
	TestEqual(TEXT("angle brackets are escaped"), ToRichText(TEXT("TArray<int32> & more")), FString(TEXT("TArray&lt;int32&gt; &amp; more")));
	TestEqual(TEXT("escaped inside code too"), ToRichText(TEXT("`TMap<K, V>`")), FString(TEXT("<code>TMap&lt;K, V&gt;</>")));
	TestEqual(TEXT("a tag typed by the model is inert"), ToRichText(TEXT("<b>not bold</>")), FString(TEXT("&lt;b&gt;not bold&lt;/&gt;")));

	// Markers without a partner stay as typed (common while a reply is arriving).
	TestEqual(TEXT("open bold"), ToRichText(TEXT("a **b")), FString(TEXT("a **b")));
	TestEqual(TEXT("open code"), ToRichText(TEXT("a `b")), FString(TEXT("a `b")));
	TestEqual(TEXT("a lone star is not a bullet"), ToRichText(TEXT("2 * 3")), FString(TEXT("2 * 3")));
	TestEqual(TEXT("hash without a space is not a heading"), ToRichText(TEXT("#include")), FString(TEXT("#include")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorEditorPanelTest, "CursorUnreal.Editor.Panel", CursorTest::Flags)
bool FCursorEditorPanelTest::RunTest(const FString&)
{
	if (!FSlateApplication::IsInitialized())
	{
		AddWarning(TEXT("Slate is not running in this process, so the panel was not built and this test checked nothing."));
		return true;
	}

	CursorTest::FScopedPolicy Policy;
	const bool bPerfToDisk = FCursorPerfLog::Get().bWriteToDisk;
	FCursorPerfLog::Get().bWriteToDisk = false;

	FText Error;
	TSharedPtr<FAgentFixtureTransport> Transport;
	const TSharedPtr<IAgentBackend> Backend = CursorAgent::CreateDemoBackend(Transport, Error);
	TestTrue(FString::Printf(TEXT("demo backend builds (%s)"), *Error.ToString()), Backend.IsValid() && Transport.IsValid());
	if (Backend.IsValid() && Transport.IsValid())
	{
		TestFalse(TEXT("the demo says what it is"), Backend->GetBanner().IsEmpty());
		TestFalse(TEXT("the demo has a model chosen"), Backend->GetModelId().IsEmpty());

		const TSharedRef<FAgentSession> Session = MakeShared<FAgentSession>(Backend.ToSharedRef());
		Session->bWriteToDisk = false;
		FCursorEditorState::Get().UseSession(Session);
		{
			// Builds every tab: chat, activity, performance, skills and setup.
			const TSharedRef<SCursorUnrealPanel> Panel = SNew(SCursorUnrealPanel);
			const TSharedPtr<SCursorChatView> Chat = Panel->GetChatView();
			TestTrue(TEXT("chat view built"), Chat.IsValid());
			TestEqual(TEXT("empty chat shows nothing"), Chat->NumItemWidgets(), 0);

			FText WhyNot;
			TestTrue(TEXT("demo turn starts"), Session->Send(TEXT("What can you do here?"), TArray<FAgentContextBlock>(), WhyNot));
			Transport->PumpAll();
			Chat->FlushPending();

			TestFalse(TEXT("demo turn ends"), Session->IsBusy());
			TestEqual(TEXT("ended normally"), Session->GetLastTurn().Outcome, FString(TEXT("done")));
			TestEqual(TEXT("both scripted tools ran for real"), Session->GetLastTurn().ToolCalls, 2);
			TestEqual(TEXT("one widget per chat item"), Chat->NumItemWidgets(), Session->GetItems().Num());
			// user, thinking, text, tool, tool, answer
			TestEqual(TEXT("items in the demo turn"), Session->GetItems().Num(), 6);

			bool bToolsSucceeded = true;
			for (const TSharedRef<FAgentItem>& Item : Session->GetItems())
			{
				if (Item->Kind == EAgentItemKind::Tool)
				{
					bToolsSucceeded &= Item->ToolState == EAgentToolState::Finished && !Item->bIsError;
				}
			}
			TestTrue(TEXT("the demo's tool calls succeed"), bToolsSucceeded);

			// Opening the cards and switching tabs must not disturb anything.
			Chat->SetAllExpanded(true);
			Chat->FlushPending();
			for (const SCursorUnrealPanel::ETab Tab : { SCursorUnrealPanel::ETab::Activity, SCursorUnrealPanel::ETab::Performance,
				SCursorUnrealPanel::ETab::Skills, SCursorUnrealPanel::ETab::Setup, SCursorUnrealPanel::ETab::Chat })
			{
				Panel->SetTab(Tab);
				Panel->SlatePrepass(1.0f);
			}

			Session->NewChat();
			Chat->FlushPending();
			TestEqual(TEXT("new chat clears the view"), Chat->NumItemWidgets(), 0);

			// The script loops, so a second chat works the same way.
			TestTrue(TEXT("second demo turn starts"), Session->Send(TEXT("Again."), TArray<FAgentContextBlock>(), WhyNot));
			Transport->PumpAll();
			Chat->FlushPending();
			TestEqual(TEXT("second turn ended normally"), Session->GetLastTurn().Outcome, FString(TEXT("done")));
			TestEqual(TEXT("second turn drew its items"), Chat->NumItemWidgets(), 6);

			// Pictures: attached to the message being written, sent with it, shown with it.
			Session->NewChat();
			Chat->FlushPending();
			TArray<FCursorImage> Pictures;
			for (int32 Index = 0; Index < CursorImages::MaxPerMessage + 1; ++Index)
			{
				Pictures.Add(CursorTest::MakeImage(60 + Index, 24, 16));
			}
			TestEqual(TEXT("the same picture is attached once"), Chat->AttachImages({ Pictures[0], Pictures[0] }), 1);
			TestEqual(TEXT("a file that is not a picture is not attached"), Chat->AttachFiles({ FPaths::ProjectDir() / TEXT("CursorUnrealDev.uproject") }), 0);
			TestEqual(TEXT("a message takes eight pictures and no more"), Chat->AttachImages(Pictures), CursorImages::MaxPerMessage - 1);
			TestEqual(TEXT("waiting to be sent"), Chat->NumAttachedImages(), CursorImages::MaxPerMessage);
			Panel->SlatePrepass(1.0f);

			Chat->Send();
			TestEqual(TEXT("sending takes the pictures with it"), Chat->NumAttachedImages(), 0);
			Transport->PumpAll();
			Chat->FlushPending();
			TestFalse(TEXT("the turn with pictures ends"), Session->IsBusy());
			TestEqual(TEXT("and ends normally"), Session->GetLastTurn().Outcome, FString(TEXT("done")));
			if (Session->GetItems().Num() > 0)
			{
				const FAgentItem& Sent = Session->GetItems()[0].Get();
				TestTrue(TEXT("the message carries the pictures"), Sent.Kind == EAgentItemKind::User && Sent.Images.Num() == CursorImages::MaxPerMessage);
				TestTrue(TEXT("in memory only, since this test session does not write to disk"), Sent.Images.Num() > 0 && Sent.Images[0].File.IsEmpty());
				TestEqual(TEXT("words are supplied when only pictures are sent"), Sent.Text, FString(TEXT("Look at the attached pictures.")));
			}
			else
			{
				AddError(TEXT("the message with pictures did not reach the chat"));
			}
			Panel->SlatePrepass(1.0f);
		}
		FCursorEditorState::Get().UseSession(nullptr);
	}

	FCursorPerfLog::Get().bWriteToDisk = bPerfToDisk;
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
