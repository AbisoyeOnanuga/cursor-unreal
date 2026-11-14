#include "SCursorMonitorViews.h"

#include "CursorEditorStyle.h"
#include "CursorTableRow.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Styling/AppStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/SMultiLineEditableText.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "CursorUnrealMonitor"

namespace
{
	FText Millis(double Ms)
	{
		if (Ms < 0.0)
		{
			return FText::FromString(TEXT("-"));
		}
		if (Ms >= 10000.0)
		{
			return FText::FromString(FString::Printf(TEXT("%.1f s"), Ms / 1000.0));
		}
		return FText::FromString(Ms >= 100.0 ? FString::Printf(TEXT("%.0f ms"), Ms) : FString::Printf(TEXT("%.1f ms"), Ms));
	}

	FText LocalClock(const FDateTime& Utc)
	{
		// Records are stamped in UTC; show the time on the user's clock.
		const FDateTime Local = Utc + (FDateTime::Now() - FDateTime::UtcNow());
		return FText::FromString(Local.ToString(TEXT("%H:%M:%S")));
	}

	SHeaderRow::FColumn::FArguments Column(const TCHAR* Id, const FText& Label, float Fill)
	{
		return SHeaderRow::Column(FName(Id)).DefaultLabel(Label).FillWidth(Fill);
	}

	// Column ids.
	const FName ColTime(TEXT("Time"));
	const FName ColTool(TEXT("Tool"));
	const FName ColCaller(TEXT("Caller"));
	const FName ColRisk(TEXT("Risk"));
	const FName ColStatus(TEXT("Status"));
	const FName ColQueue(TEXT("Queue"));
	const FName ColApproval(TEXT("Approval"));
	const FName ColRun(TEXT("Run"));
	const FName ColTransport(TEXT("Transport"));

	const FName ColModel(TEXT("Model"));
	const FName ColOutcome(TEXT("Outcome"));
	const FName ColFirst(TEXT("First"));
	const FName ColTotal(TEXT("Total"));
	const FName ColModelMs(TEXT("ModelMs"));
	const FName ColToolMs(TEXT("ToolMs"));
	const FName ColTrips(TEXT("Trips"));
	const FName ColCalls(TEXT("Calls"));
	const FName ColIn(TEXT("In"));
	const FName ColCached(TEXT("Cached"));
	const FName ColOut(TEXT("Out"));

	const FName ColErrors(TEXT("Errors"));
	const FName ColAvgRun(TEXT("AvgRun"));
	const FName ColMaxRun(TEXT("MaxRun"));
	const FName ColAvgQueue(TEXT("AvgQueue"));
	const FName ColAvgTransport(TEXT("AvgTransport"));
}

// --- Activity --------------------------------------------------------------------------------

void SCursorActivityView::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text(this, &SCursorActivityView::GetSummary)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("ClearActivity", "Clear"))
				.ToolTipText(LOCTEXT("ClearActivityTip", "Empty this list. The performance log on disk is not touched."))
				.OnClicked_Lambda([]()
				{
					FCursorActivityLog::Get().Clear();
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Vertical)
			+ SSplitter::Slot()
			.Value(0.68f)
			[
				SAssignNew(List, SListView<FRecordPtr>)
				.ListItemsSource(&Rows)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &SCursorActivityView::MakeRow)
				.OnSelectionChanged_Lambda([this](FRecordPtr Record, ESelectInfo::Type)
				{
					Selected = Record;
					ShowDetails();
				})
				.HeaderRow(
					SNew(SHeaderRow)
					+ Column(TEXT("Time"), LOCTEXT("ColTime", "Time"), 0.8f)
					+ Column(TEXT("Tool"), LOCTEXT("ColTool", "Tool"), 2.0f)
					+ Column(TEXT("Caller"), LOCTEXT("ColCaller", "Caller"), 0.9f)
					+ Column(TEXT("Risk"), LOCTEXT("ColRisk", "Risk"), 0.9f)
					+ Column(TEXT("Status"), LOCTEXT("ColStatus", "Status"), 1.2f)
					+ Column(TEXT("Queue"), LOCTEXT("ColQueue", "Waited"), 0.8f)
					+ Column(TEXT("Approval"), LOCTEXT("ColApproval", "Approval"), 0.8f)
					+ Column(TEXT("Run"), LOCTEXT("ColRun", "Ran"), 0.8f)
					+ Column(TEXT("Transport"), LOCTEXT("ColTransport", "Transport"), 0.8f))
			]
			+ SSplitter::Slot()
			.Value(0.32f)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
				.Padding(FMargin(8.0f))
				[
					SAssignNew(Details, SMultiLineEditableText)
					.IsReadOnly(true)
					.AutoWrapText(true)
					.Font(FCursorEditorStyle::Mono())
				]
			]
		]
	];

	FCursorActivityLog& Log = FCursorActivityLog::Get();
	Log.OnAdded.AddSP(this, &SCursorActivityView::HandleAdded);
	Log.OnUpdated.AddSP(this, &SCursorActivityView::HandleUpdated);
	Log.OnCleared.AddSP(this, &SCursorActivityView::Reload);
	Reload();
}

SCursorActivityView::~SCursorActivityView()
{
	FCursorActivityLog& Log = FCursorActivityLog::Get();
	Log.OnAdded.RemoveAll(this);
	Log.OnUpdated.RemoveAll(this);
	Log.OnCleared.RemoveAll(this);
}

void SCursorActivityView::Reload()
{
	Rows.Reset();
	const TArray<TSharedRef<FCursorToolCallRecord>>& Records = FCursorActivityLog::Get().GetRecords();
	// Newest first.
	for (int32 Index = Records.Num() - 1; Index >= 0; --Index)
	{
		Rows.Add(Records[Index]);
	}
	if (Selected.IsValid() && !Rows.Contains(Selected))
	{
		Selected.Reset();
	}
	List->RequestListRefresh();
	ShowDetails();
}

void SCursorActivityView::HandleAdded(const TSharedRef<FCursorToolCallRecord>& Record)
{
	Reload();
}

void SCursorActivityView::HandleUpdated(const TSharedRef<FCursorToolCallRecord>& Record)
{
	if (Selected == Record)
	{
		ShowDetails();
	}
}

void SCursorActivityView::ShowDetails()
{
	if (!Selected.IsValid())
	{
		Details->SetText(LOCTEXT("PickACall", "Select a call to see its arguments and the start of its result."));
		return;
	}
	FString Text = FString::Printf(TEXT("%s  (%s, %s)\n\nArguments\n%s\n\nResult (%d characters in all)\n%s"),
		*Selected->ToolName, *Selected->Caller, LexToString(Selected->Status),
		Selected->ArgsPreview.IsEmpty() ? TEXT("(none)") : *Selected->ArgsPreview,
		Selected->ResultChars,
		Selected->ResultPreview.IsEmpty() ? TEXT("(nothing yet)") : *Selected->ResultPreview);
	if (Selected->bRecordedUndo)
	{
		Text += TEXT("\n\nThis call is in the editor's undo history.");
	}
	Details->SetText(FText::FromString(Text));
}

TSharedRef<ITableRow> SCursorActivityView::MakeRow(FRecordPtr Record, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SCursorTextRow<FCursorToolCallRecord>, Owner, Record, &SCursorActivityView::Cell);
}

FText SCursorActivityView::Cell(const FCursorToolCallRecord& Record, FName Column)
{
	if (Column == ColTime) { return LocalClock(Record.StartedAt); }
	if (Column == ColTool) { return FText::FromString(Record.ToolName); }
	if (Column == ColCaller) { return FText::FromString(Record.Caller); }
	if (Column == ColRisk) { return FText::FromString(LexToString(Record.Risk)); }
	if (Column == ColStatus) { return FText::FromString(LexToString(Record.Status)); }
	if (Column == ColQueue) { return Millis(Record.QueueMs); }
	if (Column == ColApproval) { return Record.ApprovalMs > 0.0 ? Millis(Record.ApprovalMs) : FText::FromString(TEXT("-")); }
	if (Column == ColRun) { return Millis(Record.ExecMs); }
	if (Column == ColTransport) { return Millis(Record.TransportMs); }
	return FText::GetEmpty();
}

FText SCursorActivityView::GetSummary() const
{
	const FCursorActivity& Activity = FCursorActivity::Get();
	FText State;
	if (Activity.NumActiveSessions() > 0)
	{
		State = LOCTEXT("AgentActive", "The in-editor agent is working.");
	}
	else if (Activity.IsAgentActive())
	{
		State = LOCTEXT("AgentRecent", "A tool was called in the last half minute.");
	}
	else
	{
		State = LOCTEXT("AgentIdle", "No agent is working.");
	}
	return FText::Format(LOCTEXT("ActivitySummary", "{0} tool {0}|plural(one=call,other=calls) shown. {1} {2}"),
		FText::AsNumber(Rows.Num()),
		State,
		Activity.ShouldStayResponsive()
			? LOCTEXT("Awake", "The editor stays at full speed if you switch to another window.")
			: FText::GetEmpty());
}

// --- Performance -----------------------------------------------------------------------------

void SCursorPerfView::Construct(const FArguments& InArgs)
{
	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 6.0f))
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.Text_Lambda([this]() { return Summary; })
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("ExportCsv", "Export CSV"))
				.ToolTipText(LOCTEXT("ExportCsvTip", "Write this session's turns and tool totals to a CSV file in the log folder."))
				.OnClicked(this, &SCursorPerfView::HandleExport)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("OpenLogs", "Open log folder"))
				.ToolTipText(LOCTEXT("OpenLogsTip", "Saved/CursorUnreal/Logs holds one JSON Lines file per day. It records names, timings and counts, never prompts or results."))
				.OnClicked(this, &SCursorPerfView::HandleOpenFolder)
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(FMargin(6.0f, 0.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(LOCTEXT("ClearPerf", "Clear"))
				.ToolTipText(LOCTEXT("ClearPerfTip", "Empty these tables. Files already written are kept."))
				.OnClicked(this, &SCursorPerfView::HandleClear)
			]
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		.Padding(FMargin(8.0f, 0.0f, 8.0f, 4.0f))
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.ColorAndOpacity(FSlateColor::UseSubduedForeground())
			.Visibility_Lambda([this]() { return Note.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
			.Text_Lambda([this]() { return Note; })
		]
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNew(SSplitter)
			.Orientation(Orient_Vertical)
			+ SSplitter::Slot()
			.Value(0.55f)
			[
				SAssignNew(TurnList, SListView<FTurnPtr>)
				.ListItemsSource(&Turns)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SCursorPerfView::MakeTurnRow)
				.HeaderRow(
					SNew(SHeaderRow)
					+ Column(TEXT("Time"), LOCTEXT("TurnTime", "Turn"), 0.9f)
					+ Column(TEXT("Model"), LOCTEXT("TurnModel", "Model"), 1.6f)
					+ Column(TEXT("Outcome"), LOCTEXT("TurnOutcome", "Outcome"), 0.9f)
					+ Column(TEXT("First"), LOCTEXT("TurnFirst", "First token"), 1.0f)
					+ Column(TEXT("Total"), LOCTEXT("TurnTotal", "Total"), 0.8f)
					+ Column(TEXT("ModelMs"), LOCTEXT("TurnModelMs", "In model"), 0.8f)
					+ Column(TEXT("ToolMs"), LOCTEXT("TurnToolMs", "In tools"), 0.8f)
					+ Column(TEXT("Trips"), LOCTEXT("TurnTrips", "Requests"), 0.9f)
					+ Column(TEXT("Calls"), LOCTEXT("TurnCalls", "Tool calls"), 0.9f)
					+ Column(TEXT("In"), LOCTEXT("TurnIn", "Tokens in"), 0.9f)
					+ Column(TEXT("Cached"), LOCTEXT("TurnCached", "Cached"), 0.8f)
					+ Column(TEXT("Out"), LOCTEXT("TurnOut", "Tokens out"), 1.0f))
			]
			+ SSplitter::Slot()
			.Value(0.45f)
			[
				SAssignNew(ToolList, SListView<FToolPtr>)
				.ListItemsSource(&Tools)
				.SelectionMode(ESelectionMode::None)
				.OnGenerateRow(this, &SCursorPerfView::MakeToolRow)
				.HeaderRow(
					SNew(SHeaderRow)
					+ Column(TEXT("Tool"), LOCTEXT("AggTool", "Tool"), 2.2f)
					+ Column(TEXT("Calls"), LOCTEXT("AggCalls", "Calls"), 0.7f)
					+ Column(TEXT("Errors"), LOCTEXT("AggErrors", "Errors"), 0.7f)
					+ Column(TEXT("AvgRun"), LOCTEXT("AggAvgRun", "Average run"), 1.0f)
					+ Column(TEXT("MaxRun"), LOCTEXT("AggMaxRun", "Longest run"), 1.0f)
					+ Column(TEXT("AvgQueue"), LOCTEXT("AggAvgQueue", "Average wait"), 1.0f)
					+ Column(TEXT("AvgTransport"), LOCTEXT("AggAvgTransport", "Average transport"), 1.1f))
			]
		]
	];

	FCursorPerfLog::Get().OnChanged.AddSP(this, &SCursorPerfView::Reload);
	Reload();
}

SCursorPerfView::~SCursorPerfView()
{
	FCursorPerfLog::Get().OnChanged.RemoveAll(this);
}

void SCursorPerfView::Reload()
{
	const FCursorPerfLog& Log = FCursorPerfLog::Get();

	Turns.Reset();
	double SumFirst = 0.0;
	int32 NumFirst = 0;
	double SumTotal = 0.0;
	int64 TokensIn = 0;
	int64 TokensCached = 0;
	int64 TokensOut = 0;
	const TArray<FCursorTurnPerf>& All = Log.GetTurns();
	for (int32 Index = All.Num() - 1; Index >= 0; --Index)
	{
		const FCursorTurnPerf& Turn = All[Index];
		Turns.Add(MakeShared<FCursorTurnPerf>(Turn));
		if (Turn.TimeToFirstTokenMs >= 0.0)
		{
			SumFirst += Turn.TimeToFirstTokenMs;
			++NumFirst;
		}
		SumTotal += Turn.TotalMs;
		TokensIn += Turn.InputTokens + Turn.CacheReadTokens + Turn.CacheWriteTokens;
		TokensCached += Turn.CacheReadTokens;
		TokensOut += Turn.OutputTokens;
	}

	Tools.Reset();
	TArray<FCursorToolAggregate> Aggregates = Log.GetToolAggregates();
	Aggregates.Sort([](const FCursorToolAggregate& A, const FCursorToolAggregate& B) { return A.TotalExecMs > B.TotalExecMs; });
	for (const FCursorToolAggregate& Aggregate : Aggregates)
	{
		Tools.Add(MakeShared<FCursorToolAggregate>(Aggregate));
	}

	if (Turns.Num() == 0)
	{
		Summary = Tools.Num() == 0
			? LOCTEXT("NoPerfYet", "Nothing measured yet this session.")
			: LOCTEXT("ToolsOnly", "No model turns yet this session. The lower table shows tool calls made by other clients.");
	}
	else
	{
		Summary = FText::Format(
			LOCTEXT("PerfSummary", "{0} {0}|plural(one=turn,other=turns). Average time to first token {1}, average turn {2}. Tokens in {3} ({4}% read from cache), out {5}."),
			FText::AsNumber(Turns.Num()),
			NumFirst > 0 ? Millis(SumFirst / NumFirst) : FText::FromString(TEXT("-")),
			Millis(SumTotal / Turns.Num()),
			FText::AsNumber(TokensIn),
			FText::AsNumber(TokensIn > 0 ? FMath::RoundToInt(100.0 * static_cast<double>(TokensCached) / static_cast<double>(TokensIn)) : 0),
			FText::AsNumber(TokensOut));
	}

	TurnList->RequestListRefresh();
	ToolList->RequestListRefresh();
}

TSharedRef<ITableRow> SCursorPerfView::MakeTurnRow(FTurnPtr Turn, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SCursorTextRow<FCursorTurnPerf>, Owner, Turn, &SCursorPerfView::TurnCell);
}

TSharedRef<ITableRow> SCursorPerfView::MakeToolRow(FToolPtr Tool, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SCursorTextRow<FCursorToolAggregate>, Owner, Tool, &SCursorPerfView::ToolCell);
}

FText SCursorPerfView::TurnCell(const FCursorTurnPerf& Turn, FName Column)
{
	if (Column == ColTime) { return LocalClock(Turn.StartedAt); }
	if (Column == ColModel) { return FText::FromString(Turn.Model); }
	if (Column == ColOutcome) { return FText::FromString(Turn.Outcome); }
	if (Column == ColFirst) { return Millis(Turn.TimeToFirstTokenMs); }
	if (Column == ColTotal) { return Millis(Turn.TotalMs); }
	if (Column == ColModelMs) { return Millis(Turn.ModelMs); }
	if (Column == ColToolMs) { return Millis(Turn.ToolMs); }
	if (Column == ColTrips) { return FText::AsNumber(Turn.RoundTrips); }
	if (Column == ColCalls) { return FText::AsNumber(Turn.ToolCalls); }
	if (Column == ColIn) { return FText::AsNumber(Turn.InputTokens + Turn.CacheReadTokens + Turn.CacheWriteTokens); }
	if (Column == ColCached) { return FText::AsNumber(Turn.CacheReadTokens); }
	if (Column == ColOut) { return FText::AsNumber(Turn.OutputTokens); }
	return FText::GetEmpty();
}

FText SCursorPerfView::ToolCell(const FCursorToolAggregate& Tool, FName Column)
{
	if (Column == ColTool) { return FText::FromString(Tool.ToolName); }
	if (Column == ColCalls) { return FText::AsNumber(Tool.Calls); }
	if (Column == ColErrors) { return FText::AsNumber(Tool.Errors); }
	if (Column == ColAvgRun) { return Tool.Calls > 0 ? Millis(Tool.TotalExecMs / Tool.Calls) : FText::FromString(TEXT("-")); }
	if (Column == ColMaxRun) { return Millis(Tool.MaxExecMs); }
	if (Column == ColAvgQueue) { return Tool.Calls > 0 ? Millis(Tool.TotalQueueMs / Tool.Calls) : FText::FromString(TEXT("-")); }
	if (Column == ColAvgTransport) { return Tool.TransportSamples > 0 ? Millis(Tool.TotalTransportMs / Tool.TransportSamples) : FText::FromString(TEXT("-")); }
	return FText::GetEmpty();
}

FReply SCursorPerfView::HandleExport()
{
	const FString Path = FCursorPerfLog::GetLogDirectory() / FString::Printf(TEXT("perf-export-%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d-%H%M%S")));
	FString Error;
	if (FCursorPerfLog::Get().ExportCsv(Path, Error))
	{
		Note = FText::Format(LOCTEXT("Exported", "Written to {0}"), FText::FromString(Path));
	}
	else
	{
		Note = FText::Format(LOCTEXT("ExportFailed", "Could not write the CSV: {0}"), FText::FromString(Error));
	}
	return FReply::Handled();
}

FReply SCursorPerfView::HandleOpenFolder()
{
	const FString Directory = FCursorPerfLog::GetLogDirectory();
	IFileManager::Get().MakeDirectory(*Directory, true);
	FPlatformProcess::ExploreFolder(*Directory);
	return FReply::Handled();
}

FReply SCursorPerfView::HandleClear()
{
	FCursorPerfLog::Get().Clear();
	Note = FText::GetEmpty();
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
