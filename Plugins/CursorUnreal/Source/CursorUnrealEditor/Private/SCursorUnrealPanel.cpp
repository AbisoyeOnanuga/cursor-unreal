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
#include "Framework/Notifications/NotificationManager.h"
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
#include "Widgets/Notifications/SNotificationList.h"
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
	struct FPendingApproval
	{
		TFunction<void(ECursorApprovalDecision)> Respond;
		TWeakPtr<SNotificationItem> Item;
		bool bAnswered = false;
	};

	FString CallerLabel(const FString& Caller)
	{
		if (Caller == FCursorToolRegistry::CallerNative) { return TEXT("the in-editor agent"); }
		if (Caller == FCursorToolRegistry::CallerCursorSdk) { return TEXT("a Cursor agent (through the bridge)"); }
		if (Caller == FCursorToolRegistry::CallerMcp) { return TEXT("an MCP client such as Cursor IDE"); }
		return Caller;
	}

	void PromptForApproval(const FCursorApprovalRequest& Request, TFunction<void(ECursorApprovalDecision)> Respond)
	{
		if (!FSlateApplication::IsInitialized())
		{
			Respond(ECursorApprovalDecision::Deny);
			return;
		}

		FString Body = FString::Printf(TEXT("Asked for by %s. Risk: %s.\n\n%s"),
			*CallerLabel(Request.Caller), LexToString(Request.Risk), *CursorJson::Truncate(Request.ArgsSummary, 900));
		if (Request.SubCalls.Num() > 0)
		{
			Body += TEXT("\n\nCalls inside it that need approval:\n") + FString::Join(Request.SubCalls, TEXT("\n"));
		}

		TArray<FString> SessionTools;
		for (const FName& Tool : Request.ToolsNeedingApproval)
		{
			SessionTools.Add(Tool.ToString());
		}

		const TSharedRef<FPendingApproval> Pending = MakeShared<FPendingApproval>();
		Pending->Respond = MoveTemp(Respond);
		const auto Decide = [Pending](ECursorApprovalDecision Decision)
		{
			if (Pending->bAnswered)
			{
				return;
			}
			Pending->bAnswered = true;
			if (const TSharedPtr<SNotificationItem> Item = Pending->Item.Pin())
			{
				Item->SetCompletionState(Decision == ECursorApprovalDecision::Deny ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
				Item->ExpireAndFadeout();
			}
			Pending->Respond(Decision);
		};

		FNotificationInfo Info(FText::Format(LOCTEXT("ApprovalTitle", "Allow the agent to run {0}?"), FText::FromString(Request.ToolName)));
		Info.SubText = FText::FromString(Body);
		Info.bFireAndForget = false;
		Info.bUseSuccessFailIcons = false;
		Info.bUseThrobber = false;
		Info.ExpireDuration = 0.0f;
		Info.FadeOutDuration = 0.3f;
		Info.WidthOverride = 560.0f;
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("AllowOnce", "Allow once"),
			LOCTEXT("AllowOnceTip", "Run this call. The next one asks again."),
			FSimpleDelegate::CreateLambda([Decide]() { Decide(ECursorApprovalDecision::AllowOnce); }),
			SNotificationItem::CS_Pending));
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("AllowSession", "Allow until I close the editor"),
			FText::Format(LOCTEXT("AllowSessionTip", "Run this call and stop asking, until the editor closes, for: {0}"),
				FText::FromString(FString::Join(SessionTools, TEXT(", ")))),
			FSimpleDelegate::CreateLambda([Decide]() { Decide(ECursorApprovalDecision::AllowForSession); }),
			SNotificationItem::CS_Pending));
		Info.ButtonDetails.Add(FNotificationButtonInfo(
			LOCTEXT("Deny", "Deny"),
			LOCTEXT("DenyTip", "Do not run it. The agent is told it was refused."),
			FSimpleDelegate::CreateLambda([Decide]() { Decide(ECursorApprovalDecision::Deny); }),
			SNotificationItem::CS_Pending));

		const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
		if (!Item.IsValid())
		{
			// Nowhere to ask, so the safe answer.
			Decide(ECursorApprovalDecision::Deny);
			return;
		}
		Pending->Item = Item;
		Item->SetCompletionState(SNotificationItem::CS_Pending);
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