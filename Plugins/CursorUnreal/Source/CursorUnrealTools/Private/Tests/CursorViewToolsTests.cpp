#if WITH_DEV_AUTOMATION_TESTS

// Tests for viewport_screenshot. What it refuses is checked everywhere. The pictures themselves need a
// graphics card, so they are only taken when the editor has a renderer: Scripts/Run-Tests.ps1 -Rendering.
// Started that way the pictures are also kept, under Saved/CursorUnreal/TestShots, so a person can look at them.

#include "CursorPackTestSupport.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "CursorUnrealLog.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "EngineUtils.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace CursorViewTest
{
	using namespace CursorPackTest;

	const TCHAR* CubePath = TEXT("/Engine/BasicShapes/Cube");

	struct FShotCase
	{
		FString Name;
		FString ArgsJson;
		int32 Width = 1280;
		int32 Height = 720;
	};

	struct FState
	{
		CursorTest::FScopedPolicy Policy{ NAME_None, ECursorApprovalPolicy::AllowAll };
		TArray<FString> Labels;
		TArray<FShotCase> Cases;
		int32 Next = 0;
		bool bRunning = false;
		bool bDone = false;
		FCursorToolResult Result;
		double StartedAt = 0.0;

		~FState()
		{
			UEditorActorSubsystem* Actors = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
			for (const FString& Label : Labels)
			{
				FString Error;
				AActor* Actor = CursorToolHelpers::FindActor(Label, Error);
				if (Actor && Actors)
				{
					Actors->DestroyActor(Actor);
				}
			}
		}
	};

	/** Average colour of a square around a point given as fractions of the picture, from BGRA bytes. */
	FLinearColor Sample(const TArray<uint8>& Bgra, int32 Width, int32 Height, double U, double V)
	{
		const int32 CenterX = FMath::Clamp(FMath::RoundToInt32(U * Width), 3, Width - 4);
		const int32 CenterY = FMath::Clamp(FMath::RoundToInt32(V * Height), 3, Height - 4);
		FLinearColor Sum(0, 0, 0, 0);
		for (int32 Y = CenterY - 3; Y <= CenterY + 3; ++Y)
		{
			for (int32 X = CenterX - 3; X <= CenterX + 3; ++X)
			{
				const uint8* Pixel = &Bgra[(Y * Width + X) * 4];
				Sum += FLinearColor(Pixel[2], Pixel[1], Pixel[0], 0);
			}
		}
		return Sum / 49.0f;
	}

	double Difference(const FLinearColor& A, const FLinearColor& B)
	{
		return FMath::Abs(A.R - B.R) + FMath::Abs(A.G - B.G) + FMath::Abs(A.B - B.B);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorViewScreenshotTest, "CursorUnreal.Tools.Screenshot", CursorTest::Flags)
bool FCursorViewScreenshotTest::RunTest(const FString&)
{
	using namespace CursorViewTest;

	if (!TestNotNull(TEXT("a level is open"), CursorToolHelpers::GetEditorWorld()))
	{
		return false;
	}
	const TSharedRef<FState> State = MakeShared<FState>();

	// A small scene far above the template level: a floor, a tall block on it, and a low block beside that.
	const FString Floor = State->Labels.Add_GetRef(UniqueName(TEXT("ShotFloor")));
	const FString Tower = State->Labels.Add_GetRef(UniqueName(TEXT("ShotTower")));
	const FString Step = State->Labels.Add_GetRef(UniqueName(TEXT("ShotStep")));
	auto Spawn = [this](const FString& Json)
	{
		const FCursorToolResult R = CursorTest::Call(TEXT("actor_spawn"), *Json);
		TestFalse(TEXT("scene actor placed: ") + R.Text, R.bIsError);
	};
	Spawn(Args(TEXT("{\"class\":\"{0}\",\"location\":[0,0,29950],\"scale\":[12,8,1],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), CubePath, Floor));
	Spawn(Args(TEXT("{\"class\":\"{0}\",\"location\":[-300,0,30200],\"scale\":[2,2,4],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), CubePath, Tower));
	Spawn(Args(TEXT("{\"class\":\"{0}\",\"location\":[200,100,30050],\"scale\":[3,1,1],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), CubePath, Step));

	// A sun, a sky and light from the sky when the level has none, so the lit pictures show a lit scene whatever
	// level the tests run in. A level that has its own is left with its own: two suns would only wash the pictures out.
	const bool bLevelHasSun = TActorIterator<ADirectionalLight>(CursorToolHelpers::GetEditorWorld()) ? true : false;
	UE_LOG(LogCursorUnreal, Display, TEXT("[shot] the level %s"), bLevelHasSun ? TEXT("has its own sun") : TEXT("has no sun; the test adds one"));
	const FString Sun = UniqueName(TEXT("ShotSun"));
	const FString Sky = UniqueName(TEXT("ShotSky"));
	const FString SkyLight = UniqueName(TEXT("ShotSkyLight"));
	if (!bLevelHasSun)
	{
	State->Labels.Append({ Sun, Sky, SkyLight });
	Spawn(Args(TEXT("{\"class\":\"DirectionalLight\",\"location\":[0,0,30800],\"rotation\":[-50,35,0],\"label\":\"{0}\",\"folder\":\"CursorUnrealTests\"}"), Sun));
	Spawn(Args(TEXT("{\"class\":\"SkyAtmosphere\",\"location\":[0,0,0],\"label\":\"{0}\",\"folder\":\"CursorUnrealTests\"}"), Sky));
	Spawn(Args(TEXT("{\"class\":\"SkyLight\",\"location\":[0,0,30800],\"label\":\"{0}\",\"folder\":\"CursorUnrealTests\"}"), SkyLight));
	{
		FString Error;
		if (ADirectionalLight* SunActor = Cast<ADirectionalLight>(CursorToolHelpers::FindActor(Sun, Error)))
		{
			SunActor->GetLightComponent()->SetMobility(EComponentMobility::Movable);
		}
		if (ASkyLight* SkyLightActor = Cast<ASkyLight>(CursorToolHelpers::FindActor(SkyLight, Error)))
		{
			SkyLightActor->GetLightComponent()->SetMobility(EComponentMobility::Movable);
			SkyLightActor->GetLightComponent()->SetRealTimeCapture(true);
		}
	}
	}

	// What is refused, with or without a graphics card.
	{
		FCursorToolResult R = CursorTest::Call(TEXT("viewport_screenshot"), TEXT("{\"view\":\"underneath\"}"));
		TestTrue(TEXT("an unknown view is refused, with the list: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("top, front, back, right, left or iso")));
		R = CursorTest::Call(TEXT("viewport_screenshot"), TEXT("{\"view\":\"top\",\"mode\":\"wireframe\"}"));
		TestTrue(TEXT("an unknown mode is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("\"lit\" or \"unlit\"")));
		R = CursorTest::Call(TEXT("viewport_screenshot"), TEXT("{\"location\":[0,0,31000]}"));
		TestTrue(TEXT("a place without a direction is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("look_at")));
		R = CursorTest::Call(TEXT("viewport_screenshot"), TEXT("{\"location\":[0,0,31000],\"look_at\":[0,0,31000]}"));
		TestTrue(TEXT("looking at where the camera is, is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("same point")));
		R = CursorTest::Call(TEXT("viewport_screenshot"), TEXT("{\"location\":[0,0,31000],\"rotation\":[-90,0,0],\"projection\":\"orthographic\"}"));
		TestTrue(TEXT("an orthographic picture with nothing to size it by is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("size it by")));
		R = CursorTest::Call(TEXT("viewport_screenshot"), TEXT("{\"view\":\"top\",\"projection\":\"fisheye\"}"));
		TestTrue(TEXT("an unknown projection is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("Unknown projection")));
		R = CursorTest::Call(TEXT("viewport_screenshot"), TEXT("{\"camera\":\"NoSuchCamera_123\"}"));
		TestTrue(TEXT("a camera that is not in the level is refused"), R.bIsError);
		R = CursorTest::Call(TEXT("viewport_screenshot"), *Args(TEXT("{\"camera\":\"{0}\"}"), Tower));
		TestTrue(TEXT("an actor that is not a camera is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("no camera component")));
		R = CursorTest::Call(TEXT("viewport_screenshot"), TEXT("{\"view\":\"top\",\"frame\":[\"NoSuchActor_123\"]}"));
		TestTrue(TEXT("framing an actor that is not there is refused"), R.bIsError);
	}

	if (!FApp::CanEverRender())
	{
		const FCursorToolResult R = CursorTest::Call(TEXT("viewport_screenshot"), TEXT("{\"view\":\"top\"}"));
		TestTrue(TEXT("without a renderer the tool says so: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("without a renderer")));
		AddInfo(TEXT("This editor has no renderer, so no pictures were taken. Run Scripts/Run-Tests.ps1 -Rendering to test them."));
		return true;
	}

	const FString Frame = FString::Printf(TEXT("\"frame\":[\"%s\",\"%s\",\"%s\"]"), *Floor, *Tower, *Step);
	State->Cases.Add({ TEXT("top"), FString::Printf(TEXT("{\"view\":\"top\",%s}"), *Frame) });
	State->Cases.Add({ TEXT("right"), FString::Printf(TEXT("{\"view\":\"right\",%s}"), *Frame) });
	State->Cases.Add({ TEXT("iso"), FString::Printf(TEXT("{\"view\":\"iso\",%s}"), *Frame) });
	State->Cases.Add({ TEXT("top-unlit"), FString::Printf(TEXT("{\"view\":\"top\",\"mode\":\"unlit\",%s}"), *Frame) });
	State->Cases.Add({ TEXT("iso-unlit-small"), FString::Printf(TEXT("{\"view\":\"iso\",\"mode\":\"unlit\",\"width\":640,\"height\":640,%s}"), *Frame), 640, 640 });
	State->Cases.Add({ TEXT("placed"), TEXT("{\"location\":[900,-1100,30600],\"look_at\":[-100,0,30100],\"fov\":60}") });
	State->Cases.Add({ TEXT("whole-level"), TEXT("{\"view\":\"iso\"}") });
	State->Cases.Add({ TEXT("current"), TEXT("{}") });

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State]()
	{
		if (!State->bRunning)
		{
			if (State->Next >= State->Cases.Num())
			{
				return true;
			}
			State->bRunning = true;
			State->bDone = false;
			State->StartedAt = FPlatformTime::Seconds();
			FCursorToolContext Ctx;
			Ctx.Caller = FCursorToolRegistry::CallerNative;
			Ctx.SessionId = TEXT("test");
			FCursorToolRegistry::Get().Invoke(TEXT("viewport_screenshot"), CursorJson::Parse(State->Cases[State->Next].ArgsJson), Ctx,
				[State](const FCursorToolResult& Result) { State->Result = Result; State->bDone = true; });
			return false;
		}
		if (!State->bDone)
		{
			if (FPlatformTime::Seconds() - State->StartedAt > 120.0)
			{
				AddError(FString::Printf(TEXT("The '%s' picture was not finished within two minutes."), *State->Cases[State->Next].Name));
				return true;
			}
			return false;
		}

		const FShotCase& Case = State->Cases[State->Next];
		const FCursorToolResult& R = State->Result;
		State->bRunning = false;
		++State->Next;
		UE_LOG(LogCursorUnreal, Display, TEXT("[shot] %s took %.1f s -> %s"), *Case.Name, FPlatformTime::Seconds() - State->StartedAt, *R.Text);

		if (Case.Name == TEXT("current") && R.bIsError && R.Text.Contains(TEXT("no perspective level viewport")))
		{
			// A headless editor has no level viewport to copy the camera from.
			return false;
		}
		if (!TestFalse(FString::Printf(TEXT("'%s' is taken: %s"), *Case.Name, *R.Text), R.bIsError) || !R.Structured.IsValid())
		{
			return false;
		}
		if (!TestEqual(Case.Name + TEXT(": one picture comes back"), R.Images.Num(), 1))
		{
			return false;
		}
		const FCursorImage& Image = R.Images[0];
		TestEqual(Case.Name + TEXT(": a JPEG"), Image.MimeType, FString(TEXT("image/jpeg")));
		TestTrue(Case.Name + TEXT(": at the size asked for"), Image.Width == Case.Width && Image.Height == Case.Height);
		TestTrue(Case.Name + TEXT(": enough frames were rendered for the picture to settle"), R.Structured->GetNumberField(TEXT("frames_rendered")) >= 12.0);
		TestFalse(Case.Name + TEXT(": the picture's bytes are not in the text"), R.Text.Contains(Image.ToBase64().Left(40)));

		if (FParse::Param(FCommandLine::Get(), TEXT("CursorUnrealKeepShots")))
		{
			const FString File = FPaths::ProjectSavedDir() / TEXT("CursorUnreal") / TEXT("TestShots") / Case.Name + TEXT(".jpg");
			FFileHelper::SaveArrayToFile(*Image.Data, *File);
		}

		TArray<uint8> Bgra;
		int32 Width = 0;
		int32 Height = 0;
		if (!TestTrue(Case.Name + TEXT(": decodes"), CursorImages::DecodeForDisplay(Image, 1568, Bgra, Width, Height)))
		{
			return false;
		}
		const bool bNotedBlack = R.Text.Contains(TEXT("almost entirely black"));
		const FLinearColor Middle = Sample(Bgra, Width, Height, 0.5, 0.5);
		const FLinearColor Corner = Sample(Bgra, Width, Height, 0.02, 0.03);
		UE_LOG(LogCursorUnreal, Display, TEXT("[shot] %s middle (%.0f, %.0f, %.0f) corner (%.0f, %.0f, %.0f)"), *Case.Name, Middle.R, Middle.G, Middle.B, Corner.R, Corner.G, Corner.B);
		if (Case.Name.Contains(TEXT("unlit")))
		{
			// Without lighting a surface shows its own colour, whatever the level's lights are doing.
			TestTrue(Case.Name + TEXT(": the floor is visible without lights"), Middle.R + Middle.G + Middle.B > 60.0f);
			TestFalse(Case.Name + TEXT(": and nothing is said about a black picture"), bNotedBlack);
		}
		else if (!bNotedBlack)
		{
			TestTrue(Case.Name + TEXT(": something is visible"), Middle.R + Middle.G + Middle.B > 15.0f);
		}

		const TSharedPtr<FJsonObject> Camera = R.Structured->GetObjectField(TEXT("camera"));
		if (Case.Name.StartsWith(TEXT("top")))
		{
			TestEqual(TEXT("top: a plan, without foreshortening"), Camera->GetStringField(TEXT("projection")), FString(TEXT("orthographic")));
			TestEqual(TEXT("top: which way is right"), Camera->GetStringField(TEXT("picture_right_is")), FString(TEXT("+Y")));
			TestEqual(TEXT("top: which way is up"), Camera->GetStringField(TEXT("picture_up_is")), FString(TEXT("+X")));
			// The floor is 1200 x 800: with +X up the picture is 800 wide and 1200 tall, so the height decides.
			TestTrue(TEXT("top: the picture covers the floor with a margin"), FMath::IsNearlyEqual(Camera->GetNumberField(TEXT("picture_height_cm")), 1200.0 * 1.12, 2.0));
			TestEqual(TEXT("top: three actors framed"), static_cast<int32>(R.Structured->GetObjectField(TEXT("framed"))->GetNumberField(TEXT("actors"))), 3);
			// The floor fills the middle; left of it (-Y, beyond the 400 cm half width) is whatever is far below.
			const FLinearColor Beside = Sample(Bgra, Width, Height, 0.1, 0.5);
			if (!bNotedBlack)
			{
				TestTrue(TEXT("top: the floor stands out from what is beside it"), Difference(Middle, Beside) > 12.0);
			}
		}
		if (Case.Name == TEXT("right"))
		{
			TestEqual(TEXT("right: an elevation"), Camera->GetStringField(TEXT("projection")), FString(TEXT("orthographic")));
			TestEqual(TEXT("right: +X is to the right"), Camera->GetStringField(TEXT("picture_right_is")), FString(TEXT("+X")));
			TestEqual(TEXT("right: up is up"), Camera->GetStringField(TEXT("picture_up_is")), FString(TEXT("+Z")));
			// Seen from +Y the tower (x -400..-200, z 30000..30400) is left of the middle and reaches well above the floor.
			// The framed box is x -600..600, z 29900..30400, shown 1344 cm wide.
			const double U = 0.5 + (-300.0) / 1344.0;
			const FLinearColor OnTower = Sample(Bgra, Width, Height, U, 0.5 - 100.0 / (1344.0 * 720.0 / 1280.0));
			const FLinearColor AboveStep = Sample(Bgra, Width, Height, 0.5 + 250.0 / 1344.0, 0.5 - 100.0 / (1344.0 * 720.0 / 1280.0));
			if (!bNotedBlack)
			{
				TestTrue(TEXT("right: the tower is where its coordinates say, and the air beside it is not"), Difference(OnTower, AboveStep) > 12.0);
			}
		}
		if (Case.Name == TEXT("iso") || Case.Name == TEXT("placed"))
		{
			TestEqual(Case.Name + TEXT(": perspective"), Camera->GetStringField(TEXT("projection")), FString(TEXT("perspective")));
		}
		if (Case.Name == TEXT("whole-level"))
		{
			TestTrue(TEXT("whole level: more than the three test actors were framed"), R.Structured->GetObjectField(TEXT("framed"))->GetNumberField(TEXT("actors")) >= 3.0);
		}
		return false;
	}));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
