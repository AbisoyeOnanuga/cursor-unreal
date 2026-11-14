#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/** Small JSON helpers shared by every module. */
namespace CursorJson
{
	CURSORUNREALCORE_API FString ToString(const TSharedRef<FJsonObject>& Object, bool bPretty = false);
	CURSORUNREALCORE_API FString ToString(const TArray<TSharedPtr<FJsonValue>>& Array, bool bPretty = false);

	/** Returns null if the text is not a JSON object. */
	CURSORUNREALCORE_API TSharedPtr<FJsonObject> Parse(const FString& Text);

	/** Returns null if the text is not valid JSON. Accepts objects, arrays and scalars. */
	CURSORUNREALCORE_API TSharedPtr<FJsonValue> ParseValue(const FString& Text);

	/** Cuts text to MaxChars and appends a note saying how much was dropped. */
	CURSORUNREALCORE_API FString Truncate(const FString& Text, int32 MaxChars);

	CURSORUNREALCORE_API TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Strings);
	CURSORUNREALCORE_API TSharedRef<FJsonObject> VectorToJson(const FVector& V);
	CURSORUNREALCORE_API TSharedRef<FJsonObject> RotatorToJson(const FRotator& R);
}
