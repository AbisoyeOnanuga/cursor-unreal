#include "SCursorSetupViews.h"

#include "AgentSession.h"
#include "CursorBridgeInstall.h"
#include "CursorEditorState.h"
#include "CursorEditorStyle.h"
#include "CursorMcpServer.h"
#include "CursorUnrealSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "IDetailsView.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "CursorUnrealSetup"

namespace
{
	FSlateFontInfo HeadingFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 10);
	}

	FText LayerLabel(const FCursorSkillInfo& Skill)
	{
		switch (Skill.Layer)
		{
		case ECursorSkillLayer::Project:
			if (Skill.bInstalledUnmodified)
			{
				return LOCTEXT("LayerProjectCopy", "Project (unedited copy of the plugin's)");
			}
			return Skill.bOverridesLowerLayer ? LOCTEXT("LayerProjectOverride", "Project (replaces the plugin's)") : LOCTEXT("LayerProject", "Project");
		case ECursorSkillLayer::User:
			return Skill.bOverridesLowerLayer ? LOCTEXT("LayerUserOverride", "Yours (replaces the plugin's)") : LOCTEXT("LayerUser", "Yours (~/.cursor/skills)");
		default:
			return LOCTEXT("LayerBuiltIn", "Plugin");
		}
	}
}

// --- Skills ----------------------------------------------------------------------------------

void SCursorSkillsView::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("SkillsIntro",
				"Skills are short guides the agent reads before work they cover. Ticked skills are offered to the in-editor agent. "
				"Cursor's own agents read the project's .cursor/skills folder instead, so install the skills there to share them with Cursor and with your team. "
				"To change a skill, customise it: your project copy then replaces the plugin's and is never overwritten."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("InstallAll", "Install all into this project"))
				.ToolTipText(LOCTEXT("InstallAllTip", "Copy the plugin's skills into <Project>/.cursor/skills. Copies you have edited are left alone."))
				.OnClicked(this, &SCursorSkillsView::HandleInstallAll)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("OpenProjectSkills", "Open project skills folder"))
				.OnClicked(this, &SCursorSkillsView::HandleOpenProjectFolder)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("Rescan", "Rescan"))
				.ToolTipText(LOCTEXT("RescanTip", "Look for skills again. Changes on disk are normally picked up on their own."))
				.OnClicked_Lambda([]()
				{
					FCursorSkillLibrary::Get().Refresh();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 4.0f))
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Visibility_Lambda([this]() { return Note.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this]() { return Note; })
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SAssignNew(List, SListView<FSkillPtr>)
			.ListItemsSource(&Rows)
			.SelectionMode(ESelectionMode::None)
			.OnGenerateRow(this, &SCursorSkillsView::MakeRow)
		]
	];

	FCursorSkillLibrary::Get().OnChanged.AddSP(this, &SCursorSkillsView::Reload);
	Reload();
}

SCursorSkillsView::~SCursorSkillsView()
{
	FCursorSkillLibrary::Get().OnChanged.RemoveAll(this);
}

void SCursorSkillsView::Reload()
{
	Rows.Reset();
	for (const FCursorSkillInfo& Skill : FCursorSkillLibrary::Get().List())
	{
		Rows.Add(MakeShared<FCursorSkillInfo>(Skill));
	}
	List->RequestListRefresh();
}

TSharedRef<ITableRow> SCursorSkillsView::MakeRow(FSkillPtr Skill, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(STableRow<FSkillPtr>, Owner)
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(FMargin(0.0f, 2.0f, 8.0f, 0.0f))
			[
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("EnableTip", "Offer this skill to the in-editor agent."))
				.IsChecked(Skill->bEnabled ? ECheckBoxState::Checked : ECheckBoxState::Unchecked)
				.OnCheckStateChanged_Lambda([this, Skill](ECheckBoxState State) { SetEnabled(*Skill, State == ECheckBoxState::Checked); })
			]
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					.AutoWidth()
					[
						SNew(STextBlock)
						.Font(HeadingFont())
						.Text(FText::FromString(Skill->Name))
					]
					+ SHorizontalBox::Slot()
					.AutoWidth()
					.Padding(FMargin(10.0f, 0.0f, 0.0f, 0.0f))
					.VAlign(VAlign_Bottom)
					[
						SNew(STextBlock)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(LayerLabel(*Skill))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 2.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text(FText::FromString(Skill->Description))
				]
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(FMargin(8.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("Customise", "Customise"))
				.ToolTipText(LOCTEXT("CustomiseTip", "Copy this skill into the project and open the copy. From then on your copy is the one used."))
				.Visibility(Skill->Layer == ECursorSkillLayer::BuiltIn ? EVisibility::Visible : EVisibility::Collapsed)
				.OnClicked(this, &SCursorSkillsView::HandleCustomise, Skill->Name)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Top)
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("OpenSkill", "Open"))
				.ToolTipText(FText::FromString(Skill->SkillFile))
				.OnClicked(this, &SCursorSkillsView::HandleOpen, Skill->SkillFile)
			]
		];
}

void SCursorSkillsView::SetEnabled(const FCursorSkillInfo& Skill, bool bEnabled)
{
	UCursorUnrealSettings* Settings = UCursorUnrealSettings::GetMutable();
	// Skills from the user's own folder are opt-in, unless they stand in for one of the plugin's.
	const bool bOptIn = Skill.Layer == ECursorSkillLayer::User && !Skill.bOverridesLowerLayer;
	if (bEnabled)
	{
		Settings->DisabledSkills.Remove(Skill.Name);
		if (bOptIn)
		{
			Settings->EnabledUserSkills.AddUnique(Skill.Name);
		}
	}
	else if (bOptIn)
	{
		Settings->EnabledUserSkills.Remove(Skill.Name);
	}
	else
	{
		Settings->DisabledSkills.AddUnique(Skill.Name);
	}
	Settings->SaveConfig();
	Note = LOCTEXT("SkillToggleNote", "Takes effect in the next new chat: a chat keeps the skill list it started with.");
	FCursorSkillLibrary::Get().Refresh();
}

FReply SCursorSkillsView::HandleInstallAll()
{
	const FCursorSkillInstallReport Report = FCursorSkillLibrary::Get().InstallBuiltInsToProject();
	Note = FText::FromString(Report.ToText());
	return FReply::Handled();
}

FReply SCursorSkillsView::HandleCustomise(FString Name)
{
	FCursorSkillLibrary& Library = FCursorSkillLibrary::Get();
	const FCursorSkillInstallReport Report = Library.InstallBuiltInsToProject(Name);
	Note = FText::FromString(Report.ToText());

	const FString Copy = Library.GetDirs().Project / Name / TEXT("SKILL.md");
	if (IFileManager::Get().FileExists(*Copy))
	{
		FPlatformProcess::LaunchFileInDefaultExternalApplication(*Copy);
	}
	return FReply::Handled();
}

FReply SCursorSkillsView::HandleOpen(FString SkillFile)
{
	if (IFileManager::Get().FileExists(*SkillFile))
	{
		FPlatformProcess::LaunchFileInDefaultExternalApplication(*SkillFile);
	}
	return FReply::Handled();
}

FReply SCursorSkillsView::HandleOpenProjectFolder()
{
	const FString Directory = FCursorSkillLibrary::Get().GetDirs().Project;
	IFileManager::Get().MakeDirectory(*Directory, true);
	FPlatformProcess::ExploreFolder(*Directory);
	return FReply::Handled();
}

// --- Setup -----------------------------------------------------------------------------------

void SCursorSetupView::Construct(const FArguments& InArgs)
{
	AnthropicKey.SecretName = CursorSecrets::AnthropicApiKey;
	AnthropicKey.EnvVar = TEXT("ANTHROPIC_API_KEY");
	AnthropicKey.Label = LOCTEXT("AnthropicKey", "Anthropic API key");
	CursorKey.SecretName = CursorSecrets::CursorApiKey;
	CursorKey.EnvVar = TEXT("CURSOR_API_KEY");
	CursorKey.Label = LOCTEXT("CursorKey", "Cursor API key");
	RefreshKeyStatus(AnthropicKey);
	RefreshKeyStatus(CursorKey);
	BridgeStatus = FText::FromString(CursorBridge::ProbeInstall().StatusLine());

	FDetailsViewArgs DetailsArgs;
	DetailsArgs.bAllowSearch = true;
	DetailsArgs.bHideSelectionTip = true;
	DetailsArgs.bShowOptions = false;
	DetailsArgs.bLockable = false;
	DetailsArgs.bUpdatesFromSelection = false;
	DetailsArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	const TSharedRef<IDetailsView> SettingsView =
		FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor")).CreateDetailView(DetailsArgs);
	SettingsView->SetObject(UCursorUnrealSettings::GetMutable());
	SettingsView->OnFinishedChangingProperties().AddSP(this, &SCursorSetupView::HandleSettingsEdited);

	ChildSlot
	[
		SNew(SVerticalBox)

		// Keys.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 8.0f, 10.0f, 2.0f))
		[
			SNew(STextBlock)
			.Font(HeadingFont())
			.Text(LOCTEXT("KeysHeading", "API keys"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 0.0f, 10.0f, 4.0f))
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("KeysIntro",
				"Keys are kept in the operating system's credential store, not in the project or its config files, and are sent only to the provider they belong to. "
				"Saving a key also selects that provider, so a Cursor key alone is enough to load Cursor models — you do not need an Anthropic key."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 2.0f))
		[
			MakeKeyRow(AnthropicKey)
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 2.0f))
		[
			MakeKeyRow(CursorKey)
		]

		// In-editor Cursor provider (Node bridge).
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 10.0f, 10.0f, 2.0f))
		[
			SNew(STextBlock)
			.Font(HeadingFont())
			.Text(LOCTEXT("BridgeHeading", "Cursor provider (in-editor)"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 0.0f, 10.0f, 4.0f))
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text(LOCTEXT("BridgeIntro",
				"When Provider is set to Cursor, the editor starts a local Node process that runs Cursor agents with your Cursor API key. "
				"Models come from your Cursor account. Needs Node.js 22.13+ and `npm install` in the plugin's Bridge folder."))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 0.0f, 10.0f, 4.0f))
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text_Lambda([this]() { return BridgeStatus; })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 2.0f))
		[
			SNew(SButton)
			.Text(LOCTEXT("RefreshBridge", "Check again"))
			.ToolTipText(LOCTEXT("RefreshBridgeTip", "Look for Node and the Cursor SDK again."))
			.OnClicked_Lambda([this]()
			{
				BridgeStatus = FText::FromString(CursorBridge::ProbeInstall().StatusLine());
				return FReply::Handled();
			})
		]

		// Cursor IDE connection.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 10.0f, 10.0f, 2.0f))
		[
			SNew(STextBlock)
			.Font(HeadingFont())
			.Text(LOCTEXT("McpHeading", "Cursor IDE connection (MCP)"))
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 0.0f, 10.0f, 4.0f))
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([]() { return FText::FromString(FCursorMcpServer::Get().GetStatusText()); })
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 2.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("CopyMcp", "Copy config"))
				.ToolTipText(LOCTEXT("CopyMcpTip",
					"Copy the entry for Cursor's mcp.json to the clipboard. It contains this editor's access token, so paste it only into your own Cursor config."))
				.IsEnabled_Lambda([]() { return FCursorMcpServer::Get().IsRunning(); })
				.OnClicked(this, &SCursorSetupView::HandleCopyMcpConfig)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("InstallMcp", "Add to my Cursor config"))
				.ToolTipText(FText::Format(LOCTEXT("InstallMcpTip", "Add or update the cursor-unreal entry in {0}. Everything else in that file is kept."),
					FText::FromString(FCursorMcpServer::GetUserCursorConfigPath())))
				.IsEnabled_Lambda([]() { return FCursorMcpServer::Get().IsRunning(); })
				.OnClicked(this, &SCursorSetupView::HandleInstallMcpConfig)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("NewToken", "New access token"))
				.ToolTipText(LOCTEXT("NewTokenTip", "Replace the token MCP clients must present. Cursor stops connecting until its config is updated with the new one."))
				.OnClicked(this, &SCursorSetupView::HandleNewMcpToken)
			]
		]

		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 6.0f, 10.0f, 2.0f))
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Visibility_Lambda([this]() { return Note.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.ColorAndOpacity_Lambda([this]() { return bNoteIsError ? FSlateColor(FStyleColors::Error) : FSlateColor::UseForeground(); })
			.Text_Lambda([this]() { return Note; })
		]

		// Everything else.
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(10.0f, 10.0f, 10.0f, 4.0f))
		[
			SNew(STextBlock)
			.Font(HeadingFont())
			.Text(LOCTEXT("SettingsHeading", "Settings"))
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SettingsView
		]
	];
}

TSharedRef<SWidget> SCursorSetupView::MakeKeyRow(FKeyRow& Row)
{
	FKeyRow* RowPtr = &Row;
	const bool bCanStore = CursorSecrets::GetStore().IsAvailable();

	return SNew(SHorizontalBox)
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(130.0f)
			[
				SNew(STextBlock)
				.Text(Row.Label)
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.VAlign(VAlign_Center)
		[
			SNew(SBox)
			.WidthOverride(230.0f)
			[
				SAssignNew(Row.Box, SEditableTextBox)
				.IsPassword(true)
				.IsEnabled(bCanStore)
				.HintText(bCanStore ? LOCTEXT("KeyHint", "Paste a key to save or replace it") : LOCTEXT("KeyHintNoStore", "No credential store on this platform"))
			]
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
		[
			SNew(SButton)
			.Text(LOCTEXT("SaveKey", "Save"))
			.IsEnabled(bCanStore)
			.OnClicked(this, &SCursorSetupView::HandleSaveKey, RowPtr)
		]
		+ SHorizontalBox::Slot()
		.AutoWidth()
		.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
		[
			SNew(SButton)
			.Text(LOCTEXT("RemoveKey", "Remove"))
			.ToolTipText(LOCTEXT("RemoveKeyTip", "Delete the saved key from the credential store."))
			.IsEnabled(bCanStore)
			.OnClicked(this, &SCursorSetupView::HandleRemoveKey, RowPtr)
		]
		+ SHorizontalBox::Slot()
		.FillWidth(1.0f)
		.VAlign(VAlign_Center)
		.Padding(FMargin(10.0f, 0.0f, 0.0f, 0.0f))
		[
			SNew(STextBlock)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Text_Lambda([RowPtr]() { return RowPtr->Status; })
		];
}

void SCursorSetupView::RefreshKeyStatus(FKeyRow& Row)
{
	ECursorSecretSource Source = ECursorSecretSource::None;
	// Only where the key is, never the key: the value is dropped as soon as this returns.
	CursorSecrets::Resolve(Row.SecretName, Row.EnvVar, &Source);
	switch (Source)
	{
	case ECursorSecretSource::Store:
		Row.Status = LOCTEXT("KeyInStore", "Saved in the credential store.");
		break;
	case ECursorSecretSource::Environment:
		Row.Status = FText::Format(LOCTEXT("KeyInEnv", "Read from the {0} environment variable."), FText::FromString(Row.EnvVar));
		break;
	default:
		Row.Status = LOCTEXT("KeyMissing", "Not set.");
		break;
	}
}

FReply SCursorSetupView::HandleSaveKey(FKeyRow* Row)
{
	const FString Value = Row->Box->GetText().ToString().TrimStartAndEnd();
	if (Value.IsEmpty())
	{
		SetNote(TEXT("Paste the key into the box first."), true);
		return FReply::Handled();
	}
	if (Value.Contains(TEXT(" ")) || Value.Contains(TEXT("\n")) || Value.Contains(TEXT("\t")))
	{
		SetNote(TEXT("That does not look like a key: it has spaces or line breaks in it."), true);
		return FReply::Handled();
	}

	if (!CursorSecrets::GetStore().Set(Row->SecretName, Value))
	{
		SetNote(TEXT("The credential store refused the key. Nothing was saved."), true);
		return FReply::Handled();
	}
	Row->Box->SetText(FText::GetEmpty());
	RefreshKeyStatus(*Row);

	if (CursorAgent::IsDemoMode())
	{
		SetNote(TEXT("Key saved. This editor is running the offline demo, which does not use it: start the editor without -CursorUnrealDemo."), false);
		return FReply::Handled();
	}

	// Saving a key selects that provider so the model list check talks to the right service.
	const bool bIsCursorKey = Row == &CursorKey;
	const ECursorProvider Wanted = bIsCursorKey ? ECursorProvider::Cursor : ECursorProvider::Anthropic;
	UCursorUnrealSettings* Settings = UCursorUnrealSettings::GetMutable();
	const bool bSwitched = Settings->Provider != Wanted;
	if (bSwitched)
	{
		Settings->Provider = Wanted;
		Settings->SaveConfig();
	}
	// Always rebuild: a new key must reach the backend (and the Cursor bridge must start with it).
	FCursorEditorState::Get().RebuildSession();

	const TSharedPtr<FAgentSession> Session = FCursorEditorState::Get().GetSession();
	if (!Session.IsValid())
	{
		const FString Why = FCursorEditorState::Get().GetProblem().ToString();
		SetNote(Why.IsEmpty()
			? TEXT("Key saved, but the provider could not start.")
			: TEXT("Key saved, but the provider could not start. ") + Why, true);
		return FReply::Handled();
	}

	SetNote(bSwitched
		? FString::Printf(TEXT("Key saved. Provider set to %s. Checking it by asking for the model list..."),
			bIsCursorKey ? TEXT("Cursor") : TEXT("Anthropic"))
		: TEXT("Key saved. Checking it by asking for the model list..."), false);
	const TWeakPtr<SCursorSetupView> Weak = SharedThis(this);
	Session->GetBackend().RefreshModels([Weak](bool bOk, const TArray<FAgentModelInfo>& Models, const FString& Error)
	{
		if (const TSharedPtr<SCursorSetupView> Self = Weak.Pin())
		{
			Self->SetNote(bOk
				? FString::Printf(TEXT("Key saved and accepted. %d models available: pick one from the Model menu in the Chat tab."), Models.Num())
				: TEXT("Key saved, but the check failed. ") + Error, !bOk);
		}
	});
	return FReply::Handled();
}

FReply SCursorSetupView::HandleRemoveKey(FKeyRow* Row)
{
	CursorSecrets::GetStore().Delete(Row->SecretName);
	Row->Box->SetText(FText::GetEmpty());
	RefreshKeyStatus(*Row);
	SetNote(TEXT("Saved key removed."), false);
	return FReply::Handled();
}

FReply SCursorSetupView::HandleCopyMcpConfig()
{
	FPlatformApplicationMisc::ClipboardCopy(*FCursorMcpServer::Get().BuildClientConfigJson(true));
	SetNote(TEXT("Copied. It contains this editor's access token: paste it into your own Cursor mcp.json and nowhere else."), false);
	return FReply::Handled();
}

FReply SCursorSetupView::HandleInstallMcpConfig()
{
	const FText Question = FText::Format(
		LOCTEXT("InstallMcpQuestion", "Add the cursor-unreal server, with its access token, to\n{0}?\n\nThe rest of the file is kept. Reload MCP servers in Cursor afterwards."),
		FText::FromString(FCursorMcpServer::GetUserCursorConfigPath()));
	if (FMessageDialog::Open(EAppMsgType::YesNo, Question) != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}
	FString Message;
	const bool bOk = FCursorMcpServer::Get().InstallIntoUserCursorConfig(Message);
	SetNote(Message, !bOk);
	return FReply::Handled();
}

FReply SCursorSetupView::HandleNewMcpToken()
{
	const FText Question = LOCTEXT("NewTokenQuestion",
		"Replace the access token?\n\nCursor will not be able to connect until its config has the new one (use \"Add to my Cursor config\" or \"Copy config\" afterwards).");
	if (FMessageDialog::Open(EAppMsgType::YesNo, Question) != EAppReturnType::Yes)
	{
		return FReply::Handled();
	}
	if (CursorSecrets::RegenerateMcpToken().IsEmpty())
	{
		SetNote(TEXT("A new token could not be created, so the MCP server has been stopped."), true);
	}
	else
	{
		SetNote(TEXT("New token in place. Update Cursor's config to reconnect."), false);
	}
	// Restart so the server stops accepting the old token.
	FCursorMcpServer::Get().Stop();
	FCursorMcpServer::Get().ApplySettings();
	return FReply::Handled();
}

void SCursorSetupView::HandleSettingsEdited(const FPropertyChangedEvent& Event)
{
	UCursorUnrealSettings::GetMutable()->SaveConfig();
	// Picks up a change of provider. The MCP server follows the settings object's own change event.
	FCursorEditorState::Get().SyncWithSettings();
}

void SCursorSetupView::SetNote(const FString& Text, bool bIsError)
{
	Note = FText::FromString(Text);
	bNoteIsError = bIsError;
}

#undef LOCTEXT_NAMESPACE
