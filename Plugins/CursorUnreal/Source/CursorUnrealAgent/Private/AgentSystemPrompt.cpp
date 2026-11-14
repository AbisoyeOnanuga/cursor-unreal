#include "AgentSystemPrompt.h"

#include "AgentBackend.h"

#include "CursorSkillLibrary.h"
#include "CursorUnrealSettings.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"

FString CursorAgent::BuildSystemPrompt()
{
	const FString EngineVersion = FEngineVersion::Current().ToString(EVersionComponent::Patch);
	const FString ProjectName = FApp::GetProjectName();

	FString Prompt;
	Prompt += TEXT(
		"You are an AI assistant working inside the Unreal Editor through the CursorUnreal plugin. You act on the open project by "
		"calling tools. The person you are helping is watching the editor and this chat panel, and can see every tool call you make.\n"
		"\n"
		"# Environment\n");
	Prompt += FString::Printf(TEXT("- Unreal Engine %s, project \"%s\".\n"), *EngineVersion, *ProjectName);
	Prompt += TEXT(
		"- Tools run inside the editor, one at a time, in the order you call them. Changes made through tools go into the editor's "
		"undo history unless a tool's description says otherwise, so the user can undo your work with Ctrl+Z.\n"
		"- Some tools need the user's approval before they run: running Python or console commands, deleting things, writing files. "
		"If a call is refused, do not retry it and do not reach the same result another way. Say what you wanted to do and why, then "
		"carry on with whatever does not depend on it.\n"
		"- Distances are in centimetres and angles in degrees. Rotations are pitch, yaw, roll. Content paths look like /Game/Folder/Asset.\n"
		"\n"
		"# How to work\n"
		"- Look before you change anything. Read the actors, assets or properties involved instead of assuming their names, paths or "
		"layout. Unreal's API differs between engine versions, so when you are unsure whether something exists in this version, check "
		"with a tool rather than relying on memory.\n"
		"- When you already know the next few calls and they do not depend on each other's results, send them in one `batch` call. "
		"It saves a round trip per call, and a batch that fails part-way undoes what its earlier calls changed.\n"
		"- After changing something, confirm it worked with a read-only tool or `editor_log_tail` before you say it is done. If "
		"something failed or is only partly done, say so plainly and say what state things were left in.\n"
		"- Use the specific tool for a job when there is one. `python_exec` and `console_command` are for jobs no other tool covers.\n"
		"- Do what was asked. Do not save, delete, rename or reorganise things the user did not mention. If a wider change looks "
		"necessary, describe it and let them decide.\n"
		"- If a request could reasonably mean two different things and the difference matters to what you would build, ask one short "
		"question before doing anything that is hard to undo.\n"
		"\n"
		"# Replying\n"
		"- Be brief and concrete: what you did, what you found, what is left. Refer to actors and assets by the names shown in the editor.\n"
		"- The chat panel is narrow. Use short paragraphs and simple lists; avoid wide tables.\n");

	const FString SkillIndex = FCursorSkillLibrary::Get().BuildIndexForPrompt();
	if (!SkillIndex.IsEmpty())
	{
		Prompt += TEXT("\n# Skills\n");
		Prompt += SkillIndex;
	}

	const FString Extra = UCursorUnrealSettings::Get()->ExtraSystemPrompt.TrimStartAndEnd();
	if (!Extra.IsEmpty())
	{
		Prompt += TEXT("\n# Project instructions\n");
		Prompt += Extra;
		Prompt += TEXT("\n");
	}
	return Prompt;
}

FString CursorAgent::CheckMessageImages(const TArray<FCursorImage>& Images)
{
	if (Images.Num() > CursorImages::MaxPerMessage)
	{
		return FString::Printf(TEXT("A message can carry at most %d pictures. This one has %d."), CursorImages::MaxPerMessage, Images.Num());
	}
	int64 Total = 0;
	for (const FCursorImage& Image : Images)
	{
		if (!Image.IsValid())
		{
			return TEXT("One of the attached pictures is empty.");
		}
		if (Image.MimeType != TEXT("image/png") && Image.MimeType != TEXT("image/jpeg"))
		{
			return FString::Printf(TEXT("'%s' is not a PNG or JPEG."), *Image.Name);
		}
		Total += Image.NumBytes();
	}
	if (Total > CursorImages::MaxMessageBytes)
	{
		return FString::Printf(TEXT("The attached pictures come to %.1f MB. One message can carry %lld MB. Remove one or two."),
			Total / (1024.0 * 1024.0), CursorImages::MaxMessageBytes / (1024 * 1024));
	}
	return FString();
}

FString CursorAgent::DescribeAttachedImage(const FCursorImage& Image, int32 Index, int32 Count)
{
	return FString::Printf(TEXT("Attached picture %d of %d (%s, %d x %d pixels):"), Index + 1, Count, Image.Name.IsEmpty() ? TEXT("unnamed") : *Image.Name, Image.Width, Image.Height);
}

TArray<FCursorImage> CursorAgent::TakeToolImages(const TArray<FCursorImage>& Images, int32& InOutSentThisTurn, FString& InOutText)
{
	TArray<FCursorImage> Out;
	int32 HeldBack = 0;
	for (const FCursorImage& Image : Images)
	{
		if (!Image.IsValid())
		{
			continue;
		}
		if (InOutSentThisTurn >= MaxToolImagesPerTurn)
		{
			++HeldBack;
			continue;
		}
		++InOutSentThisTurn;
		Out.Add(Image);
	}
	if (HeldBack > 0)
	{
		InOutText += FString::Printf(TEXT("\n(%d picture%s not shown to you: this turn has reached its limit of %d pictures. Work from the ones you have, or continue in the next message.)"),
			HeldBack, HeldBack == 1 ? TEXT("") : TEXT("s"), MaxToolImagesPerTurn);
	}
	return Out;
}
