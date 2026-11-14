#pragma once

#include "CoreMinimal.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/STableRow.h"

/**
 * A list row whose cells are all text. The text is asked for every frame, so a row stays current
 * while the record behind it changes (a tool call going from running to finished).
 */
template <typename ItemType>
class SCursorTextRow : public SMultiColumnTableRow<TSharedPtr<ItemType>>
{
public:
	using FGetCell = TFunction<FText(const ItemType& Item, FName Column)>;

	SLATE_BEGIN_ARGS(SCursorTextRow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner, const TSharedPtr<ItemType>& InItem, FGetCell InGetCell)
	{
		Item = InItem;
		GetCell = MoveTemp(InGetCell);
		SMultiColumnTableRow<TSharedPtr<ItemType>>::Construct(typename SMultiColumnTableRow<TSharedPtr<ItemType>>::FArguments(), Owner);
	}

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
	{
		const TSharedPtr<ItemType> Local = Item;
		const FGetCell Getter = GetCell;
		return SNew(SBox)
			.Padding(FMargin(6.0f, 3.0f))
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.OverflowPolicy(ETextOverflowPolicy::Ellipsis)
				.Text_Lambda([Local, Getter, Column]() { return Getter(*Local, Column); })
			];
	}

private:
	TSharedPtr<ItemType> Item;
	FGetCell GetCell;
};
