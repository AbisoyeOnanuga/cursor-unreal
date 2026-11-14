#pragma once

#include "CoreMinimal.h"
#include "AgentSession.h"
#include "CursorMarkdown.h"
#include "Widgets/SCompoundWidget.h"

class SCursorImageStrip;
class SMultiLineEditableText;
class SMultiLineEditableTextBox;
class SScrollBox;
class SVerticalBox;

/** One entry in the chat: a message, a block of thinking, a tool call or a notice. */
class SCursorChatItem : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCursorChatItem) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<FAgentItem>& InItem);

	/** Brings the widget up to date with its item. Cheap when nothing changed. */
	void Refresh();
	void SetExpanded(bool bInExpanded);

private:
	TSharedRef<SWidget> MakeCollapsible(const TSharedRef<SWidget>& Body);
	TSharedRef<SWidget> MakeCodeBlock(const FString& Language, TFunction<void(const FString&)>& OutSetter);
	void RefreshAssistant();
	FText GetCardTitle() const;
	FText GetCardPreview() const;
	FSlateColor GetCardTitleColor() const;

	TSharedPtr<FAgentItem> Item;
	bool bExpanded = false;

	/** Pictures sent with a message, or returned by a tool. Always in view, whether or not the card is open. */
	TSharedPtr<SCursorImageStrip> ImageStrip;

	// Messages, notices and errors.
	TSharedPtr<SMultiLineEditableText> PlainText;
	FString ShownPlain;

	// Assistant replies: prose and code blocks, rebuilt only when their arrangement changes.
	TSharedPtr<SVerticalBox> SegmentBox;
	TArray<CursorMarkdown::FSegment> ShownSegments;
	TArray<TFunction<void(const FString&)>> SegmentSetters;

	// Tool calls and thinking.
	TSharedPtr<SMultiLineEditableText> ArgsText;
	TSharedPtr<SMultiLineEditableText> BodyText;
	FString ShownArgs;
	FString ShownBody;
};

/** The conversation, the box you type in, and the controls around them. */
class SCursorChatView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCursorChatView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCursorChatView() override;

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	/** Applies changes that are waiting for the next tick. Screenshots and tests call it because they do not tick. */
	void FlushPending();
	int32 NumItemWidgets() const { return ItemWidgets.Num(); }
	void SetAllExpanded(bool bExpanded);

	/** Attaches pictures to the message being written. Returns how many were taken; the rest did not fit, and the status line says why. */
	int32 AttachImages(const TArray<FCursorImage>& Images);
	/** Reads picture files and attaches them. Files that are not pictures are named in the status line. */
	int32 AttachFiles(const TArray<FString>& Files);
	int32 NumAttachedImages() const { return PendingImages.Num(); }
	/** What Enter and the Send button do. */
	void Send();

	virtual FReply OnDragOver(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;

private:
	void BindSession();
	void UnbindSession();
	void RebuildItems();
	void AddItemWidget(const TSharedRef<FAgentItem>& Item);

	void HandleItemAdded(const TSharedRef<FAgentItem>& Item);
	void HandleItemChanged(const TSharedRef<FAgentItem>& Item);
	void HandleItemsReset();
	void HandleStateChanged();
	void HandleUserScrolled(float Offset);

	FReply HandleSendOrStop();
	FReply HandleAttachClicked();
	FReply HandleInputKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent);
	void RemoveAttachedImage(int32 Index);
	void RefreshAttachedStrip();
	FReply HandleNewChat();
	FReply HandleRetry();
	void HandleInputCommitted(const FText& Text, ETextCommit::Type CommitType);

	TSharedRef<SWidget> BuildModelMenu();
	void ChooseModel(FString ModelId);
	void RefreshModels();
	/** Fetches the model list without being asked, when there is none yet. Quiet when it cannot (no key so far). */
	void FetchModelsIfMissing();
	void UpdateModelLabel();

	TArray<FAgentContextBlock> GatherContext() const;
	TSharedRef<SWidget> MakeContextToggle(const FText& Label, const FText& Tooltip, bool SCursorChatView::* Flag);

	FText GetStatusText() const;
	FSlateColor GetStatusColor() const;
	FText GetBannerText() const;
	void SetNote(const FString& Text, bool bIsError);

	TSharedPtr<FAgentSession> Session;
	TSharedPtr<SScrollBox> MessageBox;
	TSharedPtr<SMultiLineEditableTextBox> Input;

	/** Pictures that will go with the next message. */
	TArray<FCursorImage> PendingImages;
	TSharedPtr<SCursorImageStrip> PendingStrip;

	TMap<TSharedPtr<FAgentItem>, TSharedPtr<SCursorChatItem>> ItemWidgets;
	TSet<TSharedPtr<FAgentItem>> DirtyItems;
	bool bScrollPending = false;
	bool bStickToBottom = true;

	FText ModelLabel;
	float SecondsSinceLabelUpdate = 0.0f;
	bool bFetchingModels = false;

	/** Something to tell the user in the status line: why a message could not be sent, how a refresh went. */
	FString Note;
	bool bNoteIsError = false;

	bool bIncludeSelection = true;
	bool bIncludeOpenAssets = false;
	bool bIncludeLogProblems = false;
};
