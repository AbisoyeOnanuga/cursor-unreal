#include "PlatformerInput.h"

#include "GameFramework/PlayerController.h"

float UPlatformerInput::GetPlatformerAxis(APlayerController* Controller, FName AxisName)
{
	return Controller ? Controller->GetInputAxisValue(AxisName) : 0.f;
}

float UPlatformerInput::PlatformerPeekAxis(APlayerController* Controller)
{
	return Controller ? Controller->GetInputAxisValue(TEXT("PlatformerPeek")) : 0.f;
}

float UPlatformerInput::PlatformerStickPitchAxis(APlayerController* Controller)
{
	return Controller ? Controller->GetInputAxisValue(TEXT("PlatformerStickPitch")) : 0.f;
}

float UPlatformerInput::PlatformerJumpAxis(APlayerController* Controller)
{
	return Controller ? Controller->GetInputAxisValue(TEXT("PlatformerJump")) : 0.f;
}
