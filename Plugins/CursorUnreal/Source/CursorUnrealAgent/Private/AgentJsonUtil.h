#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Quiet readers for JSON that came from outside. FJsonObject's Get*Field functions log a warning
 * for every missing field, and API replies leave fields out or send null all the time.
 */
namespace AgentJson
{
	inline FString Str(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FString Value;
		if (Object.IsValid())
		{
			Object->TryGetStringField(Field, Value);
		}
		return Value;
	}

	inline double Num(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, double Default = 0.0)
	{
		double Value = Default;
		if (Object.IsValid())
		{
			Object->TryGetNumberField(Field, Value);
		}
		return Value;
	}

	inline bool Bool(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field, bool bDefault = false)
	{
		bool bValue = bDefault;
		if (Object.IsValid())
		{
			Object->TryGetBoolField(Field, bValue);
		}
		return bValue;
	}

	/** Null when the field is missing, null or not an object. */
	inline TSharedPtr<FJsonObject> Obj(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Value = nullptr;
		if (Object.IsValid() && Object->TryGetObjectField(Field, Value) && Value)
		{
			return *Value;
		}
		return nullptr;
	}

	/**
	 * Null when the field is missing or not an array. The result points into Object, so Object has to
	 * outlive it: keep the object in a variable first.
	 */
	inline const TArray<TSharedPtr<FJsonValue>>* Arr(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		const TArray<TSharedPtr<FJsonValue>>* Value = nullptr;
		if (Object.IsValid() && Object->TryGetArrayField(Field, Value))
		{
			return Value;
		}
		return nullptr;
	}

	/** Refused at compile time: the array would be freed with the temporary before it could be read. */
	const TArray<TSharedPtr<FJsonValue>>* Arr(TSharedPtr<FJsonObject>&& Object, const TCHAR* Field) = delete;
}
