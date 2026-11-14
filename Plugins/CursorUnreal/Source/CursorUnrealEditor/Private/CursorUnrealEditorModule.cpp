#include "Containers/Ticker.h"
#include "CursorEditorState.h"
#include "CursorEditorStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "SCursorUnrealPanel.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "CursorUnrealEditor"

namespace
{
	const FName PanelTabId(TEXT("CursorUnrealPanel"));
}

class FCursorUnrealEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Commandlets (cooking, packaging) and anything else without a UI have no use for the panel.
		if (IsRunningCommandlet() || !FSlateApplication::IsInitialized())
		{
			return;
		}
		bStarted = true;

		FCursorEditorStyle::Register();

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(PanelTabId, FOnSpawnTab::CreateRaw(this, &FCursorUnrealEditorModule::SpawnPanel))
			.SetDisplayName(LOCTEXT("TabTitle", "Cursor Unreal"))
			.SetTooltipText(LOCTEXT("TabTooltip", "Chat with an AI agent that can work in this editor, and see what it is doing."))
			.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
			.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Comment"));

		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FCursorUnrealEditorModule::RegisterMenus));

		// An unattended editor has nobody to ask. With no prompt installed, anything that needs approval is refused.
		if (!FApp::IsUnattended())
		{
			CursorApprovalUI::Install();
			bInstalledApproval = true;
		}

		FString ShotDirectory;
		if (CursorPanelShots::IsRequested(ShotDirectory))
		{
			// Wait for the editor to finish starting, then a little longer for fonts and styles to settle.
			LoopInitHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([ShotDirectory]()
			{
				FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([ShotDirectory](float)
				{
					CursorPanelShots::RunAndExit(ShotDirectory);
					return false;
				}), 3.0f);
			});
		}
	}

	virtual void ShutdownModule() override
	{
		if (!bStarted)
		{
			return;
		}
		FCoreDelegates::OnFEngineLoopInitComplete.Remove(LoopInitHandle);
		if (bInstalledApproval)
		{
			CursorApprovalUI::Uninstall();
		}

		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PanelTabId);
		}

		// Ends a running turn and lets go of the backend before the modules it uses unload.
		FCursorEditorState::Get().Shutdown();
		FCursorEditorStyle::Unregister();
	}

private:
	TSharedRef<SDockTab> SpawnPanel(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SCursorUnrealPanel)
			];
	}

	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);
		UToolMenu* Toolbar = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User");
		FToolMenuSection& Section = Toolbar->FindOrAddSection("CursorUnreal");
		Section.AddEntry(FToolMenuEntry::InitToolBarButton(
			"OpenCursorUnreal",
			FUIAction(FExecuteAction::CreateLambda([]() { FGlobalTabmanager::Get()->TryInvokeTab(PanelTabId); })),
			LOCTEXT("ToolbarLabel", "Agent"),
			LOCTEXT("ToolbarTooltip", "Open the Cursor Unreal panel."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Comment")));
	}

	FDelegateHandle LoopInitHandle;
	bool bStarted = false;
	bool bInstalledApproval = false;
};

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FCursorUnrealEditorModule, CursorUnrealEditor);
