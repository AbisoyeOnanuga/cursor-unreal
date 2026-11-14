#include "SCursorChatView.h"

#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "Framework/Application/SlateApplication.h"
#include "CursorEditorState.h"
#include "CursorEditorStyle.h"
#include "CursorJson.h"
#include "CursorLogCapture.h"
#include "Editor.h"
#include "Engine/Selection.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformApplicationMisc.h"
#include "IContentBrowserSingleton.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Text/SRichTextBlock.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CursorUnrealChat"

namespace
{
	/** A card shows this much of a tool result. The transcript on disk keeps more. */
	constexpr int32 MaxCardChars = 6000;
	constexpr int32 MaxListedInContext = 25;

	const FTextBlockStyle& BodyStyle()
	{
		return FCursorEditorStyle::Get().GetWidgetStyle<FTextBlockStyle>(FCursorEditorStyle::BodyText);
	}

	const FTextBlockStyle& SubduedStyle()
	{
		return FCursorEditorStyle::Get().GetWidgetStyle<FTextBlockStyle>(FCursorEditorStyle::SubduedText);
	}

	FString CutForCard(const FString& Text)
	{
		if (Text.Len() <= MaxCardChars)
		{
			return Text;
		}
		return Text.Left(MaxCardChars) + FString::Printf(TEXT("\n... (%d more characters not shown here)"), Text.Len() - MaxCardChars);
	}

	FString PrettyArgs(const FString& ArgsJson)
	{
		const TSharedPtr<FJsonObject> Parsed = CursorJson::Parse(ArgsJson);
		return Parsed.IsValid() ? CursorJson::ToString(Parsed.ToSharedRef(), true) : ArgsJson;
	}

	FString FirstLine(const FString& Text, int32 MaxChars)
	{
		FString Line = Text.TrimStart();
		int32 Break = INDEX_NONE;
		if (Line.FindChar(TEXT('\n'), Break))
		{
			Line.LeftInline(Break);
		}
		Line.TrimEndInline();
		return Line.Len() > MaxChars ? Line.Left(MaxChars) + TEXT("...") : Line;
	}

	FText Thousands(int64 Value)
	{
		return FText::AsNumber(Value);
	}
}

// --- SCursorChatItem -------------------------------------------------------------------------

void SCursorChatItem::Construct(const FArguments& InArgs, const TSharedRef<FAgentItem>& InItem)
{
	Item = InItem;

	switch (InItem->Kind)
	{
	case EAgentItemKind::User:
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FCursorEditorStyle::Brush(TEXT("Bubble.User")))
			.Padding(FMargin(10.0f, 8.0f))
			[
				SAssignNew(PlainText, SMultiLineEditableText)
				.IsReadOnly(true)
				.AutoWrapText(true)
				.TextStyle(&BodyStyle())
			]
		];
		break;

	case EAgentItemKind::Assistant:
		ChildSlot
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SAssignNew(SegmentBox, SVerticalBox)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(FMargin(4.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(2.0f))
				.ToolTipText(LOCTEXT("CopyReplyTip", "Copy this reply as Markdown"))
				.OnClicked_Lambda([WeakItem = TWeakPtr<FAgentItem>(InItem)]()
				{
					if (const TSharedPtr<FAgentItem> Pinned = WeakItem.Pin())
					{
						FPlatformApplicationMisc::ClipboardCopy(*Pinned->Text);
					}
					return FReply::Handled();
				})
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("GenericCommands.Copy"))
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				]
			]
		];
		break;

	case EAgentItemKind::Thinking:
		ChildSlot
		[
			MakeCollapsible(
				SAssignNew(BodyText, SMultiLineEditableText)
				.IsReadOnly(true)
				.AutoWrapText(true)
				.TextStyle(&SubduedStyle()))
		];
		break;

	case EAgentItemKind::Tool:
		ChildSlot
		[
			MakeCollapsible(
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.TextStyle(&SubduedStyle())
					.Text(LOCTEXT("ArgsLabel", "Arguments"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 2.0f, 0.0f, 8.0f))
				[
					SAssignNew(ArgsText, SMultiLineEditableText)
					.IsReadOnly(true)
					.AutoWrapText(true)
					.Font(FCursorEditorStyle::Mono())
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(STextBlock)
					.TextStyle(&SubduedStyle())
					.Text(LOCTEXT("ResultLabel", "Result"))
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
				[
					SAssignNew(BodyText, SMultiLineEditableText)
					.IsReadOnly(true)
					.AutoWrapText(true)
					.Font(FCursorEditorStyle::Mono())
				])
		];
		break;

	case EAgentItemKind::Error:
		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FCursorEditorStyle::Brush(TEXT("Bubble.Error")))
			.Padding(FMargin(10.0f, 8.0f))
			[
				SAssignNew(PlainText, SMultiLineEditableText)
				.IsReadOnly(true)
				.AutoWrapText(true)
				.TextStyle(&BodyStyle())
			]
		];
		break;

	default:
		ChildSlot
		[
			SAssignNew(PlainText, SMultiLineEditableText)
			.IsReadOnly(true)
			.AutoWrapText(true)
			.TextStyle(&SubduedStyle())
		];
		break;
	}

	Refresh();
}

TSharedRef<SWidget> SCursorChatItem::MakeCollapsible(const TSharedRef<SWidget>& Body)
{
	return SNew(SBorder)
		.BorderImage(FCursorEditorStyle::Brush(TEXT("Bubble.Tool")))
		.Padding(FMargin(0.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.ContentPadding(FMargin(6.0f, 4.0f))
				.HAlign(HAlign_Fill)
				.OnClicked_Lambda([this]()
				{
					SetExpanded(!bExpanded);
					return FReply::Handled();
				})
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
					[
						SNew(SImage)
						.Image_Lambda([this]() { return FAppStyle::GetBrush(bExpanded ? "Icons.ChevronDown" : "Icons.ChevronRight"); })
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(this, &SCursorChatItem::GetCardTitle)
						.ColorAndOpacity(this, &SCursorChatItem::GetCardTitleColor)
					]
					+ SHorizontalBox::Slot()
					.FillWidth(1.0f)
					.VAlign(VAlign_Center)
					.Padding(FMargin(10.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(STextBlock)
						.Text(this, &SCursorChatItem::GetCardPreview)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBox)
				.Padding(FMargin(10.0f, 2.0f, 10.0f, 10.0f))
				.Visibility_Lambda([this]() { return bExpanded ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					Body
				]
			]
		];
}

FText SCursorChatItem::GetCardTitle() const
{
	if (Item->Kind == EAgentItemKind::Thinking)
	{
		return LOCTEXT("ThinkingTitle", "Thinking");
	}
	const FText Name = FText::FromString(Item->ToolName);
	switch (Item->ToolState)
	{
	case EAgentToolState::Preparing:
		return FText::Format(LOCTEXT("ToolPreparing", "{0}  -  writing the call"), Name);
	case EAgentToolState::Running:
		return FText::Format(LOCTEXT("ToolRunning", "{0}  -  running"), Name);
	case EAgentToolState::Finished:
	{
		// Most editor tools finish in well under a millisecond, so whole numbers would read "0 ms".
		const FString Took = Item->ToolMs >= 10000.0 ? FString::Printf(TEXT("%.1f s"), Item->ToolMs / 1000.0)
			: Item->ToolMs >= 10.0 ? FString::Printf(TEXT("%.0f ms"), Item->ToolMs)
			: FString::Printf(TEXT("%.1f ms"), Item->ToolMs);
		return FText::Format(Item->bIsError ? LOCTEXT("ToolFailed", "{0}  -  failed after {1}") : LOCTEXT("ToolDone", "{0}  -  done in {1}"),
			Name, FText::FromString(Took));
	}
	default:
		return FText::Format(LOCTEXT("ToolNotRun", "{0}  -  not run"), Name);
	}
}

FText SCursorChatItem::GetCardPreview() const
{
	if (bExpanded)
	{
		return FText::GetEmpty();
	}
	if (Item->Kind == EAgentItemKind::Thinking)
	{
		return FText::FromString(FirstLine(Item->Text, 120));
	}
	return Item->ArgsJson.Len() > 2 ? FText::FromString(FirstLine(Item->ArgsJson, 120)) : FText::GetEmpty();
}

FSlateColor SCursorChatItem::GetCardTitleColor() const
{
	if (Item->Kind == EAgentItemKind::Tool)
	{
		if (Item->bIsError)
		{
			return FStyleColors::Error;
		}
		if (Item->ToolState == EAgentToolState::NotRun)
		{
			return FStyleColors::Warning;
		}
	}
	return FSlateColor::UseForeground();
}

void SCursorChatItem::SetExpanded(bool bInExpanded)
{
	if (Item->Kind != EAgentItemKind::Tool && Item->Kind != EAgentItemKind::Thinking)
	{
		return;
	}
	bExpanded = bInExpanded;
	Refresh();
}

TSharedRef<SWidget> SCursorChatItem::MakeCodeBlock(const FString& Language, TFunction<void(const FString&)>& OutSetter)
{
	const TSharedRef<FString> Code = MakeShared<FString>();
	TSharedPtr<SMultiLineEditableText> CodeText;

	const TSharedRef<SWidget> Widget = SNew(SBorder)
		.BorderImage(FCursorEditorStyle::Brush(TEXT("Bubble.Code")))
		.Padding(FMargin(10.0f, 4.0f, 6.0f, 8.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.TextStyle(&SubduedStyle())
					.Text(FText::FromString(Language.IsEmpty() ? FString(TEXT("code")) : Language))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.ButtonStyle(FAppStyle::Get(), "SimpleButton")
					.ContentPadding(FMargin(2.0f))
					.ToolTipText(LOCTEXT("CopyCodeTip", "Copy this block"))
					.OnClicked_Lambda([Code]()
					{
						FPlatformApplicationMisc::ClipboardCopy(**Code);
						return FReply::Handled();
					})
					[
						SNew(SImage)
						.Image(FAppStyle::GetBrush("GenericCommands.Copy"))
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					]
				]
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SAssignNew(CodeText, SMultiLineEditableText)
				.IsReadOnly(true)
				.AutoWrapText(true)
				.Font(FCursorEditorStyle::Mono())
			]
		];

	OutSetter = [Code, CodeText](const FString& Text)
	{
		*Code = Text;
		CodeText->SetText(FText::FromString(Text));
	};
	return Widget;
}

void SCursorChatItem::RefreshAssistant()
{
	TArray<CursorMarkdown::FSegment> Segments = CursorMarkdown::Split(Item->Text);

	bool bSameShape = Segments.Num() == ShownSegments.Num();
	for (int32 Index = 0; bSameShape && Index < Segments.Num(); ++Index)
	{
		bSameShape = Segments[Index].SameShape(ShownSegments[Index]);
	}

	if (!bSameShape)
	{
		// The arrangement changed (a code block opened or closed), so lay the reply out again.
		SegmentBox->ClearChildren();
		SegmentSetters.Reset();
		ShownSegments.Reset();
		for (const CursorMarkdown::FSegment& Segment : Segments)
		{
			TFunction<void(const FString&)> Setter;
			TSharedPtr<SWidget> Widget;
			if (Segment.bCode)
			{
				Widget = MakeCodeBlock(Segment.Language, Setter);
			}
			else
			{
				const TSharedRef<SRichTextBlock> Rich = SNew(SRichTextBlock)
					.AutoWrapText(true)
					.TextStyle(&BodyStyle())
					.DecoratorStyleSet(&FCursorEditorStyle::Get());
				Setter = [Rich](const FString& Text) { Rich->SetText(FText::FromString(CursorMarkdown::ToRichText(Text))); };
				Widget = Rich;
			}
			SegmentBox->AddSlot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 2.0f))
				[
					Widget.ToSharedRef()
				];
			SegmentSetters.Add(MoveTemp(Setter));

			CursorMarkdown::FSegment Blank;
			Blank.bCode = Segment.bCode;
			Blank.Language = Segment.Language;
			ShownSegments.Add(Blank);
		}
	}

	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		if (Segments[Index].Text != ShownSegments[Index].Text)
		{
			SegmentSetters[Index](Segments[Index].Text);
		}
	}
	ShownSegments = MoveTemp(Segments);
}

void SCursorChatItem::Refresh()
{
	switch (Item->Kind)
	{
	case EAgentItemKind::Assistant:
		RefreshAssistant();
		break;

	case EAgentItemKind::Thinking:
		// Filled only while open: a long block of thinking is not worth laying out unseen.
		if (bExpanded && ShownBody != Item->Text)
		{
			ShownBody = Item->Text;
			BodyText->SetText(FText::FromString(ShownBody));
		}
		break;

	case EAgentItemKind::Tool:
		if (bExpanded)
		{
			if (ShownArgs != Item->ArgsJson)
			{
				ShownArgs = Item->ArgsJson;
				ArgsText->SetText(FText::FromString(ShownArgs.IsEmpty() ? FString(TEXT("(still arriving)")) : PrettyArgs(ShownArgs)));
			}
			const FString Result = Item->ToolState == EAgentToolState::Finished || Item->ToolState == EAgentToolState::NotRun
				? Item->Text : FString(TEXT("(no result yet)"));
			if (ShownBody != Result)
			{
				ShownBody = Result;
				BodyText->SetText(FText::FromString(CutForCard(ShownBody)));
			}
		}
		break;

	default:
		if (ShownPlain != Item->Text || ShownPlain.IsEmpty())
		{
			ShownPlain = Item->Text;
			PlainText->SetText(FText::FromString(ShownPlain));
		}
		break;
	}
}

// --- SCursorChatView -------------------------------------------------------------------------

void SCursorChatView::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)

		// Model picker and chat controls.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[
				SNew(STextBlock)
				.Text(LOCTEXT("ModelLabel", "Model"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SComboButton)
				.ToolTipText(LOCTEXT("ModelTip", "The model that answers. The list comes from the provider; refresh it from this menu."))
				.OnGetMenuContent(this, &SCursorChatView::BuildModelMenu)
				.ButtonContent()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return ModelLabel; })
				]
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(0.0f, 0.0f, 6.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("Retry", "Send again"))
				.ToolTipText(LOCTEXT("RetryTip", "Send the last message again. Offered when a turn failed or was stopped before the model answered."))
				.Visibility_Lambda([this]() { return Session.IsValid() && Session->CanRetry() ? EVisibility::Visible : EVisibility::Collapsed; })
				.OnClicked(this, &SCursorChatView::HandleRetry)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("NewChat", "New chat"))
				.ToolTipText(LOCTEXT("NewChatTip", "Forget this conversation and start an empty one. A running turn is stopped."))
				.IsEnabled_Lambda([this]() { return Session.IsValid() && Session->GetItems().Num() > 0; })
				.OnClicked(this, &SCursorChatView::HandleNewChat)
			]
		]

		// Demo notice, or the reason there is nothing to talk to.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 6.0f))
		[
			SNew(SBorder)
			.BorderImage(FCursorEditorStyle::Brush(TEXT("Bubble.Banner")))
			.Padding(FMargin(10.0f, 6.0f))
			.Visibility_Lambda([this]() { return GetBannerText().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(this, &SCursorChatView::GetBannerText)
			]
		]

		// The conversation.
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(0.0f))
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					SAssignNew(MessageBox, SScrollBox)
					.OnUserScrolled(this, &SCursorChatView::HandleUserScrolled)
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				.Padding(FMargin(24.0f))
				[
					SNew(STextBlock)
					.Visibility_Lambda([this]() { return ItemWidgets.Num() == 0 ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					.Justification(ETextJustify::Center)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor::UseSubduedForeground())
					.Text(LOCTEXT("EmptyHint",
						"Ask for a change, or ask about this project.\n\n"
						"\"Space the selected lights evenly along X.\"\n"
						"\"Which Blueprints in /Game/Characters fail to compile, and why?\"\n"
						"\"Add a sprint ability to the player character in C++.\""))
				]
			]
		]

		// Status line.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 4.0f, 8.0f, 2.0f))
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text(this, &SCursorChatView::GetStatusText)
			.ColorAndOpacity(this, &SCursorChatView::GetStatusColor)
		]

		// What to send along with the message.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 2.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(FMargin(0.0f, 0.0f, 8.0f, 0.0f))
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(LOCTEXT("Attach", "Send with message:"))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				MakeContextToggle(LOCTEXT("CtxSelection", "Selection"),
					LOCTEXT("CtxSelectionTip", "The actors selected in the level and the assets selected in the Content Browser, by name."),
					&SCursorChatView::bIncludeSelection)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				MakeContextToggle(LOCTEXT("CtxOpen", "Open assets"),
					LOCTEXT("CtxOpenTip", "The assets you have open in editors, by path."),
					&SCursorChatView::bIncludeOpenAssets)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				MakeContextToggle(LOCTEXT("CtxLog", "Recent warnings"),
					LOCTEXT("CtxLogTip", "The last warnings and errors from the Output Log."),
					&SCursorChatView::bIncludeLogProblems)
			]
		]

		// The message box.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 4.0f, 8.0f, 8.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SBox)
				.MinDesiredHeight(54.0f)
				.MaxDesiredHeight(180.0f)
				[
					SAssignNew(Input, SMultiLineEditableTextBox)
					.AutoWrapText(true)
					.ModiferKeyForNewLine(EModifierKey::Shift)
					.ClearKeyboardFocusOnCommit(false)
					.HintText(LOCTEXT("InputHint", "Message the agent. Enter sends, Shift+Enter starts a new line."))
					.OnTextCommitted(this, &SCursorChatView::HandleInputCommitted)
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Bottom)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
				.IsEnabled_Lambda([this]() { return Session.IsValid(); })
				.Text_Lambda([this]() { return Session.IsValid() && Session->IsBusy() ? LOCTEXT("Stop", "Stop") : LOCTEXT("Send", "Send"); })
				.OnClicked(this, &SCursorChatView::HandleSendOrStop)
			]
		]
	];

	FCursorEditorState::Get().OnSessionReplaced.AddSP(this, &SCursorChatView::BindSession);
	BindSession();
}

SCursorChatView::~SCursorChatView()
{
	UnbindSession();
	FCursorEditorState::Get().OnSessionReplaced.RemoveAll(this);
}

TSharedRef<SWidget> SCursorChatView::MakeContextToggle(const FText& Label, const FText& Tooltip, bool SCursorChatView::* Flag)
{
	return SNew(SCheckBox)
		.ToolTipText(Tooltip)
		.Padding(FMargin(4.0f, 0.0f, 12.0f, 0.0f))
		.IsChecked_Lambda([this, Flag]() { return this->*Flag ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this, Flag](ECheckBoxState State) { this->*Flag = State == ECheckBoxState::Checked; })
		[
			SNew(STextBlock)
			.Text(Label)
		];
}

void SCursorChatView::BindSession()
{
	UnbindSession();
	Session = FCursorEditorState::Get().GetSession();
	if (Session.IsValid())
	{
		Session->OnItemAdded.AddSP(this, &SCursorChatView::HandleItemAdded);
		Session->OnItemChanged.AddSP(this, &SCursorChatView::HandleItemChanged);
		Session->OnItemsReset.AddSP(this, &SCursorChatView::HandleItemsReset);
		Session->OnStateChanged.AddSP(this, &SCursorChatView::HandleStateChanged);
	}
	Note.Reset();
	bFetchingModels = false;
	UpdateModelLabel();
	RebuildItems();
	// A provider chosen for the first time has no list on disk yet.
	FetchModelsIfMissing();
}

void SCursorChatView::UnbindSession()
{
	if (Session.IsValid())
	{
		Session->OnItemAdded.RemoveAll(this);
		Session->OnItemChanged.RemoveAll(this);
		Session->OnItemsReset.RemoveAll(this);
		Session->OnStateChanged.RemoveAll(this);
	}
	Session.Reset();
}

void SCursorChatView::RebuildItems()
{
	MessageBox->ClearChildren();
	ItemWidgets.Reset();
	DirtyItems.Reset();
	if (Session.IsValid())
	{
		for (const TSharedRef<FAgentItem>& Item : Session->GetItems())
		{
			AddItemWidget(Item);
		}
	}
	bScrollPending = true;
}

void SCursorChatView::AddItemWidget(const TSharedRef<FAgentItem>& Item)
{
	const TSharedRef<SCursorChatItem> Widget = SNew(SCursorChatItem, Item);
	// The user's own messages are set in from the left, so the two sides of the conversation are easy to tell apart.
	const float Left = Item->Kind == EAgentItemKind::User ? 48.0f : 10.0f;
	MessageBox->AddSlot()
		.Padding(FMargin(Left, 5.0f, 10.0f, 5.0f))
		[
			Widget
		];
	ItemWidgets.Add(Item, Widget);
}

void SCursorChatView::HandleItemAdded(const TSharedRef<FAgentItem>& Item)
{
	AddItemWidget(Item);
	bScrollPending = true;
}

void SCursorChatView::HandleItemChanged(const TSharedRef<FAgentItem>& Item)
{
	// Text arrives many times a frame. Lay it out once, on the next tick.
	DirtyItems.Add(Item);
}

void SCursorChatView::HandleItemsReset()
{
	RebuildItems();
}

void SCursorChatView::HandleStateChanged()
{
	if (Session.IsValid() && Session->IsBusy())
	{
		Note.Reset();
	}
}

void SCursorChatView::HandleUserScrolled(float Offset)
{
	// Follow new text only while the user is reading the end of the conversation.
	bStickToBottom = Offset >= MessageBox->GetScrollOffsetOfEnd() - 8.0f;
}

void SCursorChatView::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
	FlushPending();

	// The model can also be changed in Project Settings, which does not tell this widget.
	SecondsSinceLabelUpdate += InDeltaTime;
	if (SecondsSinceLabelUpdate > 0.5f)
	{
		UpdateModelLabel();
	}
}

void SCursorChatView::FlushPending()
{
	if (DirtyItems.Num() > 0)
	{
		for (const TSharedPtr<FAgentItem>& Item : DirtyItems)
		{
			if (const TSharedPtr<SCursorChatItem>* Widget = ItemWidgets.Find(Item))
			{
				(*Widget)->Refresh();
			}
		}
		DirtyItems.Reset();
		bScrollPending = true;
	}
	if (bScrollPending)
	{
		bScrollPending = false;
		if (bStickToBottom)
		{
			MessageBox->ScrollToEnd();
		}
	}
}

void SCursorChatView::SetAllExpanded(bool bExpanded)
{
	for (const TPair<TSharedPtr<FAgentItem>, TSharedPtr<SCursorChatItem>>& Pair : ItemWidgets)
	{
		Pair.Value->SetExpanded(bExpanded);
	}
}

// --- Sending ---------------------------------------------------------------------------------

void SCursorChatView::Send()
{
	if (!Session.IsValid())
	{
		SetNote(FCursorEditorState::Get().GetProblem().ToString(), true);
		return;
	}
	if (Session->IsBusy())
	{
		return;
	}
	FText WhyNot;
	if (Session->Send(Input->GetText().ToString(), GatherContext(), WhyNot))
	{
		Input->SetText(FText::GetEmpty());
		bStickToBottom = true;
		bScrollPending = true;
	}
	else
	{
		SetNote(WhyNot.ToString(), true);
	}
}

FReply SCursorChatView::HandleSendOrStop()
{
	if (Session.IsValid() && Session->IsBusy())
	{
		Session->Stop();
	}
	else
	{
		Send();
	}
	return FReply::Handled();
}

FReply SCursorChatView::HandleNewChat()
{
	if (Session.IsValid())
	{
		Session->NewChat();
		Note.Reset();
		bStickToBottom = true;
	}
	return FReply::Handled();
}

FReply SCursorChatView::HandleRetry()
{
	if (Session.IsValid())
	{
		FText WhyNot;
		if (!Session->Retry(WhyNot))
		{
			SetNote(WhyNot.ToString(), true);
		}
	}
	return FReply::Handled();
}

void SCursorChatView::HandleInputCommitted(const FText& Text, ETextCommit::Type CommitType)
{
	if (CommitType == ETextCommit::OnEnter)
	{
		Send();
	}
}

TArray<FAgentContextBlock> SCursorChatView::GatherContext() const
{
	TArray<FAgentContextBlock> Blocks;

	if (bIncludeSelection && GEditor)
	{
		FString Text;
		int32 NumActors = 0;
		for (FSelectionIterator It(*GEditor->GetSelectedActors()); It; ++It)
		{
			if (const AActor* Actor = Cast<AActor>(*It))
			{
				if (++NumActors <= MaxListedInContext)
				{
					Text += FString::Printf(TEXT("- actor %s (label \"%s\", class %s)\n"),
						*Actor->GetName(), *Actor->GetActorLabel(false), *Actor->GetClass()->GetName());
				}
			}
		}
		if (NumActors > MaxListedInContext)
		{
			Text += FString::Printf(TEXT("- and %d more selected actors\n"), NumActors - MaxListedInContext);
		}

		// The Content Browser only exists when the editor has a window.
		if (FSlateApplication::IsInitialized() && FApp::CanEverRender())
		{
			TArray<FAssetData> Assets;
			FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(Assets);
			for (int32 Index = 0; Index < Assets.Num() && Index < MaxListedInContext; ++Index)
			{
				Text += FString::Printf(TEXT("- asset %s (%s)\n"), *Assets[Index].GetObjectPathString(), *Assets[Index].AssetClassPath.GetAssetName().ToString());
			}
			if (Assets.Num() > MaxListedInContext)
			{
				Text += FString::Printf(TEXT("- and %d more selected assets\n"), Assets.Num() - MaxListedInContext);
			}
		}
		if (!Text.IsEmpty())
		{
			Blocks.Add({ TEXT("Selected in the editor when this message was sent"), Text });
		}
	}

	if (bIncludeOpenAssets && GEditor)
	{
		if (UAssetEditorSubsystem* AssetEditors = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
		{
			FString Text;
			for (const UObject* Asset : AssetEditors->GetAllEditedAssets())
			{
				if (Asset)
				{
					Text += FString::Printf(TEXT("- %s (%s)\n"), *Asset->GetPathName(), *Asset->GetClass()->GetName());
				}
			}
			if (!Text.IsEmpty())
			{
				Blocks.Add({ TEXT("Assets open in editors"), Text });
			}
		}
	}

	if (bIncludeLogProblems)
	{
		FString Text;
		for (const FCursorLogLine& Line : FCursorLogCapture::Get().GetLines(0, 30, ELogVerbosity::Warning))
		{
			Text += FCursorLogCapture::Format(Line) + TEXT("\n");
		}
		if (!Text.IsEmpty())
		{
			Blocks.Add({ TEXT("Most recent warnings and errors in the Output Log"), Text });
		}
	}
	return Blocks;
}

// --- Model picker ----------------------------------------------------------------------------

void SCursorChatView::UpdateModelLabel()
{
	SecondsSinceLabelUpdate = 0.0f;
	if (!Session.IsValid())
	{
		ModelLabel = LOCTEXT("NoProvider", "Not available");
		return;
	}
	const FString Id = Session->GetBackend().GetModelId();
	if (Id.IsEmpty())
	{
		ModelLabel = LOCTEXT("ChooseModel", "Choose a model");
		return;
	}
	for (const FAgentModelInfo& Model : Session->GetBackend().GetCachedModels())
	{
		if (Model.Id == Id && !Model.DisplayName.IsEmpty())
		{
			ModelLabel = FText::FromString(Model.DisplayName);
			return;
		}
	}
	ModelLabel = FText::FromString(Id);
}

TSharedRef<SWidget> SCursorChatView::BuildModelMenu()
{
	FMenuBuilder Menu(true, nullptr);
	if (!Session.IsValid())
	{
		Menu.AddWidget(SNew(STextBlock).Text(FCursorEditorState::Get().GetProblem()), FText::GetEmpty());
		return Menu.MakeWidget();
	}

	const TArray<FAgentModelInfo> Models = Session->GetBackend().GetCachedModels();
	const FString Current = Session->GetBackend().GetModelId();

	Menu.BeginSection(NAME_None, LOCTEXT("ModelsSection", "Models"));
	for (const FAgentModelInfo& Model : Models)
	{
		const FString Id = Model.Id;
		FString Label = Model.DisplayName.IsEmpty() ? Id : Model.DisplayName;
		if (Model.bDeprecated)
		{
			Label += TEXT("  (being retired)");
		}
		Menu.AddMenuEntry(
			FText::FromString(Label),
			FText::FromString(Id),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP(this, &SCursorChatView::ChooseModel, Id),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([Current, Id]() { return Current == Id; })),
			NAME_None,
			EUserInterfaceActionType::RadioButton);
	}
	if (Models.Num() == 0)
	{
		FetchModelsIfMissing();
		Menu.AddWidget(
			SNew(SBox)
			.Padding(FMargin(12.0f, 4.0f))
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(bFetchingModels
					? LOCTEXT("FetchingModels", "Fetching the model list. Open this menu again in a moment.")
					: LOCTEXT("NoModels", "No model list yet. Add a key in the Setup tab, then refresh."))
			],
			FText::GetEmpty());
	}
	Menu.EndSection();

	Menu.BeginSection(NAME_None, FText::GetEmpty());
	Menu.AddMenuEntry(
		LOCTEXT("RefreshModels", "Refresh the list"),
		LOCTEXT("RefreshModelsTip", "Ask the provider which models this key can use."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Refresh"),
		FUIAction(FExecuteAction::CreateSP(this, &SCursorChatView::RefreshModels)));
	Menu.EndSection();

	return Menu.MakeWidget();
}

void SCursorChatView::ChooseModel(FString ModelId)
{
	if (Session.IsValid())
	{
		Session->GetBackend().SetModelId(ModelId);
		UpdateModelLabel();
		Note.Reset();
	}
}

void SCursorChatView::RefreshModels()
{
	if (!Session.IsValid())
	{
		return;
	}
	SetNote(TEXT("Fetching the model list..."), false);
	const TWeakPtr<SCursorChatView> Weak = SharedThis(this);
	Session->GetBackend().RefreshModels([Weak](bool bOk, const TArray<FAgentModelInfo>& Models, const FString& Error)
	{
		if (const TSharedPtr<SCursorChatView> Self = Weak.Pin())
		{
			Self->SetNote(bOk ? FString::Printf(TEXT("Model list updated: %d models."), Models.Num()) : Error, !bOk);
			Self->UpdateModelLabel();
		}
	});
}

void SCursorChatView::FetchModelsIfMissing()
{
	// Tests and screenshots must never reach a provider on their own.
	if (!Session.IsValid() || bFetchingModels || FApp::IsUnattended() || GIsAutomationTesting)
	{
		return;
	}
	if (Session->GetBackend().GetCachedModels().Num() > 0)
	{
		return;
	}
	bFetchingModels = true;
	const TWeakPtr<SCursorChatView> Weak = SharedThis(this);
	const TWeakPtr<FAgentSession> ForSession = Session;
	Session->GetBackend().RefreshModels([Weak, ForSession](bool bOk, const TArray<FAgentModelInfo>& Models, const FString&)
	{
		const TSharedPtr<SCursorChatView> Self = Weak.Pin();
		if (!Self.IsValid() || Self->Session != ForSession.Pin())
		{
			return;
		}
		Self->bFetchingModels = false;
		// Failing is the usual case before a key is saved, and the Model menu already says what to do.
		if (bOk)
		{
			Self->SetNote(FString::Printf(TEXT("Model list fetched: %d models. Pick one from the Model menu."), Models.Num()), false);
			Self->UpdateModelLabel();
		}
	});
}

// --- Status ----------------------------------------------------------------------------------

void SCursorChatView::SetNote(const FString& Text, bool bIsError)
{
	Note = Text;
	bNoteIsError = bIsError;
}

FText SCursorChatView::GetBannerText() const
{
	if (!Session.IsValid())
	{
		return FCursorEditorState::Get().GetProblem();
	}
	return FText::FromString(Session->GetBackend().GetBanner());
}

FSlateColor SCursorChatView::GetStatusColor() const
{
	return !Note.IsEmpty() && bNoteIsError ? FSlateColor(FStyleColors::Error) : FSlateColor::UseSubduedForeground();
}

FText SCursorChatView::GetStatusText() const
{
	if (!Note.IsEmpty())
	{
		return FText::FromString(Note);
	}
	if (!Session.IsValid())
	{
		return FText::GetEmpty();
	}

	if (Session->IsBusy())
	{
		const FCursorTurnPerf& Turn = Session->GetLastTurn();
		const double Seconds = (FDateTime::UtcNow() - Turn.StartedAt).GetTotalSeconds();
		return FText::Format(LOCTEXT("Working", "Working: {0} s, {1} {1}|plural(one=request,other=requests) to the model, {2} tool {2}|plural(one=call,other=calls)"),
			FText::AsNumber(FMath::FloorToInt(Seconds)), FText::AsNumber(Turn.RoundTrips), FText::AsNumber(Turn.ToolCalls));
	}

	const FAgentUsage& Usage = Session->GetUsage();
	if (Usage.TotalInput() == 0 && Usage.OutputTokens == 0)
	{
		return FText::GetEmpty();
	}
	const int32 CachedPercent = Usage.TotalInput() > 0
		? FMath::RoundToInt(100.0 * static_cast<double>(Usage.CacheReadTokens) / static_cast<double>(Usage.TotalInput())) : 0;
	return FText::Format(LOCTEXT("ChatUsage", "This chat: {0} tokens in ({1}% read from cache), {2} out. Last turn took {3} s."),
		Thousands(Usage.TotalInput()), FText::AsNumber(CachedPercent), Thousands(Usage.OutputTokens),
		FText::AsNumber(Session->GetLastTurn().TotalMs / 1000.0, &FNumberFormattingOptions().SetMinimumFractionalDigits(1).SetMaximumFractionalDigits(1)));
}

#undef LOCTEXT_NAMESPACE
