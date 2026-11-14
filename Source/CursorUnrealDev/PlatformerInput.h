#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PlatformerInput.generated.h"

UCLASS()
class CURSORUNREALDEV_API UPlatformerInput : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Reads a named axis from the player controller. Name pins stay editable; raw key pins do not. */
	UFUNCTION(BlueprintPure, Category="Platformer")
	static float GetPlatformerAxis(APlayerController* Controller, FName AxisName);

	UFUNCTION(BlueprintPure, Category="Platformer")
	static float PlatformerPeekAxis(APlayerController* Controller);

	UFUNCTION(BlueprintPure, Category="Platformer")
	static float PlatformerStickPitchAxis(APlayerController* Controller);

	UFUNCTION(BlueprintPure, Category="Platformer")
	static float PlatformerJumpAxis(APlayerController* Controller);
};
