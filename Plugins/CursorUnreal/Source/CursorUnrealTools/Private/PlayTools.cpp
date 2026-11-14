#include "CursorToolPacks.h"

#include "Camera/PlayerCameraManager.h"
#include "Containers/Ticker.h"
#include "CursorJson.h"
#include "CursorLogCapture.h"
#include "CursorPackSupport.h"
#include "CursorTool.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PawnMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/PlatformTime.h"
#include "IAssetViewport.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "LevelEditor.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "PlayInEditorDataTypes.h"

/*
 * Play pack. One tool, play_test: start Play In Editor the way the Play button does, hold keys and push sticks for
 * the player to a script, measure what the player's pawn did, stop, and report. It exists because an agent that
 * cannot play its own level reports that the controls work without knowing; this gives it the numbers.
 *
 * It is real Play In Editor rather than a private copy of the level: a copy would have to re-create what Play does
 * with the game mode, the Player Start, references between actors and streamed parts of the level, and a test that
 * passes in a copy and fails when the user presses Play is the failure this tool is here to end.
 */

namespace
{
	const TCHAR* PlayPack = TEXT("play");

	/** A number to one decimal place, written as 507 or 90.1 rather than 90.100000000000001. */
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

	struct FPlayStep
	{
		TArray<FKey> Keys;
		TArray<TPair<FKey, float>> Axes;
		float Seconds = 1.0f;
	};

	/** One running play test. Lives from the tool call until Play has ended and the result has been handed back. */
	class FPlayTestRun : public TSharedFromThis<FPlayTestRun>
	{
	public:
		static constexpr float FrameSeconds = 1.0f / 60.0f;
		static constexpr float MaxTotalSeconds = 60.0f;
		static constexpr int32 MaxSteps = 40;

		TArray<FPlayStep> Steps;
		TArray<TWeakObjectPtr<AActor>> Watched;
		TOptional<FVector> StartLocation;
		TOptional<FRotator> StartRotation;
		float SettleSeconds = 0.5f;
		FCursorToolCompletion Done;

		/** Only one test at a time: there is only one Play session. */
		static TSharedPtr<FPlayTestRun>& Active()
		{
			static TSharedPtr<FPlayTestRun> Run;
			return Run;
		}

		void Begin()
		{
			LogStart = FCursorLogCapture::Get().GetLatestSequence();

			// The same 60 steps a second however fast or slow the editor is drawing, so the numbers repeat from run to run.
			bSavedFixedStep = FApp::UseFixedTimeStep();
			SavedFixedDelta = FApp::GetFixedDeltaTime();
			FApp::SetUseFixedTimeStep(true);
			FApp::SetFixedDeltaTime(FrameSeconds);

			StartedHandle = FEditorDelegates::PostPIEStarted.AddSP(this, &FPlayTestRun::OnPlayStarted);
			EndedHandle = FEditorDelegates::EndPIE.AddSP(this, &FPlayTestRun::OnPlayEnded);
			CancelledHandle = FEditorDelegates::CancelPIE.AddSP(this, &FPlayTestRun::OnPlayCancelled);
			TickStartHandle = FWorldDelegates::OnWorldTickStart.AddSP(this, &FPlayTestRun::OnWorldTickStart);
			PostTickHandle = FWorldDelegates::OnWorldPostActorTick.AddSP(this, &FPlayTestRun::OnWorldPostActorTick);
			WatchdogHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FPlayTestRun::Watchdog), 0.25f);

			FRequestPlaySessionParams Params;
			Params.WorldType = EPlaySessionWorldType::PlayInEditor;
			if (StartLocation.IsSet())
			{
				Params.StartLocation = StartLocation;
				Params.StartRotation = StartRotation.Get(FRotator::ZeroRotator);
			}
			// In the level viewport when there is one, as the Play button does; otherwise the editor opens a game window.
			if (FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor")))
			{
				const TSharedPtr<IAssetViewport> Viewport = LevelEditor->GetFirstActiveViewport();
				if (Viewport.IsValid())
				{
					Params.DestinationSlateViewport = TWeakPtr<IAssetViewport>(Viewport);
				}
			}
			PhaseStartedAt = FPlatformTime::Seconds();
			GEditor->RequestPlaySession(Params);
		}

	private:
		enum class EPhase : uint8 { Starting, Settling, Running, Ending, Finished };

		EPhase Phase = EPhase::Starting;
		double PhaseStartedAt = 0.0;
		int64 LogStart = 0;
		bool bSavedFixedStep = false;
		double SavedFixedDelta = 0.0;

		FDelegateHandle StartedHandle, EndedHandle, CancelledHandle, TickStartHandle, PostTickHandle;
		FTSTicker::FDelegateHandle WatchdogHandle;

		TWeakObjectPtr<UWorld> PlayWorld;
		float SettledFor = 0.0f;
		float WaitedForPlayer = 0.0f;
		int32 StepIndex = INDEX_NONE;
		float StepElapsed = 0.0f;
		TArray<FKey> Held;

		// What the current step has seen so far.
		TWeakObjectPtr<APawn> StepPawn;
		FVector StepFrom = FVector::ZeroVector;
		double StepHighest = 0.0;
		bool bPawnChanged = false;

		TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> StepReports;
		TArray<FString> Notes;
		FString Failure;

		APlayerController* Controller() const
		{
			UWorld* World = PlayWorld.Get();
			return World ? World->GetFirstPlayerController() : nullptr;
		}

		void SetPhase(EPhase NewPhase)
		{
			Phase = NewPhase;
			PhaseStartedAt = FPlatformTime::Seconds();
		}

		void OnPlayStarted(bool bSimulating)
		{
			if (Phase != EPhase::Starting)
			{
				return;
			}
			PlayWorld = GEditor->PlayWorld;
			SetPhase(EPhase::Settling);
		}

		void OnPlayCancelled()
		{
			if (Phase == EPhase::Starting)
			{
				Failure = TEXT("The editor cancelled Play before it started. See editor_log_tail for the reason (a Blueprint that does not compile is the usual one).");
				Complete();
			}
		}

		void OnPlayEnded(bool bSimulating)
		{
			if (Phase == EPhase::Finished)
			{
				return;
			}
			if (Phase != EPhase::Ending)
			{
				Notes.Add(TEXT("Play ended before the test finished: it was stopped by hand, or the game quit. Steps after that did not run."));
			}
			Complete();
		}

		bool Watchdog(float)
		{
			const double Waited = FPlatformTime::Seconds() - PhaseStartedAt;
			if (Phase == EPhase::Starting && (Waited > 120.0 || (Waited > 2.0 && !GEditor->IsPlaySessionInProgress())))
			{
				GEditor->CancelRequestPlaySession();
				Failure = TEXT("Play did not start. See editor_log_tail for the reason.");
				Complete();
				return false;
			}
			if (Phase == EPhase::Ending && Waited > 30.0)
			{
				Notes.Add(TEXT("Play was asked to stop and had not finished stopping after 30 seconds."));
				Complete();
				return false;
			}
			return Phase != EPhase::Finished;
		}

		void SendKey(const FKey& Key, EInputEvent Event, float Amount)
		{
			if (APlayerController* Player = Controller())
			{
				Player->InputKey(FInputKeyEventArgs::CreateSimulated(Key, Event, Amount));
			}
		}

		void OnWorldTickStart(UWorld* World, ELevelTick, float DeltaSeconds)
		{
			if (World != PlayWorld.Get())
			{
				return;
			}
			if (Phase == EPhase::Settling)
			{
				// Give the game mode a moment to spawn the player and the pawn a moment to land.
				if (!Controller())
				{
					WaitedForPlayer += DeltaSeconds;
					if (WaitedForPlayer > 5.0f)
					{
						Notes.Add(TEXT("No player controller appeared within 5 seconds of Play starting, so no input could be sent."));
						EndPlay();
					}
					return;
				}
				SettledFor += DeltaSeconds;
				if (SettledFor >= SettleSeconds)
				{
					DescribePlayer();
					SetPhase(EPhase::Running);
					BeginStep(0);
				}
				return;
			}
			if (Phase == EPhase::Running && Steps.IsValidIndex(StepIndex))
			{
				// A stick or the mouse reports its value every frame; so does this.
				for (const TPair<FKey, float>& Axis : Steps[StepIndex].Axes)
				{
					SendKey(Axis.Key, IE_Axis, Axis.Value);
				}
			}
		}

		void OnWorldPostActorTick(UWorld* World, ELevelTick, float DeltaSeconds)
		{
			if (World != PlayWorld.Get() || Phase != EPhase::Running || !Steps.IsValidIndex(StepIndex))
			{
				return;
			}
			APlayerController* Player = Controller();
			APawn* Pawn = Player ? Player->GetPawn() : nullptr;
			if (Pawn != StepPawn.Get())
			{
				bPawnChanged = true;
				StepPawn = Pawn;
			}
			if (Pawn)
			{
				StepHighest = FMath::Max(StepHighest, Pawn->GetActorLocation().Z);
			}
			StepElapsed += DeltaSeconds;
			if (StepElapsed + KINDA_SMALL_NUMBER >= Steps[StepIndex].Seconds)
			{
				EndStep();
				if (Steps.IsValidIndex(StepIndex + 1))
				{
					BeginStep(StepIndex + 1);
				}
				else
				{
					EndPlay();
				}
			}
		}

		void BeginStep(int32 Index)
		{
			StepIndex = Index;
			StepElapsed = 0.0f;
			bPawnChanged = false;
			const FPlayStep& Step = Steps[Index];

			for (int32 i = Held.Num() - 1; i >= 0; --i)
			{
				if (!Step.Keys.Contains(Held[i]))
				{
					SendKey(Held[i], IE_Released, 0.0f);
					Held.RemoveAt(i);
				}
			}
			for (const FKey& Key : Step.Keys)
			{
				if (!Held.Contains(Key))
				{
					SendKey(Key, IE_Pressed, 1.0f);
					Held.Add(Key);
				}
			}

			APlayerController* Player = Controller();
			StepPawn = Player ? Player->GetPawn() : nullptr;
			StepFrom = StepPawn.IsValid() ? StepPawn->GetActorLocation() : FVector::ZeroVector;
			StepHighest = StepFrom.Z;
		}

		void EndStep()
		{
			const FPlayStep& Step = Steps[StepIndex];
			TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
			TArray<FString> KeyNames;
			for (const FKey& Key : Step.Keys)
			{
				KeyNames.Add(Key.ToString());
			}
			Out->SetArrayField(TEXT("keys"), CursorJson::StringArray(KeyNames));
			if (Step.Axes.Num() > 0)
			{
				TSharedRef<FJsonObject> Axes = MakeShared<FJsonObject>();
				for (const TPair<FKey, float>& Axis : Step.Axes)
				{
					Axes->SetNumberField(Axis.Key.ToString(), Axis.Value);
				}
				Out->SetObjectField(TEXT("axes"), Axes);
			}
			Out->SetNumberField(TEXT("seconds"), Step.Seconds);

			if (const APawn* Pawn = StepPawn.Get())
			{
				const FVector To = Pawn->GetActorLocation();
				Out->SetField(TEXT("from"), VectorJson(StepFrom));
				Out->SetField(TEXT("to"), VectorJson(To));
				Out->SetField(TEXT("moved"), VectorJson(To - StepFrom));
				Out->SetField(TEXT("highest_rise"), Tenths(StepHighest - StepFrom.Z));
				Out->SetNumberField(TEXT("speed_at_end"), FMath::RoundToDouble(Pawn->GetVelocity().Size()));
				if (const UPawnMovementComponent* Movement = Pawn->GetMovementComponent())
				{
					Out->SetBoolField(TEXT("on_ground_at_end"), Movement->IsMovingOnGround());
				}
				Out->SetField(TEXT("facing"), VectorJson(Pawn->GetActorForwardVector()));
			}
			else
			{
				Out->SetStringField(TEXT("pawn"), TEXT("none: the player controls no pawn at the end of this step"));
			}
			if (bPawnChanged)
			{
				Out->SetBoolField(TEXT("pawn_changed"), true);
				Out->SetStringField(TEXT("pawn_changed_note"), TEXT("The player's pawn was replaced or destroyed during this step (it died, respawned, or possessed something else); 'from' is where the new one started."));
			}

			if (Watched.Num() > 0)
			{
				TSharedRef<FJsonObject> Seen = MakeShared<FJsonObject>();
				for (const TWeakObjectPtr<AActor>& EditorActor : Watched)
				{
					AActor* Actor = EditorActor.Get();
					if (!Actor)
					{
						continue;
					}
					const AActor* InPlay = EditorUtilities::GetSimWorldCounterpartActor(Actor);
					if (InPlay)
					{
						Seen->SetField(Actor->GetActorLabel(), VectorJson(InPlay->GetActorLocation()));
					}
					else
					{
						Seen->SetStringField(Actor->GetActorLabel(), TEXT("not in the game: destroyed, or not loaded there"));
					}
				}
				Out->SetObjectField(TEXT("watched"), Seen);
			}
			StepReports.Add(MakeShared<FJsonValueObject>(Out));
		}

		/** Who the player is, read once the game has put them in the level. */
		void DescribePlayer()
		{
			UWorld* World = PlayWorld.Get();
			APlayerController* Player = Controller();
			if (!World || !Player)
			{
				return;
			}
			if (const AGameModeBase* GameMode = World->GetAuthGameMode())
			{
				Report->SetStringField(TEXT("game_mode"), GameMode->GetClass()->GetName());
			}
			Report->SetStringField(TEXT("player_controller"), Player->GetClass()->GetName());
			if (Player->PlayerInput)
			{
				Report->SetStringField(TEXT("player_input"), Player->PlayerInput->GetClass()->GetName());
			}
			if (const APawn* Pawn = Player->GetPawn())
			{
				Report->SetStringField(TEXT("pawn"), Pawn->GetClass()->GetName());
				Report->SetField(TEXT("pawn_start"), VectorJson(Pawn->GetActorLocation()));
			}
			else
			{
				Report->SetStringField(TEXT("pawn"), TEXT("none"));
				Notes.Add(TEXT("The player has no pawn: the game mode's Default Pawn Class is empty, or no Player Start could be used. Keys have nothing to move."));
			}
		}

		void EndPlay()
		{
			for (const FKey& Key : Held)
			{
				SendKey(Key, IE_Released, 0.0f);
			}
			Held.Reset();

			if (const APlayerController* Player = Controller())
			{
				if (const APlayerCameraManager* Camera = Player->PlayerCameraManager)
				{
					TSharedRef<FJsonObject> View = MakeShared<FJsonObject>();
					View->SetField(TEXT("location"), VectorJson(Camera->GetCameraLocation()));
					const FRotator Rotation = Camera->GetCameraRotation();
					View->SetField(TEXT("rotation_pitch_yaw_roll"), VectorJson(FVector(Rotation.Pitch, Rotation.Yaw, Rotation.Roll)));
					if (const APawn* Pawn = Player->GetPawn())
					{
						View->SetField(TEXT("offset_from_pawn"), VectorJson(Camera->GetCameraLocation() - Pawn->GetActorLocation()));
					}
					Report->SetObjectField(TEXT("camera_at_end"), View);
				}
			}
			SetPhase(EPhase::Ending);
			GEditor->RequestEndPlayMap();
		}

		void Complete()
		{
			if (Phase == EPhase::Finished)
			{
				return;
			}
			Phase = EPhase::Finished;

			FEditorDelegates::PostPIEStarted.Remove(StartedHandle);
			FEditorDelegates::EndPIE.Remove(EndedHandle);
			FEditorDelegates::CancelPIE.Remove(CancelledHandle);
			FWorldDelegates::OnWorldTickStart.Remove(TickStartHandle);
			FWorldDelegates::OnWorldPostActorTick.Remove(PostTickHandle);
			FTSTicker::GetCoreTicker().RemoveTicker(WatchdogHandle);
			FApp::SetUseFixedTimeStep(bSavedFixedStep);
			FApp::SetFixedDeltaTime(SavedFixedDelta);

			// What the game said while it ran: Blueprint runtime errors and other warnings, and anything a graph printed.
			TArray<FString> Log;
			for (const FCursorLogLine& Line : FCursorLogCapture::Get().GetLines(LogStart, 400))
			{
				const bool bPrinted = Line.Category == TEXT("LogBlueprintUserMessages");
				if ((bPrinted || Line.Verbosity <= ELogVerbosity::Warning) && Log.Num() < 40)
				{
					Log.Add(FCursorLogCapture::Format(Line));
				}
			}

			FCursorToolResult Result;
			if (!Failure.IsEmpty())
			{
				Result = FCursorToolResult::Error(Failure + (Log.Num() > 0 ? TEXT("\n") + FString::Join(Log, TEXT("\n")) : FString()));
			}
			else
			{
				Report->SetNumberField(TEXT("steps_run"), StepReports.Num());
				Report->SetNumberField(TEXT("steps_asked"), Steps.Num());
				Report->SetArrayField(TEXT("steps"), StepReports);
				if (Log.Num() > 0)
				{
					Report->SetArrayField(TEXT("warnings_and_prints"), CursorJson::StringArray(Log));
				}
				if (Notes.Num() > 0)
				{
					Report->SetArrayField(TEXT("notes"), CursorJson::StringArray(Notes));
				}
				Report->SetStringField(TEXT("how_to_read"), TEXT("Positions are centimetres. 'moved' is where the pawn ended relative to where the step began; "
					"'highest_rise' is how far above its starting height it got during the step. This ran in Play In Editor at 60 steps a second and has now stopped."));
				Result = FCursorToolResult::OkJson(Report);
			}

			// Move out first: the callback may start another test, and dropping the last reference destroys this object.
			FCursorToolCompletion Callback = MoveTemp(Done);
			const TSharedRef<FPlayTestRun> KeepAlive = AsShared();
			Active().Reset();
			if (Callback)
			{
				Callback(Result);
			}
		}
	};

	bool ReadKeys(const FCursorArgs& Step, TArray<FKey>& OutKeys, FString& OutError)
	{
		TArray<FString> Names = Step.StrArray(TEXT("keys"));
		if (Step.Has(TEXT("key")))
		{
			Names.Add(Step.Str(TEXT("key")));
		}
		for (const FString& Name : Names)
		{
			FString KeyName;
			if (!CursorPackSupport::NormaliseKeyName(Name, KeyName, OutError))
			{
				return false;
			}
			const FKey Key{ FName(*KeyName) };
			if (KeyName == TEXT("None"))
			{
				continue;
			}
			if (Key.IsAxis1D() || Key.IsAxis2D() || Key.IsAxis3D())
			{
				OutError = FString::Printf(TEXT("%s is an axis, not a button. Put it under 'axes' with a value, for example {\"axes\":{\"%s\":1.0}}."), *KeyName, *KeyName);
				return false;
			}
			OutKeys.AddUnique(Key);
		}
		return true;
	}
}

void CursorToolPacks::RegisterPlayPack()
{
	FCursorToolBuilder(TEXT("play_test"), PlayPack)
		.Describe(TEXT("Play the open level and report what the player's pawn did. Starts Play In Editor as the Play button does, holds the keys and pushes the sticks "
			"you list, step by step, then stops Play and returns, for each step, where the pawn started and ended, how far it moved, how high it rose, its speed and "
			"whether it was on the ground; plus the game mode, pawn and controller classes in use, where the camera ended, and every warning, Blueprint runtime error "
			"and PrintString the game logged. Use it after building or changing anything a player controls or touches, before saying that it works. "
			"Keys in a step are held for the whole step and released when a later step does not list them, so a tap is a short step (0.1 s) followed by a wait. "
			"The level viewport shows the game while the test runs; the test takes about as long as its steps add up to."))
		.Risk(ECursorToolRisk::ReadOnly)
		.Schema(FCursorSchema()
			.ObjArray(TEXT("steps"), TEXT("What to do, in order. Each step: {\"keys\": [\"D\", \"LeftShift\"], \"axes\": {\"Gamepad_LeftX\": 1.0, \"MouseX\": 5}, \"seconds\": 1.0}. "
				"'keys' are held for the step; 'axes' are sent every frame of it; 'seconds' is how long it lasts (default 1). A step with neither is a wait. "
				"Default: one 1-second wait, which is enough to see where the player lands and whether anything logs an error."))
			.Vec3(TEXT("start"), TEXT("Start the player here [x, y, z] instead of at the level's Player Start (the editor's 'Play From Here'). Use it to test one part of a level."))
			.Vec3(TEXT("start_rotation"), TEXT("With 'start': which way the player faces [pitch, yaw, roll]."))
			.Num(TEXT("settle_seconds"), TEXT("Time to let the player land before the first step. Default 0.5."))
			.StrArray(TEXT("watch"), TEXT("Labels of actors in the level whose position to report at the end of every step: a moving platform, a door, a crate.")))
		.RunAsync([](const FCursorArgs& Args, const FCursorToolContext&, FCursorToolCompletion Done)
		{
			if (!GEditor || !CursorToolHelpers::GetEditorWorld())
			{
				Done(FCursorToolResult::Error(TEXT("No level is open in the editor.")));
				return;
			}
			if (FPlayTestRun::Active().IsValid())
			{
				Done(FCursorToolResult::Error(TEXT("A play test is already running. Wait for it to finish.")));
				return;
			}
			if (GEditor->IsPlaySessionInProgress())
			{
				Done(FCursorToolResult::Error(TEXT("Play is already running in the editor. Stop it (Esc, or the Stop button) and call play_test again; "
					"the test starts and stops Play itself so that every run begins from the same place.")));
				return;
			}

			const TSharedRef<FPlayTestRun> Run = MakeShared<FPlayTestRun>();
			FString Error;
			float Total = 0.0f;
			if (const TArray<TSharedPtr<FJsonValue>>* Steps = Args.Arr(TEXT("steps")))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Steps)
				{
					const TSharedPtr<FJsonObject>* Object = nullptr;
					if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object)
					{
						Done(FCursorToolResult::Error(TEXT("Each step must be an object such as {\"keys\": [\"D\"], \"seconds\": 1.0}.")));
						return;
					}
					const FCursorArgs StepArgs(Object->ToSharedRef());
					FPlayStep Step;
					if (!ReadKeys(StepArgs, Step.Keys, Error))
					{
						Done(FCursorToolResult::Error(Error));
						return;
					}
					if (const TSharedPtr<FJsonObject> Axes = StepArgs.Obj(TEXT("axes")))
					{
						for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Axes->Values)
						{
							FString KeyName;
							double Amount = 0.0;
							if (!CursorPackSupport::NormaliseKeyName(Pair.Key, KeyName, Error))
							{
								Done(FCursorToolResult::Error(Error));
								return;
							}
							const FKey Key{ FName(*KeyName) };
							if (!Key.IsAxis1D() || !Pair.Value.IsValid() || !Pair.Value->TryGetNumber(Amount))
							{
								Done(FCursorToolResult::Error(FString::Printf(TEXT("'axes' takes one-dimensional axes with a number each, such as Gamepad_LeftX, Gamepad_LeftY, "
									"Gamepad_RightX, Gamepad_RightY, Gamepad_LeftTriggerAxis, MouseX, MouseY, MouseWheelAxis; '%s' is not one. Buttons and keys go under 'keys'."), *Pair.Key)));
								return;
							}
							Step.Axes.Emplace(Key, static_cast<float>(Amount));
						}
					}
					Step.Seconds = FMath::Max(static_cast<float>(StepArgs.Num(TEXT("seconds"), 1.0)), FPlayTestRun::FrameSeconds);
					Total += Step.Seconds;
					Run->Steps.Add(MoveTemp(Step));
				}
			}
			if (Run->Steps.Num() == 0)
			{
				Run->Steps.AddDefaulted();
				Total = 1.0f;
			}
			if (Run->Steps.Num() > FPlayTestRun::MaxSteps || Total > FPlayTestRun::MaxTotalSeconds)
			{
				Done(FCursorToolResult::Error(FString::Printf(TEXT("A play test can have at most %d steps adding up to %.0f seconds; this one has %d steps and %.1f seconds. Split it into separate tests."),
					FPlayTestRun::MaxSteps, FPlayTestRun::MaxTotalSeconds, Run->Steps.Num(), Total)));
				return;
			}

			for (const FString& Label : Args.StrArray(TEXT("watch")))
			{
				AActor* Actor = CursorToolHelpers::FindActor(Label, Error);
				if (!Actor)
				{
					Done(FCursorToolResult::Error(Error));
					return;
				}
				Run->Watched.Add(Actor);
			}

			FVector Start;
			if (Args.Vec(TEXT("start"), Start))
			{
				Run->StartLocation = Start;
				FRotator Rotation;
				if (Args.Rot(TEXT("start_rotation"), Rotation))
				{
					Run->StartRotation = Rotation;
				}
			}
			Run->SettleSeconds = FMath::Clamp(static_cast<float>(Args.Num(TEXT("settle_seconds"), 0.5)), 0.0f, 10.0f);
			Run->Done = MoveTemp(Done);
			FPlayTestRun::Active() = Run;
			Run->Begin();
		})
		.Register();
}

void CursorToolPacks::UnregisterPlayPack()
{
	FCursorToolRegistry::Get().UnregisterPack(PlayPack);
}
