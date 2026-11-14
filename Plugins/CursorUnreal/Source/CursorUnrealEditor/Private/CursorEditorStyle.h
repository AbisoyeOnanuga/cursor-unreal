#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

/** Text styles and brushes used by the panel. Colours come from the editor theme, so a custom theme carries over. */
class FCursorEditorStyle
{
public:
	static void Register();
	static void Unregister();

	static const ISlateStyle& Get();
	static FName GetStyleSetName();

	static FSlateFontInfo Mono(int32 Size = 9);
	static const FSlateBrush* Brush(const TCHAR* Name);

	// Names of the text styles the chat's rich text uses as tags: <b>, <h>, <code>.
	static const FName BodyText;
	static const FName SubduedText;

private:
	static TSharedPtr<FSlateStyleSet> StyleSet;
};
