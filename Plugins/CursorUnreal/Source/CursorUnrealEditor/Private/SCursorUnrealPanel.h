#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class SCursorChatView;

/** The Cursor Unreal tab: chat, activity, performance, skills and setup. */
class SCursorUnrealPanel : public SCompoundWidget
{
public:
	enum class ETab : int32
	{
		Chat,
		Activity,
		Performance,
		Skills,
		Setup
	};

	SLATE_BEGIN_ARGS(SCursorUnrealPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetTab(ETab Tab) { ActiveTab = Tab; }
	ETab GetTab() const { return ActiveTab; }
	TSharedPtr<SCursorChatView> GetChatView() const { return ChatView; }

private:
	ETab ActiveTab = ETab::Chat;
	TSharedPtr<SCursorChatView> ChatView;
};

/** Asks the user before a tool call that needs approval runs. */
namespace CursorApprovalUI
{
	void Install();
	void Uninstall();
}

/** Renders each tab of the panel to a PNG, for the documentation. Started with -CursorUnrealShots=<folder>. */
namespace CursorPanelShots
{
	/** True, with the folder, when the command line asks for screenshots. */
	bool IsRequested(FString& OutDirectory);
	/** Plays the offline demo, writes the images, then closes the editor. */
	void RunAndExit(const FString& Directory);
}
