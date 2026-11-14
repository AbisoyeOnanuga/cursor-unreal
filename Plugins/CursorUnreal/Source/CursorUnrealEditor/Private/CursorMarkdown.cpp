#include "CursorMarkdown.h"

namespace
{
	bool IsFence(const FString& Line, FString& OutLanguage)
	{
		const FString Trimmed = Line.TrimStart();
		if (!Trimmed.StartsWith(TEXT("```")))
		{
			return false;
		}
		OutLanguage = Trimmed.RightChop(3).TrimStartAndEnd();
		return true;
	}

	/** Text with ** and ` markers taken out, for places where nested styling is not possible. */
	FString StripInlineMarkers(const FString& Text)
	{
		return Text.Replace(TEXT("**"), TEXT("")).Replace(TEXT("`"), TEXT(""));
	}

	/** Bold and inline code within one line. A marker with no partner stays as typed. */
	FString Inline(const FString& Line)
	{
		FString Out;
		int32 Pos = 0;
		const int32 Len = Line.Len();
		while (Pos < Len)
		{
			if (Line[Pos] == TEXT('`'))
			{
				const int32 Close = Line.Find(TEXT("`"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos + 1);
				if (Close > Pos + 1)
				{
					Out += TEXT("<code>") + CursorMarkdown::Escape(Line.Mid(Pos + 1, Close - Pos - 1)) + TEXT("</>");
					Pos = Close + 1;
					continue;
				}
			}
			else if (Line[Pos] == TEXT('*') && Pos + 1 < Len && Line[Pos + 1] == TEXT('*'))
			{
				const int32 Close = Line.Find(TEXT("**"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Pos + 2);
				if (Close > Pos + 2)
				{
					Out += TEXT("<b>") + CursorMarkdown::Escape(StripInlineMarkers(Line.Mid(Pos + 2, Close - Pos - 2))) + TEXT("</>");
					Pos = Close + 2;
					continue;
				}
			}
			Out += CursorMarkdown::Escape(Line.Mid(Pos, 1));
			++Pos;
		}
		return Out;
	}

	bool IsRule(const FString& Trimmed)
	{
		if (Trimmed.Len() < 3)
		{
			return false;
		}
		const TCHAR First = Trimmed[0];
		if (First != TEXT('-') && First != TEXT('*') && First != TEXT('_'))
		{
			return false;
		}
		for (const TCHAR Ch : Trimmed)
		{
			if (Ch != First)
			{
				return false;
			}
		}
		return true;
	}
}

FString CursorMarkdown::Escape(const FString& Text)
{
	// The ampersand first, or the others would be escaped twice.
	return Text.Replace(TEXT("&"), TEXT("&amp;"))
		.Replace(TEXT("<"), TEXT("&lt;"))
		.Replace(TEXT(">"), TEXT("&gt;"))
		.Replace(TEXT("\""), TEXT("&quot;"));
}

TArray<CursorMarkdown::FSegment> CursorMarkdown::Split(const FString& Markdown)
{
	TArray<FString> Lines;
	Markdown.Replace(TEXT("\r\n"), TEXT("\n")).ParseIntoArray(Lines, TEXT("\n"), false);

	TArray<FSegment> Segments;
	FSegment Current;
	bool bHasLines = false;

	const auto Flush = [&Segments, &Current, &bHasLines]()
	{
		// Blank lines around a block are spacing, not content. An empty code block is still shown: it may be about to fill.
		while (Current.Text.EndsWith(TEXT("\n")))
		{
			Current.Text.LeftChopInline(1);
		}
		if (Current.bCode || !Current.Text.TrimStartAndEnd().IsEmpty())
		{
			if (!Current.bCode)
			{
				while (Current.Text.StartsWith(TEXT("\n")))
				{
					Current.Text.RightChopInline(1);
				}
			}
			Segments.Add(Current);
		}
		Current = FSegment();
		bHasLines = false;
	};

	for (const FString& Line : Lines)
	{
		FString Language;
		if (IsFence(Line, Language))
		{
			const bool bOpening = !Current.bCode;
			Flush();
			if (bOpening)
			{
				Current.bCode = true;
				Current.Language = Language;
			}
			continue;
		}
		if (bHasLines)
		{
			Current.Text += TEXT("\n");
		}
		Current.Text += Line;
		bHasLines = true;
	}
	Flush();
	return Segments;
}

FString CursorMarkdown::ToRichText(const FString& Prose)
{
	TArray<FString> Lines;
	Prose.Replace(TEXT("\r\n"), TEXT("\n")).ParseIntoArray(Lines, TEXT("\n"), false);

	FString Out;
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		const FString& Line = Lines[Index];
		const FString Trimmed = Line.TrimStart();
		const int32 Indent = Line.Len() - Trimmed.Len();

		int32 Hashes = 0;
		while (Hashes < Trimmed.Len() && Trimmed[Hashes] == TEXT('#'))
		{
			++Hashes;
		}

		if (Hashes >= 1 && Hashes <= 6 && Hashes < Trimmed.Len() && Trimmed[Hashes] == TEXT(' '))
		{
			// Rich text tags do not nest, so a heading is one style from end to end.
			const FString Title = StripInlineMarkers(Trimmed.RightChop(Hashes + 1)).TrimStartAndEnd();
			Out += Title.IsEmpty() ? FString() : TEXT("<h>") + Escape(Title) + TEXT("</>");
		}
		else if (IsRule(Trimmed.TrimEnd()))
		{
			Out += TEXT("\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500");
		}
		else if (Trimmed.Len() > 2 && (Trimmed[0] == TEXT('-') || Trimmed[0] == TEXT('*') || Trimmed[0] == TEXT('+')) && Trimmed[1] == TEXT(' '))
		{
			Out += FString::ChrN(Indent + 2, TEXT(' ')) + TEXT("\u2022 ") + Inline(Trimmed.RightChop(2));
		}
		else
		{
			Out += FString::ChrN(Indent, TEXT(' ')) + Inline(Trimmed);
		}

		if (Index + 1 < Lines.Num())
		{
			Out += TEXT("\n");
		}
	}
	return Out;
}
