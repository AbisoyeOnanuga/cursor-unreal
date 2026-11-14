#pragma once

#include "CoreMinimal.h"
#include "CursorSecrets.h"
#include "CursorSkillLibrary.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SEditableTextBox;
struct FPropertyChangedEvent;

/** The skills the in-editor agent is offered, where each came from, and the way to make one your own. */
class SCursorSkillsView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCursorSkillsView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCursorSkillsView() override;

private:
	using FSkillPtr = TSharedPtr<FCursorSkillInfo>;

	void Reload();
	TSharedRef<ITableRow> MakeRow(FSkillPtr Skill, const TSharedRef<STableViewBase>& Owner);
	void SetEnabled(const FCursorSkillInfo& Skill, bool bEnabled);
	FReply HandleInstallAll();
	FReply HandleCustomise(FString Name);
	FReply HandleOpen(FString SkillFile);
	FReply HandleOpenProjectFolder();

	TArray<FSkillPtr> Rows;
	TSharedPtr<SListView<FSkillPtr>> List;
	FText Note;
};

/** Keys, connections, and every setting. Keys go to the OS credential store and are never shown again. */
class SCursorSetupView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCursorSetupView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	struct FKeyRow
	{
		const TCHAR* SecretName = nullptr;
		const TCHAR* EnvVar = nullptr;
		FText Label;
		FText Status;
		TSharedPtr<SEditableTextBox> Box;
	};

	TSharedRef<SWidget> MakeKeyRow(FKeyRow& Row);
	void RefreshKeyStatus(FKeyRow& Row);
	FReply HandleSaveKey(FKeyRow* Row);
	FReply HandleRemoveKey(FKeyRow* Row);

	FReply HandleCopyMcpConfig();
	FReply HandleInstallMcpConfig();
	FReply HandleNewMcpToken();
	void HandleSettingsEdited(const FPropertyChangedEvent& Event);
	void SetNote(const FString& Text, bool bIsError);

	FKeyRow AnthropicKey;
	FKeyRow CursorKey;
	FText BridgeStatus;
	FText Note;
	bool bNoteIsError = false;
};
