#include "CursorEditorStyle.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/StyleColors.h"

TSharedPtr<FSlateStyleSet> FCursorEditorStyle::StyleSet;
const FName FCursorEditorStyle::BodyText(TEXT("Body"));
const FName FCursorEditorStyle::SubduedText(TEXT("Subdued"));

FName FCursorEditorStyle::GetStyleSetName()
{
	static const FName Name(TEXT("CursorUnrealStyle"));
	return Name;
}

FSlateFontInfo FCursorEditorStyle::Mono(int32 Size)
{
	return FCoreStyle::GetDefaultFontStyle("Mono", Size);
}

void FCursorEditorStyle::Register()
{
	if (StyleSet.IsValid())
	{
		return;
	}
	StyleSet = MakeShared<FSlateStyleSet>(GetStyleSetName());

	const FTextBlockStyle Normal = FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText");
	constexpr int32 BodySize = 10;

	StyleSet->Set(BodyText, FTextBlockStyle(Normal)
		.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", BodySize))
		.SetColorAndOpacity(FStyleColors::Foreground));
	StyleSet->Set(SubduedText, FTextBlockStyle(Normal)
		.SetFont(FCoreStyle::GetDefaultFontStyle("Italic", 9))
		.SetColorAndOpacity(FSlateColor::UseSubduedForeground()));

	// Looked up by tag name from the chat's rich text.
	StyleSet->Set("b", FTextBlockStyle(Normal)
		.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", BodySize))
		.SetColorAndOpacity(FStyleColors::ForegroundHover));
	StyleSet->Set("h", FTextBlockStyle(Normal)
		.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", BodySize + 3))
		.SetColorAndOpacity(FStyleColors::ForegroundHover));
	StyleSet->Set("code", FTextBlockStyle(Normal)
		.SetFont(Mono(BodySize - 1))
		.SetColorAndOpacity(FStyleColors::AccentBlue));

	StyleSet->Set("Bubble.User", new FSlateRoundedBoxBrush(FStyleColors::Header, 6.0f));
	StyleSet->Set("Bubble.Code", new FSlateRoundedBoxBrush(FStyleColors::Recessed, 4.0f));
	StyleSet->Set("Bubble.Tool", new FSlateRoundedBoxBrush(FStyleColors::Panel, 4.0f, FStyleColors::Dropdown, 1.0f));
	StyleSet->Set("Bubble.Error", new FSlateRoundedBoxBrush(FLinearColor(0.35f, 0.04f, 0.04f, 0.6f), 4.0f));
	StyleSet->Set("Bubble.Banner", new FSlateRoundedBoxBrush(FLinearColor(0.40f, 0.26f, 0.02f, 0.6f), 4.0f));
	StyleSet->Set("Chip.On", new FSlateRoundedBoxBrush(FStyleColors::Primary, 10.0f));
	StyleSet->Set("Chip.Off", new FSlateRoundedBoxBrush(FStyleColors::Dropdown, 10.0f));

	FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
}

void FCursorEditorStyle::Unregister()
{
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		StyleSet.Reset();
	}
}

const ISlateStyle& FCursorEditorStyle::Get()
{
	if (!StyleSet.IsValid())
	{
		Register();
	}
	return *StyleSet;
}

const FSlateBrush* FCursorEditorStyle::Brush(const TCHAR* Name)
{
	return Get().GetBrush(Name);
}
