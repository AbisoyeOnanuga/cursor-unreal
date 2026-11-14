#pragma once

#include "CoreMinimal.h"
#include "CursorActivity.h"
#include "CursorPerfLog.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SMultiLineEditableText;

/** Every tool call this session, from every caller: the in-editor agent, the Cursor bridge and MCP clients. */
class SCursorActivityView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCursorActivityView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCursorActivityView() override;

private:
	using FRecordPtr = TSharedPtr<FCursorToolCallRecord>;

	void Reload();
	void ShowDetails();
	void HandleAdded(const TSharedRef<FCursorToolCallRecord>& Record);
	void HandleUpdated(const TSharedRef<FCursorToolCallRecord>& Record);
	TSharedRef<ITableRow> MakeRow(FRecordPtr Record, const TSharedRef<STableViewBase>& Owner);
	static FText Cell(const FCursorToolCallRecord& Record, FName Column);
	FText GetSummary() const;

	TArray<FRecordPtr> Rows;
	TSharedPtr<SListView<FRecordPtr>> List;
	FRecordPtr Selected;
	TSharedPtr<SMultiLineEditableText> Details;
};

/** Timings and token counts for model turns, and a per-tool rollup. Names and numbers only: no prompts or results. */
class SCursorPerfView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCursorPerfView) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SCursorPerfView() override;

private:
	using FTurnPtr = TSharedPtr<FCursorTurnPerf>;
	using FToolPtr = TSharedPtr<FCursorToolAggregate>;

	void Reload();
	TSharedRef<ITableRow> MakeTurnRow(FTurnPtr Turn, const TSharedRef<STableViewBase>& Owner);
	TSharedRef<ITableRow> MakeToolRow(FToolPtr Tool, const TSharedRef<STableViewBase>& Owner);
	static FText TurnCell(const FCursorTurnPerf& Turn, FName Column);
	static FText ToolCell(const FCursorToolAggregate& Tool, FName Column);
	FReply HandleExport();
	FReply HandleOpenFolder();
	FReply HandleClear();

	TArray<FTurnPtr> Turns;
	TArray<FToolPtr> Tools;
	TSharedPtr<SListView<FTurnPtr>> TurnList;
	TSharedPtr<SListView<FToolPtr>> ToolList;
	FText Summary;
	FText Note;
};
