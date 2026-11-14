#pragma once

#include "CoreMinimal.h"

/**
 * Just enough Markdown for chat replies: fenced code blocks, headings, bold, inline code and
 * bullets. Anything else is shown as written. Made to be run on a reply that is still arriving.
 */
namespace CursorMarkdown
{
	struct FSegment
	{
		bool bCode = false;
		/** The word after the opening fence, such as "cpp". May be empty. */
		FString Language;
		FString Text;

		bool SameShape(const FSegment& Other) const { return bCode == Other.bCode && Language == Other.Language; }
	};

	/** Splits a reply at ``` fences. A fence that has not been closed yet counts as code to the end. */
	TArray<FSegment> Split(const FString& Markdown);

	/** Turns prose into SRichTextBlock markup using the tags <h>, <b> and <code>. */
	FString ToRichText(const FString& Prose);

	/** Escapes the characters SRichTextBlock would read as markup. */
	FString Escape(const FString& Text);
}
