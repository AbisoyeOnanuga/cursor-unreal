#pragma once

#include "CoreMinimal.h"
#include "AgentSession.h"
#include "Widgets/SCompoundWidget.h"

class SWrapBox;
struct FSlateDynamicImageBrush;

DECLARE_DELEGATE_OneParam(FOnCursorImageRemoved, int32 /*Index*/);

/**
 * A row of small pictures that wraps: what is attached to the message being written, what was
 * sent with one, or what a tool showed the model. Clicking a picture that has a saved copy opens
 * it. Owns the textures it shows and lets go of them with the widget.
 */
class SCursorImageStrip : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SCursorImageStrip) : _ThumbHeight(72.0f) {}
		SLATE_ARGUMENT(float, ThumbHeight)
		/** When bound, each picture gets a button that removes it. */
		SLATE_EVENT(FOnCursorImageRemoved, OnRemoved)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	/** Cheap when the pictures are the ones already shown. */
	void SetImages(const TArray<FAgentItemImage>& Images);
	int32 NumShown() const { return ShownKeys.Num(); }

private:
	TSharedRef<SWidget> MakeEntry(const FAgentItemImage& Entry, int32 Index);

	float ThumbHeight = 72.0f;
	FOnCursorImageRemoved OnRemoved;
	TSharedPtr<SWrapBox> Box;
	TArray<TSharedPtr<FSlateDynamicImageBrush>> Brushes;
	/** Hash and saved path of each picture shown, to tell when nothing changed. */
	TArray<FString> ShownKeys;
};
