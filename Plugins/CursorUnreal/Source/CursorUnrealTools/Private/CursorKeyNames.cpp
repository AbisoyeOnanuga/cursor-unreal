#include "CursorPackSupport.h"

#include "InputCoreTypes.h"

namespace CursorPackSupport
{
	/** Lower case with spaces, underscores and dashes removed, so "Space Bar", "space_bar" and "SpaceBar" compare equal. */
	FString Squash(const FString& Text)
	{
		FString Out;
		for (const TCHAR Ch : Text)
		{
			if (FChar::IsAlnum(Ch))
			{
				Out.AppendChar(FChar::ToLower(Ch));
			}
		}
		return Out;
	}

	/**
	 * A key pin holds a bare key name: SpaceBar, not (KeyName="SpaceBar"). The schema accepts any text for a struct pin,
	 * and a key written the struct way is read back as no key at all, so the graph compiles and then ignores the keyboard.
	 * This takes the spellings a model reaches for and returns the engine's own name, or says which names are close.
	 */
	bool NormaliseKeyName(const FString& Value, FString& OutName, FString& OutError)
	{
		FString Name = Value.TrimStartAndEnd();
		if (Name.StartsWith(TEXT("(")) && Name.EndsWith(TEXT(")")))
		{
			Name = Name.Mid(1, Name.Len() - 2).TrimStartAndEnd();
			if (Name.StartsWith(TEXT("KeyName"), ESearchCase::IgnoreCase))
			{
				Name = Name.Mid(7).TrimStartAndEnd();
				Name.RemoveFromStart(TEXT("="));
			}
		}
		Name = Name.TrimStartAndEnd().TrimQuotes().TrimStartAndEnd();
		Name.RemoveFromStart(TEXT("EKeys::"));
		if (Name.IsEmpty() || Name.Equals(TEXT("None"), ESearchCase::IgnoreCase))
		{
			OutName = TEXT("None");
			return true;
		}

		TArray<FKey> Keys;
		EKeys::GetAllKeys(Keys);
		for (const FKey& Key : Keys)
		{
			if (Key.GetFName().ToString().Equals(Name, ESearchCase::IgnoreCase))
			{
				OutName = Key.GetFName().ToString();
				return true;
			}
		}

		// Names people use that the engine spells differently. Only ones with a single sensible meaning.
		static const TMap<FString, FString> Aliases = {
			{ TEXT("space"), TEXT("SpaceBar") }, { TEXT("shift"), TEXT("LeftShift") }, { TEXT("lshift"), TEXT("LeftShift") }, { TEXT("rshift"), TEXT("RightShift") },
			{ TEXT("ctrl"), TEXT("LeftControl") }, { TEXT("control"), TEXT("LeftControl") }, { TEXT("leftctrl"), TEXT("LeftControl") }, { TEXT("rightctrl"), TEXT("RightControl") },
			{ TEXT("alt"), TEXT("LeftAlt") }, { TEXT("esc"), TEXT("Escape") }, { TEXT("return"), TEXT("Enter") }, { TEXT("del"), TEXT("Delete") }, { TEXT("backspace"), TEXT("BackSpace") },
			{ TEXT("leftarrow"), TEXT("Left") }, { TEXT("rightarrow"), TEXT("Right") }, { TEXT("uparrow"), TEXT("Up") }, { TEXT("downarrow"), TEXT("Down") },
			{ TEXT("lmb"), TEXT("LeftMouseButton") }, { TEXT("leftmouse"), TEXT("LeftMouseButton") }, { TEXT("leftclick"), TEXT("LeftMouseButton") },
			{ TEXT("rmb"), TEXT("RightMouseButton") }, { TEXT("rightmouse"), TEXT("RightMouseButton") }, { TEXT("rightclick"), TEXT("RightMouseButton") },
			{ TEXT("mmb"), TEXT("MiddleMouseButton") }, { TEXT("mousewheel"), TEXT("MouseWheelAxis") },
			{ TEXT("gamepada"), TEXT("Gamepad_FaceButton_Bottom") }, { TEXT("gamepadb"), TEXT("Gamepad_FaceButton_Right") },
			{ TEXT("gamepadx"), TEXT("Gamepad_FaceButton_Left") }, { TEXT("gamepady"), TEXT("Gamepad_FaceButton_Top") },
			{ TEXT("xboxa"), TEXT("Gamepad_FaceButton_Bottom") }, { TEXT("xboxb"), TEXT("Gamepad_FaceButton_Right") },
			{ TEXT("xboxx"), TEXT("Gamepad_FaceButton_Left") }, { TEXT("xboxy"), TEXT("Gamepad_FaceButton_Top") },
			{ TEXT("gamepadlb"), TEXT("Gamepad_LeftShoulder") }, { TEXT("gamepadrb"), TEXT("Gamepad_RightShoulder") },
			{ TEXT("gamepadleftbumper"), TEXT("Gamepad_LeftShoulder") }, { TEXT("gamepadrightbumper"), TEXT("Gamepad_RightShoulder") },
			{ TEXT("gamepadlt"), TEXT("Gamepad_LeftTrigger") }, { TEXT("gamepadrt"), TEXT("Gamepad_RightTrigger") },
			{ TEXT("gamepadl3"), TEXT("Gamepad_LeftThumbstick") }, { TEXT("gamepadr3"), TEXT("Gamepad_RightThumbstick") },
			{ TEXT("gamepadleftstickx"), TEXT("Gamepad_LeftX") }, { TEXT("gamepadleftsticky"), TEXT("Gamepad_LeftY") },
			{ TEXT("gamepadrightstickx"), TEXT("Gamepad_RightX") }, { TEXT("gamepadrightsticky"), TEXT("Gamepad_RightY") },
			{ TEXT("gamepaddpadup"), TEXT("Gamepad_DPad_Up") }, { TEXT("gamepaddpaddown"), TEXT("Gamepad_DPad_Down") },
			{ TEXT("gamepaddpadleft"), TEXT("Gamepad_DPad_Left") }, { TEXT("gamepaddpadright"), TEXT("Gamepad_DPad_Right") },
			{ TEXT("gamepadstart"), TEXT("Gamepad_Special_Right") }, { TEXT("gamepadback"), TEXT("Gamepad_Special_Left") },
		};
		const FString Wanted = Squash(Name);
		if (const FString* Alias = Aliases.Find(Wanted))
		{
			OutName = *Alias;
			return true;
		}
		// "left shift", "space_bar", "Gamepad Left X": the engine's name, or the name the editor shows, written with other spacing.
		for (const bool bDisplayName : { false, true })
		{
			for (const FKey& Key : Keys)
			{
				if (Squash(bDisplayName ? Key.GetDisplayName().ToString() : Key.GetFName().ToString()) == Wanted)
				{
					OutName = Key.GetFName().ToString();
					return true;
				}
			}
		}

		TArray<FString> Close;
		for (const FKey& Key : Keys)
		{
			const FString KeyName = Squash(Key.GetFName().ToString());
			const FString Display = Squash(Key.GetDisplayName().ToString());
			if (Wanted.Len() >= 3 && (KeyName.Contains(Wanted) || Display.Contains(Wanted) || (KeyName.Len() >= 3 && Wanted.Contains(KeyName))) && Close.Num() < 12)
			{
				Close.Add(Key.GetFName().ToString());
			}
		}
		OutError = FString::Printf(TEXT("'%s' is not a key name.%s Key pins take the engine's key names as plain text: A to Z, Zero to Nine, SpaceBar, Enter, Escape, LeftShift, "
			"LeftControl, Left, Right, Up, Down, LeftMouseButton, RightMouseButton, MouseX, MouseY, Gamepad_LeftX, Gamepad_LeftY, Gamepad_RightX, Gamepad_RightY, "
			"Gamepad_FaceButton_Bottom (Xbox A), Gamepad_FaceButton_Right (B), Gamepad_FaceButton_Left (X), Gamepad_FaceButton_Top (Y), Gamepad_LeftShoulder, Gamepad_RightShoulder, "
			"Gamepad_LeftTrigger, Gamepad_RightTrigger, Gamepad_LeftThumbstick, Gamepad_RightThumbstick, Gamepad_DPad_Up/Down/Left/Right."),
			*Value, Close.Num() > 0 ? *(TEXT(" Close matches: ") + FString::Join(Close, TEXT(", ")) + TEXT(".")) : TEXT(""));
		return false;
	}
}
