#include "SCursorUnrealPanel.h"

#include "AgentSession.h"
#include "Containers/Ticker.h"
#include "CursorApprovalGate.h"
#include "CursorEditorState.h"
#include "CursorJson.h"
#include "CursorPerfLog.h"
#include "CursorToolRegistry.h"
#include "CursorUnrealLog.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "RenderingThread.h"
#include "SCursorChatView.h"
#include "SCursorMonitorViews.h"
#include "SCursorSetupViews.h"
#include "Slate/WidgetRenderer.h"
#include "Styling/AppStyle.h"
#include "TextureResource.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"

#define LOCTEXT_NAMESPACE "CursorUnrealPanel"

// --- Panel -----------------------------------------------------------------------------------

void SCursorUnrealPanel::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.HAlign(HAlign_Left)
		.Padding(FMargin(8.0f, 8.0f, 8.0f, 2.0f))
		[
			SNew(SSegmentedControl<ETab>)
			.Value_Lambda([this]() { return ActiveTab; })
			.OnValueChanged_Lambda([this](ETab Tab) { ActiveTab = Tab; })
			+ SSegmentedControl<ETab>::Slot(ETab::Chat)
			.Text(LOCTEXT("TabChat", "Chat"))
			.ToolTip(LOCTEXT("TabChatTip", "Talk to the agent."))
			+ SSegmentedControl<ETab>::Slot(ETab::Activity)
			.Text(LOCTEXT("TabActivity", "Activity"))
			.ToolTip(LOCTEXT("TabActivityTip", "Every tool call made in this editor, by any agent."))
			+ SSegmentedControl<ETab>::Slot(ETab::Performance)
			.Text(LOCTEXT("TabPerformance", "Performance"))
			.ToolTip(LOCTEXT("TabPerformanceTip", "How long turns and tools take, and how many tokens they use."))
			+ SSegmentedControl<ETab>::Slot(ETab::Skills)
			.Text(LOCTEXT("TabSkills", "Skills"))
			.ToolTip(LOCTEXT("TabSkillsTip", "The guides the agent reads, and how to make them your own."))
			+ SSegmentedControl<ETab>::Slot(ETab::Setup)
			.Text(LOCTEXT("TabSetup", "Setup"))
			.ToolTip(LOCTEXT("TabSetupTip", "Keys, the Cursor IDE connection, and settings."))
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([this]() { return static_cast<int32>(ActiveTab); })
			+ SWidgetSwitcher::Slot()
			[
				SAssignNew(ChatView, SCursorChatView)
			]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SCursorActivityView)
			]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SCursorPerfView)
			]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SCursorSkillsView)
			]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SCursorSetupView)
			]
		]
	];
}

// --- Approval prompt -------------------------------------------------------------------------

namespace
{
	FString CallerLabel(const FString& Caller)
	{
		if (Caller == FCursorToolRegistry::CallerNative) { return TEXT("the in-editor agent"); }
		if (Caller == FCursorToolRegistry::CallerCursorSdk) { return TEXT("a Cursor agent (through the bridge)"); }
		if (Caller == FCursorToolRegistry::CallerMcp) { return TEXT("an MCP client such as Cursor IDE"); }
		return Caller;
	}

	/** One line the user can scan: collapses whitespace and cuts length. */
	FString OneLine(FString Text, int32 MaxChars)
	{
		Text.ReplaceInline(TEXT("\r\n"), TEXT(" "));
		Text.ReplaceInline(TEXT("\n"), TEXT(" "));
		Text.ReplaceInline(TEXT("\t"), TEXT(" "));
		while (Text.Contains(TEXT("  ")))
		{
			Text.ReplaceInline(TEXT("  "), TEXT(" "));
		}
		return CursorJson::Truncate(Text.TrimStartAndEnd(), MaxChars);
	}

	/**
	 * What the dialog shows under the title. Batches are counted by tool, with a short sample of
	 * calls — never the full argument blob again. A single call gets a short argument preview.
	 * The scroll box, not this text, is what keeps the window on screen.
	 */
	FString BuildApprovalDetail(const FCursorApprovalRequest& Request)
	{
		if (Request.SubCalls.Num() == 0)
		{
			return OneLine(Request.ArgsSummary, 480);
		}

		TMap<FString, int32> Counts;
		for (const FString& Line : Request.SubCalls)
		{
			FString Tool = Line;
			int32 Space = INDEX_NONE;
			if (Line.FindChar(TEXT(' '), Space))
			{
				Tool = Line.Left(Space);
			}
			Counts.FindOrAdd(Tool)++;
		}

		TArray<FString> Parts;
		TSet<FString> Listed;
		for (const FName& Name : Request.ToolsNeedingApproval)
		{
			const FString Key = Name.ToString();
			if (const int32* Count = Counts.Find(Key))
			{
				Parts.Add(FString::Printf(TEXT("%dx %s"), *Count, *Key));
				Listed.Add(Key);
			}
		}
		for (const TPair<FString, int32>& Pair : Counts)
		{
			if (!Listed.Contains(Pair.Key))
			{
				Parts.Add(FString::Printf(TEXT("%dx %s"), Pair.Value, *Pair.Key));
			}
		}

		FString Detail = FString::Printf(TEXT("%d call%s need approval (%s)."),
			Request.SubCalls.Num(),
			Request.SubCalls.Num() == 1 ? TEXT("") : TEXT("s"),
			*FString::Join(Parts, TEXT(", ")));

		constexpr int32 MaxSamples = 5;
		const int32 Shown = FMath::Min(MaxSamples, Request.SubCalls.Num());
		for (int32 Index = 0; Index < Shown; ++Index)
		{
			Detail += TEXT("\n• ") + OneLine(Request.SubCalls[Index], 100);
		}
		if (Request.SubCalls.Num() > MaxSamples)
		{
			Detail += FString::Printf(TEXT("\n…and %d more."), Request.SubCalls.Num() - MaxSamples);
		}
		return Detail;
	}

	TSharedRef<SWidget> MakeDecisionButton(const FText& Label, const FText& Tip, TFunction<void()> OnClick)
	{
		return SNew(SButton)
			.ToolTipText(Tip)
			.OnClicked_Lambda([OnClick = MoveTemp(OnClick)]()
			{
				OnClick();
				return FReply::Handled();
			})
			[
				SNew(STextBlock).Text(Label)
			];
	}

	void PromptForApproval(const FCursorApprovalRequest& Request, TFunction<void(ECursorApprovalDecision)> Respond)
	{
		if (!FSlateApplication::IsInitialized())
		{
			Respond(ECursorApprovalDecision::Deny);
			return;
		}

		TArray<FString> SessionTools;
		for (const FName& Tool : Request.ToolsNeedingApproval)
		{
			SessionTools.Add(Tool.ToString());
		}
		const FString SessionList = SessionTools.Num() > 0 ? FString::Join(SessionTools, TEXT(", ")) : Request.ToolName;
		const FText Intro = FText::Format(
			LOCTEXT("ApprovalIntro", "Asked for by {0}. Risk: {1}."),
			FText::FromString(CallerLabel(Request.Caller)),
			FText::FromString(LexToString(Request.Risk)));
		const FText Detail = FText::FromString(BuildApprovalDetail(Request));

		struct FChoice
		{
			ECursorApprovalDecision Decision = ECursorApprovalDecision::Deny;
			TWeakPtr<SWindow> Window;
			bool bAnswered = false;

			void Choose(ECursorApprovalDecision Next)
			{
				if (bAnswered)
				{
					return;
				}
				bAnswered = true;
				Decision = Next;
				if (const TSharedPtr<SWindow> Open = Window.Pin())
				{
					Open->RequestDestroyWindow();
				}
			}
		};
		const TSharedRef<FChoice> Choice = MakeShared<FChoice>();

		const TSharedRef<SWindow> Window = SNew(SWindow)
			.Title(FText::Format(LOCTEXT("ApprovalTitle", "Allow the agent to run {0}?"), FText::FromString(Request.ToolName)))
			.SizingRule(ESizingRule::Autosized)
			.SupportsMaximize(false)
			.SupportsMinimize(false)
			.HasCloseButton(true)
			.CreateTitleBar(true)
			.IsTopmostWindow(true)
			.FocusWhenFirstShown(true)
			[
				SNew(SBox)
				.Padding(FMargin(16.0f))
				.MinDesiredWidth(480.0f)
				.MaxDesiredWidth(560.0f)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 8.0f)
					[
						SNew(STextBlock)
						.Text(Intro)
						.AutoWrapText(true)
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 12.0f)
					[
						// Cap height so a long batch cannot push the buttons or the title off screen.
						SNew(SBox)
						.MaxDesiredHeight(220.0f)
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot()
							[
								SNew(STextBlock)
								.Text(Detail)
								.AutoWrapText(true)
							]
						]
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							MakeDecisionButton(
								LOCTEXT("AllowOnce", "Allow once"),
								LOCTEXT("AllowOnceTip", "Run this call. The next one asks again."),
								[Choice]() { Choice->Choose(ECursorApprovalDecision::AllowOnce); })
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.Padding(0.0f, 0.0f, 8.0f, 0.0f)
						[
							MakeDecisionButton(
								LOCTEXT("AllowSession", "Allow until I close the editor"),
								FText::Format(LOCTEXT("AllowSessionTip", "Run this call and stop asking, until the editor closes, for: {0}"),
									FText::FromString(SessionList)),
								[Choice]() { Choice->Choose(ECursorApprovalDecision::AllowForSession); })
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						[
							MakeDecisionButton(
								LOCTEXT("Deny", "Deny"),
								LOCTEXT("DenyTip", "Do not run it. The agent is told it was refused."),
								[Choice]() { Choice->Choose(ECursorApprovalDecision::Deny); })
						]
					]
				]
			];

		Choice->Window = Window;
		Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([Choice](const TSharedRef<SWindow>&)
		{
			// Closing the window without a button is a refusal.
			Choice->Choose(ECursorApprovalDecision::Deny);
		}));

		const TSharedPtr<SWindow> Parent = FSlateApplication::Get().GetActiveTopLevelWindow();
		FSlateApplication::Get().AddModalWindow(Window, Parent);
		Respond(Choice->Decision);
	}
}

void CursorApprovalUI::Install()
{
	FCursorApprovalGate::Get().SetHandler(FCursorApprovalHandler::CreateStatic(&PromptForApproval));
}

void CursorApprovalUI::Uninstall()
{
	FCursorApprovalGate::Get().ClearHandler();
}

// --- Documentation screenshots ---------------------------------------------------------------

namespace
{
	/**
	 * Builds the panel, plays one demo turn, and renders each tab to a file.
	 *
	 * The work is spread over two moments a second apart. Icons are rasterised the first time
	 * they are drawn and reach the GPU on the editor's next frame, so a picture taken on the
	 * very first draw shows blank squares where they should be.
	 */
	class FPanelShotJob : public TSharedFromThis<FPanelShotJob>
	{
	public:
		explicit FPanelShotJob(const FString& InDirectory)
			: Directory(InDirectory)
			, Renderer(false)
		{
		}

		~FPanelShotJob()
		{
			if (Target)
			{
				Target->RemoveFromRoot();
			}
		}

		/** Returns false, having logged why, if there is nothing to photograph. */
		bool Prepare()
		{
			FText Error;
			const TSharedPtr<IAgentBackend> Backend = CursorAgent::CreateDemoBackend(Transport, Error);
			if (!Backend.IsValid())
			{
				UE_LOG(LogCursorUnreal, Error, TEXT("Screenshots: %s"), *Error.ToString());
				return false;
			}

			// Demo numbers are invented, so keep them out of the performance log on disk.
			FCursorPerfLog::Get().bWriteToDisk = false;

			Session = MakeShared<FAgentSession>(Backend.ToSharedRef());
			FCursorEditorState::Get().UseSession(Session);

			Panel = SNew(SCursorUnrealPanel);
			Framed = SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
				.Padding(FMargin(0.0f))
				[
					Panel.ToSharedRef()
				];

			FText WhyNot;
			if (!Session->Send(TEXT("What can you do in this project?"), TArray<FAgentContextBlock>(), WhyNot))
			{
				UE_LOG(LogCursorUnreal, Error, TEXT("Screenshots: the demo turn did not start: %s"), *WhyNot.ToString());
				return false;
			}
			Transport->PumpAll();
			Panel->GetChatView()->FlushPending();

			// The colours Slate draws are already display-ready, so no gamma step on the way to the file.
			Target = FWidgetRenderer::CreateTargetFor(FVector2D(Size), TF_Bilinear, false);
			if (!Target)
			{
				UE_LOG(LogCursorUnreal, Error, TEXT("Screenshots: no render target. Run with -RenderOffScreen, not -nullrhi."));
				return false;
			}
			Target->AddToRoot();

			// Draw everything once and throw the result away, to get every icon and glyph requested.
			Panel->GetChatView()->SetAllExpanded(true);
			Panel->GetChatView()->FlushPending();
			for (const SCursorUnrealPanel::ETab Tab : { SCursorUnrealPanel::ETab::Chat, SCursorUnrealPanel::ETab::Activity,
				SCursorUnrealPanel::ETab::Performance, SCursorUnrealPanel::ETab::Skills, SCursorUnrealPanel::ETab::Setup })
			{
				Panel->SetTab(Tab);
				Draw();
			}
			Panel->GetChatView()->SetAllExpanded(false);
			return true;
		}

		int32 Capture()
		{
			IFileManager::Get().MakeDirectory(*Directory, true);
			int32 NumWritten = 0;
			NumWritten += Shoot(SCursorUnrealPanel::ETab::Chat, TEXT("panel-chat.png"));
			Panel->GetChatView()->SetAllExpanded(true);
			Panel->GetChatView()->FlushPending();
			NumWritten += Shoot(SCursorUnrealPanel::ETab::Chat, TEXT("panel-chat-expanded.png"));
			NumWritten += Shoot(SCursorUnrealPanel::ETab::Activity, TEXT("panel-activity.png"));
			NumWritten += Shoot(SCursorUnrealPanel::ETab::Performance, TEXT("panel-performance.png"));
			NumWritten += Shoot(SCursorUnrealPanel::ETab::Skills, TEXT("panel-skills.png"));
			NumWritten += Shoot(SCursorUnrealPanel::ETab::Setup, TEXT("panel-setup.png"));

			Panel.Reset();
			Framed.Reset();
			FCursorEditorState::Get().UseSession(nullptr);
			return NumWritten;
		}

	private:
		void Draw()
		{
			// Lists build their rows on one pass and lay them out on the next, so draw a few times.
			for (int32 Pass = 0; Pass < 4; ++Pass)
			{
				Renderer.DrawWidget(Target, Framed.ToSharedRef(), FVector2D(Size), 0.1f);
				FlushRenderingCommands();
			}
		}

		int32 Shoot(SCursorUnrealPanel::ETab Tab, const TCHAR* Name)
		{
			Panel->SetTab(Tab);
			Draw();

			const FString Path = Directory / Name;
			TArray<FColor> Pixels;
			FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
			bool bOk = Resource && Resource->ReadPixels(Pixels) && Pixels.Num() == Size.X * Size.Y;
			if (bOk)
			{
				for (FColor& Pixel : Pixels)
				{
					Pixel.A = 255;
				}
				TArray64<uint8> Png;
				FImageUtils::PNGCompressImageArray(Size.X, Size.Y, TArrayView64<const FColor>(Pixels.GetData(), Pixels.Num()), Png);
				bOk = Png.Num() > 0 && FFileHelper::SaveArrayToFile(Png, *Path);
			}
			if (bOk)
			{
				UE_LOG(LogCursorUnreal, Display, TEXT("Screenshots: wrote %s"), *Path);
			}
			else
			{
				UE_LOG(LogCursorUnreal, Error, TEXT("Screenshots: could not write %s"), *Path);
			}
			return bOk ? 1 : 0;
		}

		const FIntPoint Size = FIntPoint(900, 1000);
		FString Directory;
		FWidgetRenderer Renderer;
		UTextureRenderTarget2D* Target = nullptr;
		TSharedPtr<FAgentFixtureTransport> Transport;
		TSharedPtr<FAgentSession> Session;
		TSharedPtr<SCursorUnrealPanel> Panel;
		TSharedPtr<SWidget> Framed;
	};
}

bool CursorPanelShots::IsRequested(FString& OutDirectory)
{
	return FParse::Value(FCommandLine::Get(), TEXT("-CursorUnrealShots="), OutDirectory) && !OutDirectory.IsEmpty();
}

void CursorPanelShots::RunAndExit(const FString& Directory)
{
	const TSharedRef<FPanelShotJob> Job = MakeShared<FPanelShotJob>(Directory);
	if (!Job->Prepare())
	{
		UE_LOG(LogCursorUnreal, Display, TEXT("Screenshots: 0 written to %s"), *Directory);
		FPlatformMisc::RequestExit(false);
		return;
	}
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Job, Directory](float)
	{
		const int32 NumWritten = Job->Capture();
		UE_LOG(LogCursorUnreal, Display, TEXT("Screenshots: %d written to %s"), NumWritten, *Directory);
		FPlatformMisc::RequestExit(false);
		return false;
	}), 1.0f);
}

#undef LOCTEXT_NAMESPACE