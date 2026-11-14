#include "CursorToolPacks.h"

#include "AssetCompilingManager.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Containers/Ticker.h"
#include "CursorJson.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "Editor.h"
#include "Engine/Brush.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "ShowFlags.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

/*
 * View pack. One tool, viewport_screenshot: render the level from a camera and give the picture to the model.
 * It exists so an agent can look at what it built (does the blockout read, is the scene lit, does it match the
 * reference picture the user attached) instead of reasoning about coordinates alone.
 *
 * The picture is rendered off screen with a scene capture rather than read back from the level viewport, so it
 * does not depend on which window has focus or how big the viewport is, shows no gizmos or selection outlines,
 * and can be taken from any camera without moving the user's view.
 */

namespace
{
	const TCHAR* ViewPack = TEXT("view");

	constexpr int32 DefaultWidth = 1280;
	constexpr int32 DefaultHeight = 720;
	constexpr int32 MinEdge = 256;
	/** Frames rendered before the picture is read back, so exposure, global illumination and anti-aliasing settle. */
	constexpr int32 SettleFrames = 60; // TEMP-EXPERIMENT
	/** Longest the tool waits for shaders and textures that are still being prepared. */
	constexpr double MaxWaitSeconds = 20.0;
	/** Actors larger than this are left out when choosing what to frame: sky domes, backdrops. */
	constexpr double MaxFramedActorSize = 200000.0;

	TSharedPtr<FJsonValue> Tenths(double Value)
	{
		FString Text = FString::Printf(TEXT("%.1f"), Value);
		Text.RemoveFromEnd(TEXT(".0"));
		if (Text == TEXT("-0"))
		{
			Text = TEXT("0");
		}
		return MakeShared<FJsonValueNumberString>(Text);
	}

	TSharedPtr<FJsonValue> VectorJson(const FVector& V)
	{
		return MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{ Tenths(V.X), Tenths(V.Y), Tenths(V.Z) });
	}

	/** "+X", "-Z" and so on for a direction that lies along an axis; empty otherwise. */
	FString AxisName(const FVector& Direction)
	{
		const TCHAR* Names[3] = { TEXT("X"), TEXT("Y"), TEXT("Z") };
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (FMath::Abs(Direction[Axis]) > 0.999)
			{
				return FString::Printf(TEXT("%s%s"), Direction[Axis] > 0.0 ? TEXT("+") : TEXT("-"), Names[Axis]);
			}
		}
		return FString();
	}

	/** The camera a picture is taken from. */
	struct FShot
	{
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		bool bOrthographic = false;
		float Fov = 90.0f;
		float OrthoWidth = 1000.0f;
		int32 Width = DefaultWidth;
		int32 Height = DefaultHeight;
		bool bUnlit = false;
		FString ViewName;
		TOptional<FPostProcessSettings> PostProcess;
	};

	/** The visible, in-game parts of an actor. Invalid when it has none. */
	FBox VisibleBounds(const AActor* Actor)
	{
		FBox Box(ForceInit);
		TInlineComponentArray<UPrimitiveComponent*> Components(Actor);
		for (const UPrimitiveComponent* Component : Components)
		{
			if (Component->IsRegistered() && Component->IsVisible() && !Component->bHiddenInGame && !Component->IsEditorOnly())
			{
				Box += Component->Bounds.GetBox();
			}
		}
		return Box;
	}

	/**
	 * Everything in the level worth looking at: placed geometry, without skies, volumes and editor helpers. Terrain is
	 * left out too when there is anything else, because a landscape is kilometres wide and what was built on it would
	 * be a dot in the picture; bOutTerrainLeftOut says when that happened.
	 */
	FBox LevelBounds(UWorld* World, int32& OutActors, bool& bOutTerrainLeftOut)
	{
		FBox Placed(ForceInit);
		FBox Terrain(ForceInit);
		int32 NumPlaced = 0;
		int32 NumTerrain = 0;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			const AActor* Actor = *It;
			const FString ClassName = Actor->GetClass()->GetName();
			if (Actor->IsHidden() || Actor->IsEditorOnly() || Actor->IsA<ABrush>() || ClassName.Contains(TEXT("Sky")))
			{
				continue;
			}
			const FBox Box = VisibleBounds(Actor);
			if (!Box.IsValid || Box.GetSize().GetMax() > MaxFramedActorSize)
			{
				continue;
			}
			if (ClassName.Contains(TEXT("Landscape")))
			{
				Terrain += Box;
				++NumTerrain;
			}
			else
			{
				Placed += Box;
				++NumPlaced;
			}
		}
		bOutTerrainLeftOut = NumPlaced > 0 && NumTerrain > 0;
		OutActors = NumPlaced > 0 ? NumPlaced : NumTerrain;
		return NumPlaced > 0 ? Placed : Terrain;
	}

	/** Places the camera of Shot, which already has its rotation and projection, so that Box fills the picture. */
	void FrameBox(const FBox& Box, FShot& Shot)
	{
		const FVector Center = Box.GetCenter();
		const FVector Extent = Box.GetExtent().ComponentMax(FVector(25.0));
		const FRotationMatrix Axes(Shot.Rotation);
		const FVector Forward = Axes.GetUnitAxis(EAxis::X);
		const FVector Right = Axes.GetUnitAxis(EAxis::Y);
		const FVector Up = Axes.GetUnitAxis(EAxis::Z);
		auto HalfSpan = [&Extent](const FVector& Axis)
		{
			return FMath::Abs(Axis.X) * Extent.X + FMath::Abs(Axis.Y) * Extent.Y + FMath::Abs(Axis.Z) * Extent.Z;
		};
		const double HalfWidth = HalfSpan(Right);
		const double HalfHeight = HalfSpan(Up);
		const double HalfDepth = HalfSpan(Forward);
		const double Aspect = static_cast<double>(Shot.Width) / Shot.Height;
		constexpr double Margin = 1.12;

		if (Shot.bOrthographic)
		{
			Shot.OrthoWidth = static_cast<float>(2.0 * FMath::Max(HalfWidth, HalfHeight * Aspect) * Margin);
			Shot.Location = Center - Forward * (HalfDepth + 1000.0);
		}
		else
		{
			const double HalfHorizontal = FMath::DegreesToRadians(Shot.Fov * 0.5);
			const double HalfVertical = FMath::Atan(FMath::Tan(HalfHorizontal) / Aspect);
			// Far enough that the near face of the box fits both ways.
			const double Distance = HalfDepth + Margin * FMath::Max(HalfWidth / FMath::Tan(HalfHorizontal), HalfHeight / FMath::Tan(HalfVertical));
			Shot.Location = Center - Forward * Distance;
		}
	}

	/** One screenshot being taken. Lives from the tool call until the picture has been handed back. */
	class FScreenshotRun : public TSharedFromThis<FScreenshotRun>
	{
	public:
		FShot Shot;
		TWeakObjectPtr<UWorld> World;
		TSharedRef<FJsonObject> Info = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Notes;
		FCursorToolCompletion Done;

		void Begin()
		{
			Target.Reset(NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient));
			Target->ClearColor = FLinearColor::Black;
			// The renderer's final colour is already encoded for a display. The target must store those bytes as they are
			// (so: not an sRGB texture, which would encode them a second time and wash the picture out) while still telling
			// the renderer that a display gamma is wanted (so: TargetGamma, which is what the tone mapper reads).
			Target->TargetGamma = 2.2f;
			Target->InitCustomFormat(Shot.Width, Shot.Height, PF_B8G8R8A8, /*bForceLinearGamma*/ true);
			Target->UpdateResourceImmediate(true);

			Capture.Reset(NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient));
			Capture->bCaptureEveryFrame = false;
			Capture->bCaptureOnMovement = false;
			// Keeps exposure, anti-aliasing and global illumination history between the frames rendered below.
			Capture->bAlwaysPersistRenderingState = true;
			Capture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
			Capture->TextureTarget = Target.Get();
			Capture->ProjectionType = Shot.bOrthographic ? ECameraProjectionMode::Orthographic : ECameraProjectionMode::Perspective;
			Capture->FOVAngle = Shot.Fov;
			Capture->OrthoWidth = Shot.OrthoWidth;
			if (Shot.PostProcess.IsSet())
			{
				Capture->PostProcessSettings = Shot.PostProcess.GetValue();
				Capture->PostProcessBlendWeight = 1.0f;
			}
			Capture->SetRelativeLocationAndRotation(Shot.Location, Shot.Rotation);
			Capture->RegisterComponentWithWorld(World.Get());

			// After registering, which resets the show flags to the component's saved settings.
			Capture->ShowFlags.SetMotionBlur(false);
			if (Shot.bUnlit)
			{
				// The same switches the level viewport's Unlit view mode sets.
				ApplyViewMode(VMI_Unlit, !Shot.bOrthographic, Capture->ShowFlags);
				EngineShowFlagOverride(ESFIM_Game, VMI_Unlit, Capture->ShowFlags, false);
			}

			StartedAt = FPlatformTime::Seconds();
			// The ticker owns this object from here on (a CreateSP delegate would only hold it weakly, and nothing else does),
			// and lets go of it when Tick returns false.
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Self = AsShared()](float DeltaSeconds) { return Self->Tick(DeltaSeconds); }));
		}

	private:
		bool Tick(float)
		{
			if (!World.IsValid() || !Capture.IsValid() || !Capture->IsRegistered())
			{
				Finish(FCursorToolResult::Error(TEXT("The level was closed or replaced while the picture was being taken. Call viewport_screenshot again.")));
				return false;
			}
			Capture->CaptureScene();
			++Frames;

			const int32 Preparing = FAssetCompilingManager::Get().GetNumRemainingAssets();
			const bool bTimedOut = FPlatformTime::Seconds() - StartedAt > MaxWaitSeconds;
			if (Frames < SettleFrames || (Preparing > 0 && !bTimedOut))
			{
				return true;
			}
			if (Preparing > 0)
			{
				Notes.Add(MakeShared<FJsonValueString>(FString::Printf(
					TEXT("%d shaders, textures or meshes were still being prepared when this was taken, so some surfaces may show a grey placeholder. Take it again in a little while."), Preparing)));
			}

			TArray<FColor> Pixels;
			FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
			if (Resource == nullptr || !Resource->ReadPixels(Pixels) || Pixels.Num() != Shot.Width * Shot.Height)
			{
				Finish(FCursorToolResult::Error(TEXT("The picture was rendered but could not be read back from the graphics card.")));
				return false;
			}

			// A level with no light renders black. Saying so saves the model from describing a black rectangle.
			int64 Sum = 0;
			for (int32 Index = 0; Index < Pixels.Num(); Index += 97)
			{
				Sum += Pixels[Index].R + Pixels[Index].G + Pixels[Index].B;
			}
			const double Average = static_cast<double>(Sum) / (3.0 * FMath::Max(1, Pixels.Num() / 97));
			if (Average < 3.0 && !Shot.bUnlit)
			{
				Notes.Add(MakeShared<FJsonValueString>(TEXT("The picture is almost entirely black. The level probably has no lights, or the camera is inside or facing away from everything. "
					"Use \"mode\": \"unlit\" to see shapes without lighting.")));
			}

			FCursorImage Image;
			FString Error;
			if (!CursorImages::FromPixels(Pixels, Shot.Width, Shot.Height, FString::Printf(TEXT("viewport (%s)"), *Shot.ViewName), true, Image, Error))
			{
				Finish(FCursorToolResult::Error(Error));
				return false;
			}

			Info->SetNumberField(TEXT("frames_rendered"), Frames);
			if (Notes.Num() > 0)
			{
				Info->SetArrayField(TEXT("notes"), Notes);
			}
			FCursorToolResult Result = FCursorToolResult::OkJson(Info);
			Result.Images.Add(MoveTemp(Image));
			Finish(Result);
			return false;
		}

		void Finish(const FCursorToolResult& Result)
		{
			if (Capture.IsValid())
			{
				Capture->TextureTarget = nullptr;
				if (Capture->IsRegistered())
				{
					Capture->UnregisterComponent();
				}
				Capture->MarkAsGarbage();
			}
			if (Target.IsValid())
			{
				Target->ReleaseResource();
				Target->MarkAsGarbage();
			}
			Capture.Reset();
			Target.Reset();

			FCursorToolCompletion Callback = MoveTemp(Done);
			Done = nullptr;
			if (Callback)
			{
				Callback(Result);
			}
		}

		TStrongObjectPtr<USceneCaptureComponent2D> Capture;
		TStrongObjectPtr<UTextureRenderTarget2D> Target;
		double StartedAt = 0.0;
		int32 Frames = 0;
	};

	/** The camera of the level viewport the user is working in, or of the first perspective one. */
	bool ViewportCamera(FShot& Shot)
	{
		const FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient;
		if (Client == nullptr || !Client->IsPerspective())
		{
			Client = nullptr;
			for (const FLevelEditorViewportClient* Candidate : GEditor->GetLevelViewportClients())
			{
				if (Candidate && Candidate->IsPerspective())
				{
					Client = Candidate;
					break;
				}
			}
		}
		if (Client == nullptr)
		{
			return false;
		}
		Shot.Location = Client->GetViewLocation();
		Shot.Rotation = Client->GetViewRotation();
		Shot.Fov = Client->ViewFOV;
		return true;
	}
}

void CursorToolPacks::RegisterViewPack()
{
	FCursorToolBuilder(TEXT("viewport_screenshot"), ViewPack)
		.Describe(TEXT("Take a picture of the open level and look at it. Renders the level off screen from a camera you choose and returns the picture to you, "
			"with where the camera was. Use it to check your own work the way a person would: after blocking out a level (\"view\": \"top\" or \"right\" shows the "
			"whole layout), after placing lights, fog, sky or effects, and to compare against a reference picture the user attached. "
			"Views: \"current\" is what the user's level viewport shows (during Play, what the player sees); \"top\" looks straight down; "
			"\"front\", \"back\", \"right\" and \"left\" look from the +X, -X, +Y and -Y side; \"iso\" looks down from a corner. Views other than \"current\" "
			"frame the whole level, or just the actors listed in 'frame'. Or give 'camera' (a camera actor in the level) or 'location' with 'rotation' or 'look_at'. "
			"Gizmos, selection outlines and editor-only helpers are never in the picture; volumes and anything hidden in game are not either. "
			"Each picture costs about as much as 1,500 words of text every time the conversation is sent, so take the ones you need, not one after every step."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.Enum(TEXT("view"), TEXT("Where to look from. Default \"current\"."), { TEXT("current"), TEXT("top"), TEXT("front"), TEXT("back"), TEXT("right"), TEXT("left"), TEXT("iso") })
			.StrArray(TEXT("frame"), TEXT("Labels of actors to fit in the picture, for views other than \"current\". Default: everything placed in the level."))
			.Str(TEXT("camera"), TEXT("Label of an actor with a camera component. The picture is what that camera sees, with its field of view and post-process settings."))
			.Vec3(TEXT("location"), TEXT("Put the camera here [x, y, z]. Needs 'rotation' or 'look_at'."))
			.Vec3(TEXT("rotation"), TEXT("With 'location': which way the camera faces [pitch, yaw, roll]."))
			.Vec3(TEXT("look_at"), TEXT("With 'location': a point [x, y, z] for the camera to face."))
			.Enum(TEXT("projection"), TEXT("\"perspective\" or \"orthographic\" (no foreshortening, like a plan or an elevation). Default: orthographic for top, front, back, right and left; perspective otherwise."),
				{ TEXT("perspective"), TEXT("orthographic") })
			.Num(TEXT("fov"), TEXT("Horizontal field of view in degrees for a perspective picture. Default: the viewport's or camera's own, otherwise 70."))
			.Enum(TEXT("mode"), TEXT("\"lit\" (default) is the level as the game shows it. \"unlit\" shows surface colours without lighting, for a level that has no lights yet."),
				{ TEXT("lit"), TEXT("unlit") })
			.Int(TEXT("width"), TEXT("Picture width in pixels, 256 to 1568. Default 1280."))
			.Int(TEXT("height"), TEXT("Picture height in pixels, 256 to 1568. Default 720.")))
		.RunAsync([](const FCursorArgs& Args, const FCursorToolContext&, FCursorToolCompletion Done)
		{
			if (!GEditor || !CursorToolHelpers::GetEditorWorld())
			{
				Done(FCursorToolResult::Error(TEXT("No level is open in the editor.")));
				return;
			}
			// While the game is running in the editor, the picture is of the game.
			UWorld* World = GEditor->PlayWorld ? GEditor->PlayWorld.Get() : CursorToolHelpers::GetEditorWorld();

			const TSharedRef<FScreenshotRun> Run = MakeShared<FScreenshotRun>();
			FShot& Shot = Run->Shot;
			Shot.Width = FMath::Clamp(Args.Int(TEXT("width"), DefaultWidth), MinEdge, CursorImages::MaxLongEdge);
			Shot.Height = FMath::Clamp(Args.Int(TEXT("height"), DefaultHeight), MinEdge, CursorImages::MaxLongEdge);
			Shot.bUnlit = Args.Str(TEXT("mode"), TEXT("lit")).Equals(TEXT("unlit"), ESearchCase::IgnoreCase);
			const FString Mode = Args.Str(TEXT("mode"), TEXT("lit")).ToLower();
			if (Mode != TEXT("lit") && Mode != TEXT("unlit"))
			{
				Done(FCursorToolResult::Error(FString::Printf(TEXT("Unknown mode '%s'. Use \"lit\" or \"unlit\"."), *Mode)));
				return;
			}

			FString Error;
			FString View = Args.Str(TEXT("view")).ToLower();
			const FString CameraLabel = Args.Str(TEXT("camera"));
			FVector Location;
			const bool bHasLocation = Args.Vec(TEXT("location"), Location);
			bool bFrame = false;
			bool bAxisView = false;

			if (!CameraLabel.IsEmpty())
			{
				AActor* Actor = CursorToolHelpers::FindActor(CameraLabel, Error);
				if (!Actor)
				{
					Done(FCursorToolResult::Error(Error));
					return;
				}
				const UCameraComponent* Camera = Actor->FindComponentByClass<UCameraComponent>();
				if (!Camera)
				{
					Done(FCursorToolResult::Error(FString::Printf(TEXT("'%s' (%s) has no camera component. Give a camera actor, or use 'location' with 'rotation' or 'look_at'."),
						*CameraLabel, *Actor->GetClass()->GetName())));
					return;
				}
				FMinimalViewInfo Info;
				const_cast<UCameraComponent*>(Camera)->GetCameraView(0.0f, Info);
				Shot.Location = Info.Location;
				Shot.Rotation = Info.Rotation;
				Shot.Fov = Info.FOV;
				Shot.bOrthographic = Info.ProjectionMode == ECameraProjectionMode::Orthographic;
				Shot.OrthoWidth = Info.OrthoWidth;
				Shot.PostProcess = Info.PostProcessSettings;
				if (Info.bConstrainAspectRatio && Info.AspectRatio > 0.0f && !Args.Has(TEXT("height")))
				{
					Shot.Height = FMath::Clamp(FMath::RoundToInt32(Shot.Width / Info.AspectRatio), MinEdge, CursorImages::MaxLongEdge);
				}
				Shot.ViewName = FString::Printf(TEXT("camera %s"), *CameraLabel);
			}
			else if (bHasLocation)
			{
				FRotator Rotation;
				FVector LookAt;
				if (Args.Rot(TEXT("rotation"), Rotation))
				{
					Shot.Rotation = Rotation;
				}
				else if (Args.Vec(TEXT("look_at"), LookAt))
				{
					if (LookAt.Equals(Location, 0.1))
					{
						Done(FCursorToolResult::Error(TEXT("'look_at' is the same point as 'location', so the camera has no direction to face.")));
						return;
					}
					Shot.Rotation = (LookAt - Location).Rotation();
				}
				else
				{
					Done(FCursorToolResult::Error(TEXT("'location' needs 'rotation' [pitch, yaw, roll] or 'look_at' [x, y, z] to say which way the camera faces.")));
					return;
				}
				Shot.Location = Location;
				Shot.Fov = 70.0f;
				Shot.ViewName = TEXT("placed camera");
			}
			else if (View.IsEmpty() || View == TEXT("current"))
			{
				const APlayerController* Player = GEditor->PlayWorld ? GEditor->PlayWorld->GetFirstPlayerController() : nullptr;
				if (Player && Player->PlayerCameraManager)
				{
					const FMinimalViewInfo& Info = Player->PlayerCameraManager->GetCameraCacheView();
					Shot.Location = Info.Location;
					Shot.Rotation = Info.Rotation;
					Shot.Fov = Info.FOV;
					Shot.bOrthographic = Info.ProjectionMode == ECameraProjectionMode::Orthographic;
					Shot.OrthoWidth = Info.OrthoWidth;
					Shot.PostProcess = Info.PostProcessSettings;
					Shot.ViewName = TEXT("player");
				}
				else if (ViewportCamera(Shot))
				{
					Shot.ViewName = TEXT("current");
				}
				else
				{
					Done(FCursorToolResult::Error(TEXT("There is no perspective level viewport to take the current view from. Ask for a view such as \"top\", \"right\" or \"iso\" instead.")));
					return;
				}
			}
			else
			{
				bFrame = true;
				bAxisView = View != TEXT("iso");
				if (View == TEXT("top")) { Shot.Rotation = FRotator(-90.0, 0.0, 0.0); }
				else if (View == TEXT("front")) { Shot.Rotation = FRotator(0.0, 180.0, 0.0); }
				else if (View == TEXT("back")) { Shot.Rotation = FRotator(0.0, 0.0, 0.0); }
				else if (View == TEXT("right")) { Shot.Rotation = FRotator(0.0, -90.0, 0.0); }
				else if (View == TEXT("left")) { Shot.Rotation = FRotator(0.0, 90.0, 0.0); }
				else if (View == TEXT("iso")) { Shot.Rotation = FRotator(-35.0, -135.0, 0.0); }
				else
				{
					Done(FCursorToolResult::Error(FString::Printf(TEXT("Unknown view '%s'. Use current, top, front, back, right, left or iso."), *View)));
					return;
				}
				Shot.Fov = 70.0f;
				Shot.bOrthographic = bAxisView;
				Shot.ViewName = View;
			}

			const FString Projection = Args.Str(TEXT("projection")).ToLower();
			if (Projection == TEXT("orthographic") || Projection == TEXT("perspective"))
			{
				const bool bWanted = Projection == TEXT("orthographic");
				if (bWanted && !Shot.bOrthographic && !bFrame)
				{
					Done(FCursorToolResult::Error(TEXT("An orthographic picture needs something to size it by: use a view such as \"top\" or \"right\" (optionally with 'frame'), or a camera actor that is itself orthographic.")));
					return;
				}
				Shot.bOrthographic = bWanted;
			}
			else if (!Projection.IsEmpty())
			{
				Done(FCursorToolResult::Error(FString::Printf(TEXT("Unknown projection '%s'. Use \"perspective\" or \"orthographic\"."), *Projection)));
				return;
			}
			if (Args.Has(TEXT("fov")))
			{
				Shot.Fov = static_cast<float>(Args.Num(TEXT("fov"), Shot.Fov));
			}
			Shot.Fov = FMath::Clamp(Shot.Fov, 5.0f, 150.0f);

			const TSharedRef<FJsonObject>& Info = Run->Info;
			if (bFrame)
			{
				FBox Box(ForceInit);
				int32 NumFramed = 0;
				const TArray<FString> Labels = Args.StrArray(TEXT("frame"));
				for (const FString& Label : Labels)
				{
					const AActor* Actor = CursorToolHelpers::FindActor(Label, Error);
					if (!Actor)
					{
						Done(FCursorToolResult::Error(Error));
						return;
					}
					FBox ActorBox = VisibleBounds(Actor);
					if (!ActorBox.IsValid)
					{
						// Lights, volumes, empty actors: nothing to see, but still a place to point the camera at.
						ActorBox = FBox::BuildAABB(Actor->GetActorLocation(), FVector(100.0));
					}
					Box += ActorBox;
					++NumFramed;
				}
				if (Labels.Num() == 0)
				{
					bool bTerrainLeftOut = false;
					Box = LevelBounds(World, NumFramed, bTerrainLeftOut);
					if (bTerrainLeftOut)
					{
						Run->Notes.Add(MakeShared<FJsonValueString>(TEXT("The picture is framed on what is placed in the level, not on the whole landscape under it.")));
					}
				}
				if (!Box.IsValid)
				{
					Done(FCursorToolResult::Error(TEXT("The level has nothing visible to frame. Place something first, or give 'location' with 'look_at'.")));
					return;
				}
				FrameBox(Box, Shot);

				TSharedRef<FJsonObject> Framed = MakeShared<FJsonObject>();
				Framed->SetNumberField(TEXT("actors"), NumFramed);
				Framed->SetField(TEXT("center"), VectorJson(Box.GetCenter()));
				Framed->SetField(TEXT("size"), VectorJson(Box.GetSize()));
				Info->SetObjectField(TEXT("framed"), Framed);
			}
			else if (Args.StrArray(TEXT("frame")).Num() > 0)
			{
				Run->Notes.Add(MakeShared<FJsonValueString>(TEXT("'frame' was ignored: it applies to the views top, front, back, right, left and iso, not to a camera that is already placed.")));
			}

			if (!FApp::CanEverRender())
			{
				Done(FCursorToolResult::Error(TEXT("This editor is running without a renderer (-nullrhi or a commandlet), so there is nothing to take a picture of.")));
				return;
			}

			Info->SetStringField(TEXT("view"), Shot.ViewName);
			Info->SetStringField(TEXT("world"), GEditor->PlayWorld ? TEXT("the running game") : TEXT("the level in the editor"));
			Info->SetNumberField(TEXT("width"), Shot.Width);
			Info->SetNumberField(TEXT("height"), Shot.Height);
			Info->SetStringField(TEXT("mode"), Shot.bUnlit ? TEXT("unlit") : TEXT("lit"));
			TSharedRef<FJsonObject> Camera = MakeShared<FJsonObject>();
			Camera->SetField(TEXT("location"), VectorJson(Shot.Location));
			const FRotator Shown = Shot.Rotation.GetNormalized();
			Camera->SetField(TEXT("rotation_pitch_yaw_roll"), VectorJson(FVector(Shown.Pitch, Shown.Yaw, Shown.Roll)));
			if (Shot.bOrthographic)
			{
				Camera->SetStringField(TEXT("projection"), TEXT("orthographic"));
				Camera->SetField(TEXT("picture_width_cm"), Tenths(Shot.OrthoWidth));
				Camera->SetField(TEXT("picture_height_cm"), Tenths(Shot.OrthoWidth * Shot.Height / Shot.Width));
			}
			else
			{
				Camera->SetStringField(TEXT("projection"), TEXT("perspective"));
				Camera->SetField(TEXT("fov"), Tenths(Shot.Fov));
			}
			const FRotationMatrix Axes(Shot.Rotation);
			const FString RightAxis = AxisName(Axes.GetUnitAxis(EAxis::Y));
			const FString UpAxis = AxisName(Axes.GetUnitAxis(EAxis::Z));
			if (!RightAxis.IsEmpty() && !UpAxis.IsEmpty())
			{
				// So a position read off the picture can be turned back into world coordinates.
				Camera->SetStringField(TEXT("picture_right_is"), RightAxis);
				Camera->SetStringField(TEXT("picture_up_is"), UpAxis);
			}
			Info->SetObjectField(TEXT("camera"), Camera);

			Run->World = World;
			Run->Done = MoveTemp(Done);
			Run->Begin();
		})
		.Register();
}

void CursorToolPacks::UnregisterViewPack()
{
	FCursorToolRegistry::Get().UnregisterPack(ViewPack);
}
