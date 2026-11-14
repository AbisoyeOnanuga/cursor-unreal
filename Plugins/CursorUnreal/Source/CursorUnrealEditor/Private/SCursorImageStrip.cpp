#include "SCursorImageStrip.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "CursorEditorStyle.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "CursorUnrealImages"

namespace
{
	/** Thumbnails are decoded no larger than this, however big the picture. */
	constexpr int32 ThumbnailPixels = 256;

	FString KeyOf(const FAgentItemImage& Entry)
	{
		return Entry.Image.Hash() + TEXT("|") + Entry.File;
	}
}

void SCursorImageStrip::Construct(const FArguments& InArgs)
{
	ThumbHeight = InArgs._ThumbHeight;
	OnRemoved = InArgs._OnRemoved;
	ChildSlot
	[
		SAssignNew(Box, SWrapBox)
		.UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(6.0f, 6.0f))
	];
	SetVisibility(EVisibility::Collapsed);
}

void SCursorImageStrip::SetImages(const TArray<FAgentItemImage>& Images)
{
	TArray<FString> Keys;
	for (const FAgentItemImage& Entry : Images)
	{
		Keys.Add(KeyOf(Entry));
	}
	if (Keys == ShownKeys)
	{
		return;
	}
	ShownKeys = MoveTemp(Keys);

	Box->ClearChildren();
	Brushes.Reset();
	for (int32 Index = 0; Index < Images.Num(); ++Index)
	{
		Box->AddSlot()
		[
			MakeEntry(Images[Index], Index)
		];
	}
	SetVisibility(Images.Num() > 0 ? EVisibility::Visible : EVisibility::Collapsed);
}

TSharedRef<SWidget> SCursorImageStrip::MakeEntry(const FAgentItemImage& Entry, int32 Index)
{
	const FCursorImage& Image = Entry.Image;
	const FString File = Entry.File;
	const FText Tip = File.IsEmpty()
		? FText::Format(LOCTEXT("TipUnsaved", "{0}\nNot saved to disk: saving conversations is switched off."), FText::FromString(Image.Describe()))
		: FText::Format(LOCTEXT("TipSaved", "{0}\nClick to open the saved copy ({1})."), FText::FromString(Image.Describe()), FText::FromString(FPaths::GetCleanFilename(File)));

	TSharedPtr<SWidget> Picture;
	TArray<uint8> Pixels;
	int32 Width = 0;
	int32 Height = 0;
	TSharedPtr<FSlateDynamicImageBrush> Brush;
	// Without a renderer (tests, commandlets) there are no textures to make; the name is shown instead.
	if (FSlateApplication::IsInitialized() && FApp::CanEverRender() && CursorImages::DecodeForDisplay(Image, ThumbnailPixels, Pixels, Width, Height))
	{
		static int32 Serial = 0;
		const FName ResourceName(*FString::Printf(TEXT("CursorUnrealThumb_%d"), ++Serial));
		Brush = FSlateDynamicImageBrush::CreateWithImageData(ResourceName, FVector2D(Width, Height), Pixels);
	}
	if (Brush.IsValid())
	{
		Brushes.Add(Brush);
		const float Aspect = Height > 0 ? static_cast<float>(Width) / static_cast<float>(Height) : 1.0f;
		Picture = SNew(SBox)
			.HeightOverride(ThumbHeight)
			.WidthOverride(FMath::Clamp(ThumbHeight * Aspect, ThumbHeight * 0.4f, ThumbHeight * 3.0f))
			[
				SNew(SImage)
				.Image(Brush.Get())
			];
	}
	else
	{
		Picture = SNew(SBorder)
			.BorderImage(FCursorEditorStyle::Brush(TEXT("Bubble.Code")))
			.Padding(FMargin(8.0f, 6.0f))
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor::UseSubduedForeground())
				.Text(FText::FromString(Image.Describe()))
			];
	}

	const TSharedRef<SWidget> Clickable = SNew(SButton)
		.ButtonStyle(FAppStyle::Get(), "NoBorder")
		.ContentPadding(FMargin(0.0f))
		.ToolTipText(Tip)
		.Cursor(File.IsEmpty() ? EMouseCursor::Default : EMouseCursor::Hand)
		.OnClicked_Lambda([File]()
		{
			if (!File.IsEmpty() && FPaths::FileExists(File))
			{
				FPlatformProcess::LaunchFileInDefaultExternalApplication(*File);
			}
			return FReply::Handled();
		})
		[
			Picture.ToSharedRef()
		];

	if (!OnRemoved.IsBound())
	{
		return Clickable;
	}
	return SNew(SOverlay)
		+ SOverlay::Slot()
		[
			Clickable
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Top)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ContentPadding(FMargin(1.0f))
			.ToolTipText(LOCTEXT("RemoveTip", "Do not send this picture"))
			.OnClicked_Lambda([this, Index]()
			{
				// Copied first: the handler rebuilds this strip, which destroys this button.
				const FOnCursorImageRemoved Handler = OnRemoved;
				Handler.ExecuteIfBound(Index);
				return FReply::Handled();
			})
			[
				SNew(SImage)
				.Image(FAppStyle::GetBrush("Icons.X"))
				.DesiredSizeOverride(FVector2D(12.0f, 12.0f))
			]
		];
}

#undef LOCTEXT_NAMESPACE
