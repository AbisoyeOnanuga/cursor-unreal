#if WITH_DEV_AUTOMATION_TESTS

// Tests for the unreal-level-design skill (Plugins/CursorUnreal/Skills/unreal-level-design).
//
// The skill gives the agent numbers (how high a character jumps, how big a cube is) and recipes
// (the exact tool calls that build a moving platform, a trigger, a kill zone...). These tests
// run those recipes through the same tools the agent uses and then play the results in a small
// game world, so a recipe that stops working, or a number that an engine version changes, fails
// here instead of in front of a user.

#include "CursorPackTestSupport.h"
#include "CursorSkillLibrary.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "CursorUnrealLog.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InterpToMovementComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "GameFramework/WorldSettings.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Regex.h"
#include "Materials/MaterialInterface.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/EditorActorSubsystem.h"
#include "UObject/UObjectGlobals.h"

using namespace CursorPackTest;

namespace CursorLevelDesignTest
{
	const TCHAR* CubePath = TEXT("/Engine/BasicShapes/Cube");

	/** Runs a tool the way the agent does. A tool error is a test error, with the call that caused it. */
	FCursorToolResult Tool(FAutomationTestBase& Test, const TCHAR* Name, const FString& ArgsJson)
	{
		const FCursorToolResult Result = CursorTest::Call(Name, *ArgsJson);
		if (Result.bIsError)
		{
			Test.AddError(FString::Printf(TEXT("%s %s failed: %s"), Name, *ArgsJson, *Result.Text));
		}
		return Result;
	}

	void Note(const FString& Text)
	{
		UE_LOG(LogCursorUnreal, Display, TEXT("[LevelDesign] %s"), *Text);
	}

	/** Where a Blueprint keeps the defaults of a component it added: <path>.<Name>_C:<Component>_GEN_VARIABLE. */
	FString TemplatePath(const FString& BlueprintPath, const TCHAR* Component)
	{
		return FString::Printf(TEXT("%s.%s_C:%s_GEN_VARIABLE"), *BlueprintPath, *FPackageName::GetShortName(BlueprintPath), Component);
	}

	bool Compile(FAutomationTestBase& Test, const FString& Blueprint)
	{
		const FCursorToolResult Result = Tool(Test, TEXT("blueprint_compile"), Args(TEXT("{\"blueprint\":\"{0}\"}"), Blueprint));
		const bool bClean = !Result.bIsError && Result.Structured.IsValid() && Result.Structured->GetStringField(TEXT("status")) == TEXT("up_to_date");
		Test.TestTrue(FString::Printf(TEXT("%s compiles with no errors or warnings: %s"), *Blueprint, *Result.Text), bClean);
		return bClean;
	}

	FString AddNode(FAutomationTestBase& Test, const FString& Blueprint, const TCHAR* Fields)
	{
		const FCursorToolResult Result = Tool(Test, TEXT("blueprint_add_node"), FString::Printf(TEXT("{\"blueprint\":\"%s\",%s}"), *Blueprint, Fields));
		return Result.Structured.IsValid() ? Result.Structured->GetStringField(TEXT("id")) : FString();
	}

	void Connect(FAutomationTestBase& Test, const FString& Blueprint, const FString& From, const TCHAR* FromPin, const FString& To, const TCHAR* ToPin)
	{
		Tool(Test, TEXT("blueprint_connect_pins"), FString::Printf(TEXT("{\"blueprint\":\"%s\",\"from_node\":\"%s\",\"from_pin\":\"%s\",\"to_node\":\"%s\",\"to_pin\":\"%s\"}"),
			*Blueprint, *From, FromPin, *To, ToPin));
	}

	void PinDefault(FAutomationTestBase& Test, const FString& Blueprint, const FString& Node, const TCHAR* Pin, const TCHAR* Value)
	{
		Tool(Test, TEXT("blueprint_set_pin_default"), FString::Printf(TEXT("{\"blueprint\":\"%s\",\"node\":\"%s\",\"pin\":\"%s\",\"value\":\"%s\"}"), *Blueprint, *Node, Pin, Value));
	}

	/** An Actor Blueprint whose root is a box that overlaps things: the base of every trigger in the kit. */
	void CreateTriggerBlueprint(FAutomationTestBase& Test, const FString& Path, const TCHAR* ColorJson)
	{
		Tool(Test, TEXT("blueprint_create"), Args(TEXT("{\"path\":\"{0}\",\"parent_class\":\"Actor\"}"), Path));
		Tool(Test, TEXT("blueprint_add_component"), Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"BoxComponent\",\"name\":\"Trigger\"}"), Path));
		Compile(Test, Path);
		// 50 cm half-size: at scale 1 the trigger is a 100 cm cube, the same convention as the blockout cube.
		Tool(Test, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}\",\"properties\":{\"BoxExtent\":{\"X\":50,\"Y\":50,\"Z\":50},\"bHiddenInGame\":false,\"ShapeColor\":{1}}}"),
			TemplatePath(Path, TEXT("Trigger")), ColorJson));
	}

	void AddActorVariable(FAutomationTestBase& Test, const FString& Path, const TCHAR* Name)
	{
		Tool(Test, TEXT("blueprint_add_variable"), Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"{1}\",\"type\":\"object:Actor\",\"instance_editable\":true}"), Path, Name));
	}

	struct FKit
	{
		FString MovingPlatform;
		FString Trigger;
		FString KillZone;
		FString JumpPad;
		FString Goal;
	};

	/** Builds the five Blueprints exactly as Skills/unreal-level-design/gameplay-kit.md tells the agent to. */
	FKit BuildKit(FAutomationTestBase& Test, FScopedTestAssets& Assets)
	{
		FKit Kit;

		// --- BP_MovingPlatform: a cube carried along a path by an InterpToMovement component. No graph.
		{
			const FString P = Kit.MovingPlatform = Assets.Add(TEXT("BP_MovingPlatform"));
			Tool(Test, TEXT("blueprint_create"), Args(TEXT("{\"path\":\"{0}\",\"parent_class\":\"Actor\"}"), P));
			Tool(Test, TEXT("blueprint_add_component"), Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"StaticMeshComponent\",\"name\":\"Mesh\"}"), P));
			Tool(Test, TEXT("blueprint_add_component"), Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"InterpToMovementComponent\",\"name\":\"Mover\"}"), P));
			Compile(Test, P);
			Tool(Test, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}\",\"properties\":{\"StaticMesh\":\"{1}\"}}"), TemplatePath(P, TEXT("Mesh")), CubePath));
			Tool(Test, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}\",\"properties\":{")
				TEXT("\"ControlPoints\":[{\"PositionControlPoint\":{\"X\":0,\"Y\":0,\"Z\":0},\"bPositionIsRelative\":true},{\"PositionControlPoint\":{\"X\":0,\"Y\":0,\"Z\":300},\"bPositionIsRelative\":true}],")
				TEXT("\"Duration\":3,\"BehaviourType\":\"PingPong\",\"bSweep\":false}}"), TemplatePath(P, TEXT("Mover"))));
			Compile(Test, P);
		}

		// --- BP_Trigger: when something enters the box, start the mover on the actor it points at.
		{
			const FString P = Kit.Trigger = Assets.Add(TEXT("BP_Trigger"));
			CreateTriggerBlueprint(Test, P, TEXT("{\"R\":255,\"G\":210,\"B\":0,\"A\":255}"));
			AddActorVariable(Test, P, TEXT("Target"));
			AddActorVariable(Test, P, TEXT("RequiredActor"));
			Compile(Test, P);

			const FString Event = AddNode(Test, P, TEXT("\"kind\":\"event\",\"name\":\"ActorBeginOverlap\""));
			const FString Required = AddNode(Test, P, TEXT("\"kind\":\"variable_get\",\"name\":\"RequiredActor\""));
			const FString Target = AddNode(Test, P, TEXT("\"kind\":\"variable_get\",\"name\":\"Target\""));
			const FString IsAnyone = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"EqualEqual_ObjectObject\",\"class\":\"KismetMathLibrary\""));
			const FString IsRequired = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"EqualEqual_ObjectObject\",\"class\":\"KismetMathLibrary\""));
			const FString Either = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"BooleanOR\",\"class\":\"KismetMathLibrary\""));
			const FString Branch = AddNode(Test, P, TEXT("\"kind\":\"branch\""));
			const FString GetMover = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"GetComponentByClass\",\"class\":\"Actor\""));
			const FString Activate = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"Activate\",\"class\":\"ActorComponent\""));

			Connect(Test, P, Required, TEXT("RequiredActor"), IsAnyone, TEXT("A")); // B stays empty: "is RequiredActor unset?"
			Connect(Test, P, Event, TEXT("OtherActor"), IsRequired, TEXT("A"));
			Connect(Test, P, Required, TEXT("RequiredActor"), IsRequired, TEXT("B"));
			Connect(Test, P, IsAnyone, TEXT("ReturnValue"), Either, TEXT("A"));
			Connect(Test, P, IsRequired, TEXT("ReturnValue"), Either, TEXT("B"));
			Connect(Test, P, Either, TEXT("ReturnValue"), Branch, TEXT("Condition"));
			Connect(Test, P, Event, TEXT("then"), Branch, TEXT("execute"));
			Connect(Test, P, Target, TEXT("Target"), GetMover, TEXT("self"));
			PinDefault(Test, P, GetMover, TEXT("ComponentClass"), TEXT("InterpToMovementComponent"));
			Connect(Test, P, GetMover, TEXT("ReturnValue"), Activate, TEXT("self"));
			Connect(Test, P, Branch, TEXT("then"), Activate, TEXT("execute"));
			Compile(Test, P);
		}

		// --- BP_KillZone: whatever enters is put back at the actor RespawnPoint refers to.
		{
			const FString P = Kit.KillZone = Assets.Add(TEXT("BP_KillZone"));
			CreateTriggerBlueprint(Test, P, TEXT("{\"R\":255,\"G\":0,\"B\":0,\"A\":255}"));
			AddActorVariable(Test, P, TEXT("RespawnPoint"));
			Compile(Test, P);

			const FString Event = AddNode(Test, P, TEXT("\"kind\":\"event\",\"name\":\"ActorBeginOverlap\""));
			const FString Respawn = AddNode(Test, P, TEXT("\"kind\":\"variable_get\",\"name\":\"RespawnPoint\""));
			const FString GetLocation = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"GetActorLocation\",\"class\":\"Actor\""));
			const FString SetLocation = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"SetActorLocation\",\"class\":\"Actor\""));
			Connect(Test, P, Respawn, TEXT("RespawnPoint"), GetLocation, TEXT("self"));
			Connect(Test, P, Event, TEXT("OtherActor"), SetLocation, TEXT("self"));
			Connect(Test, P, GetLocation, TEXT("ReturnValue"), SetLocation, TEXT("NewLocation"));
			Connect(Test, P, Event, TEXT("then"), SetLocation, TEXT("execute"));
			Compile(Test, P);
		}

		// --- BP_JumpPad: launches the player character upwards.
		{
			const FString P = Kit.JumpPad = Assets.Add(TEXT("BP_JumpPad"));
			CreateTriggerBlueprint(Test, P, TEXT("{\"R\":0,\"G\":200,\"B\":255,\"A\":255}"));
			Tool(Test, TEXT("blueprint_add_variable"), Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"LaunchVelocity\",\"type\":\"vector\",\"default_value\":\"(X=0,Y=0,Z=1000)\",\"instance_editable\":true}"), P));
			Compile(Test, P);

			const FString Event = AddNode(Test, P, TEXT("\"kind\":\"event\",\"name\":\"ActorBeginOverlap\""));
			const FString Player = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"GetPlayerCharacter\",\"class\":\"GameplayStatics\""));
			const FString Velocity = AddNode(Test, P, TEXT("\"kind\":\"variable_get\",\"name\":\"LaunchVelocity\""));
			const FString Launch = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"LaunchCharacter\",\"class\":\"Character\""));
			const FString IsPlayer = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"EqualEqual_ObjectObject\",\"class\":\"KismetMathLibrary\""));
			const FString Branch = AddNode(Test, P, TEXT("\"kind\":\"branch\""));
			// Only when the player is what touched it: a crate landing on the pad must not launch a player standing elsewhere.
			Connect(Test, P, Event, TEXT("OtherActor"), IsPlayer, TEXT("A"));
			Connect(Test, P, Player, TEXT("ReturnValue"), IsPlayer, TEXT("B"));
			Connect(Test, P, IsPlayer, TEXT("ReturnValue"), Branch, TEXT("Condition"));
			Connect(Test, P, Event, TEXT("then"), Branch, TEXT("execute"));
			Connect(Test, P, Player, TEXT("ReturnValue"), Launch, TEXT("self"));
			Connect(Test, P, Velocity, TEXT("LaunchVelocity"), Launch, TEXT("LaunchVelocity"));
			// Both overrides on: the launch replaces the player's velocity, so where they land does not depend on how they arrived.
			PinDefault(Test, P, Launch, TEXT("bXYOverride"), TEXT("true"));
			PinDefault(Test, P, Launch, TEXT("bZOverride"), TEXT("true"));
			Connect(Test, P, Branch, TEXT("then"), Launch, TEXT("execute"));
			Compile(Test, P);
		}

		// --- BP_Goal: says so on screen when reached.
		{
			const FString P = Kit.Goal = Assets.Add(TEXT("BP_Goal"));
			CreateTriggerBlueprint(Test, P, TEXT("{\"R\":0,\"G\":255,\"B\":0,\"A\":255}"));
			Compile(Test, P);

			const FString Event = AddNode(Test, P, TEXT("\"kind\":\"event\",\"name\":\"ActorBeginOverlap\""));
			const FString Print = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"PrintString\",\"class\":\"KismetSystemLibrary\""));
			PinDefault(Test, P, Print, TEXT("InString"), TEXT("Goal reached"));
			PinDefault(Test, P, Print, TEXT("Duration"), TEXT("5.0"));
			Connect(Test, P, Event, TEXT("then"), Print, TEXT("execute"));
			Compile(Test, P);
		}
		return Kit;
	}

	UClass* GeneratedClass(const FString& BlueprintPath)
	{
		FString Error;
		const UBlueprint* Blueprint = Cast<UBlueprint>(CursorToolHelpers::LoadAsset(BlueprintPath, Error));
		return Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
	}

	/** Removes the actors a test put in the open level. */
	struct FScopedLevelActors
	{
		TArray<FString> Labels;

		FString Add(const TCHAR* Prefix) { return Labels.Add_GetRef(UniqueName(Prefix)); }

		~FScopedLevelActors()
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

	/** A game world with physics that is ticked by hand: enough to play a few seconds of a level without Play In Editor. */
	struct FPlayWorld
	{
		UWorld* World = nullptr;
		uint64 SavedFrameCounter = 0;

		FPlayWorld()
		{
			SavedFrameCounter = GFrameCounter;
			World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("CursorLevelDesignPlay"));
			FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
			Context.SetCurrentWorld(World);
			World->InitializeActorsForPlay(FURL());
		}

		~FPlayWorld()
		{
			World->EndPlay(EEndPlayReason::Quit);
			GEngine->DestroyWorldContext(World);
			World->DestroyWorld(false);
			GFrameCounter = SavedFrameCounter;
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}

		/** Like the level finishing loading: every actor placed so far gets BeginPlay. */
		void BeginPlay()
		{
			World->GetWorldSettings()->NotifyBeginPlay();
		}

		void Tick(float Seconds, TFunctionRef<void()> BeforeEachFrame = [] {})
		{
			const float Step = 1.0f / 60.0f;
			for (float Elapsed = 0.0f; Elapsed < Seconds; Elapsed += Step)
			{
				BeforeEachFrame();
				World->Tick(LEVELTICK_All, Step);
				++GFrameCounter;
			}
		}

		/** A blockout cube: Size in cm, Top is the height of its walkable surface. */
		AStaticMeshActor* Cube(const FVector& TopCentre, const FVector& Size, bool bPhysics = false)
		{
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(FVector(TopCentre.X, TopCentre.Y, TopCentre.Z - Size.Z * 0.5), FRotator::ZeroRotator);
			UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
			Mesh->SetMobility(EComponentMobility::Movable);
			Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, CubePath));
			Actor->SetActorScale3D(Size / 100.0);
			if (bPhysics)
			{
				Mesh->SetGenerateOverlapEvents(true);
				Mesh->SetSimulatePhysics(true);
			}
			return Actor;
		}

		/** The engine's default Character, possessed by a player controller, standing with its feet at FeetLocation. */
		ACharacter* Player(const FVector& FeetLocation)
		{
			ACharacter* Character = World->SpawnActor<ACharacter>(FeetLocation + FVector(0, 0, 90), FRotator::ZeroRotator);
			APlayerController* Controller = World->SpawnActor<APlayerController>();
			Controller->Possess(Character);
			return Character;
		}

		static double FeetZ(const ACharacter* Character)
		{
			return Character->GetActorLocation().Z - Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		}
	};

	void SetActorReference(AActor* Owner, const TCHAR* Variable, AActor* Value)
	{
		if (const FObjectProperty* Property = FindFProperty<FObjectProperty>(Owner->GetClass(), Variable))
		{
			Property->SetObjectPropertyValue_InContainer(Owner, Value);
		}
	}
}

using namespace CursorLevelDesignTest;

// The skill's text itself: its reference files are there, and every tool it names exists.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorLevelDesignSkillTest, "CursorUnreal.LevelDesign.Skill", CursorTest::Flags)
bool FCursorLevelDesignSkillTest::RunTest(const FString&)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("CursorUnreal"));
	if (!TestTrue(TEXT("plugin found"), Plugin.IsValid()))
	{
		return false;
	}
	FCursorSkillLibrary::FLayerDirs Dirs;
	Dirs.BuiltIn = Plugin->GetBaseDir() / TEXT("Skills");
	const FCursorSkillLibrary Library(Dirs);

	const TCHAR* Skill = TEXT("unreal-level-design");
	FString Body, Error;
	if (!TestTrue(TEXT("the skill loads: ") + Error, Library.Load(Skill, Body, Error)))
	{
		return false;
	}

	TArray<FString> Texts = { Body };
	for (const TCHAR* File : { TEXT("platformer.md"), TEXT("gameplay-kit.md"), TEXT("player.md") })
	{
		TestTrue(FString::Printf(TEXT("SKILL.md tells the agent about %s"), File), Body.Contains(File));
		FString Text;
		if (TestTrue(FString::Printf(TEXT("%s is readable through load_skill's 'file': %s"), File, *Error), Library.ReadFile(Skill, File, Text, Error)))
		{
			TestTrue(FString::Printf(TEXT("%s is not empty"), File), Text.Len() > 500);
			Texts.Add(MoveTemp(Text));
		}
	}

	// Anything written as `prefix_name` with a tool prefix must be a registered tool: a skill that names a tool
	// that does not exist sends the agent looking for it.
	const TSet<FString> ToolPrefixes = { TEXT("actor"), TEXT("asset"), TEXT("blueprint"), TEXT("object"), TEXT("editor"), TEXT("selection"),
		TEXT("material"), TEXT("console"), TEXT("python"), TEXT("load"), TEXT("list") };
	TSet<FString> Named;
	const FRegexPattern Pattern(TEXT("`([a-z]+(?:_[a-z]+)+)`"));
	for (const FString& Text : Texts)
	{
		FRegexMatcher Matcher(Pattern, Text);
		while (Matcher.FindNext())
		{
			const FString Token = Matcher.GetCaptureGroup(1);
			FString Prefix, Rest;
			if (Token.Split(TEXT("_"), &Prefix, &Rest) && ToolPrefixes.Contains(Prefix))
			{
				Named.Add(Token);
			}
		}
	}
	Named.Add(TEXT("batch"));
	TestTrue(TEXT("the skill names the tools it relies on"), Named.Num() >= 10);
	for (const FString& Tool : Named)
	{
		TestTrue(FString::Printf(TEXT("tool named in the skill exists: %s"), *Tool), FCursorToolRegistry::Get().Find(FName(*Tool)).IsValid());
	}

	// Numbers the other tests measure must be the ones the skill quotes.
	TestTrue(TEXT("quotes the default jump height and reach"), Body.Contains(TEXT("| 90 | 514 | 45 |")));
	return true;
}
// The numbers the skill quotes for the engine's default character and for the blockout cube, read the way the skill tells the agent to read them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorLevelDesignMetricsTest, "CursorUnreal.LevelDesign.Metrics", CursorTest::Flags)
bool FCursorLevelDesignMetricsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedTestAssets Assets;

	// Engine defaults, through object_get_properties on the class default object.
	FCursorToolResult R = Tool(*this, TEXT("object_get_properties"),
		TEXT("{\"object\":\"/Script/Engine.Default__Character:CharMoveComp\",\"names\":[\"JumpZVelocity\",\"GravityScale\",\"MaxWalkSpeed\",\"MaxStepHeight\",\"AirControl\",\"WalkableFloorAngle\",\"bConstrainToPlane\",\"PlaneConstraintNormal\"]}"));
	if (!R.bIsError)
	{
		const TSharedPtr<FJsonObject> P = R.Structured->GetObjectField(TEXT("properties"));
		Note(TEXT("Character movement defaults -> ") + R.Text);
		TestTrue(TEXT("default WalkableFloorAngle is about 44.8"), FMath::IsNearlyEqual(P->GetNumberField(TEXT("WalkableFloorAngle")), 44.765, 0.01));
		TestTrue(TEXT("plane constraint is readable and off by default"), P->HasField(TEXT("bConstrainToPlane")) && !P->GetBoolField(TEXT("bConstrainToPlane")) && P->HasField(TEXT("PlaneConstraintNormal")));
		TestEqual(TEXT("default JumpZVelocity"), P->GetNumberField(TEXT("JumpZVelocity")), 420.0);
		TestEqual(TEXT("default GravityScale"), P->GetNumberField(TEXT("GravityScale")), 1.0);
		TestEqual(TEXT("default MaxWalkSpeed"), P->GetNumberField(TEXT("MaxWalkSpeed")), 600.0);
		TestEqual(TEXT("default MaxStepHeight"), P->GetNumberField(TEXT("MaxStepHeight")), 45.0);
		TestTrue(TEXT("default AirControl"), FMath::IsNearlyEqual(P->GetNumberField(TEXT("AirControl")), 0.05, 0.001));
	}
	R = Tool(*this, TEXT("object_get_properties"), TEXT("{\"object\":\"/Script/Engine.Default__Character:CollisionCylinder\",\"names\":[\"CapsuleHalfHeight\",\"CapsuleRadius\"]}"));
	if (!R.bIsError)
	{
		const TSharedPtr<FJsonObject> P = R.Structured->GetObjectField(TEXT("properties"));
		TestEqual(TEXT("default capsule half height"), P->GetNumberField(TEXT("CapsuleHalfHeight")), 88.0);
		TestEqual(TEXT("default capsule radius"), P->GetNumberField(TEXT("CapsuleRadius")), 34.0);
	}
	R = Tool(*this, TEXT("object_get_properties"), TEXT("{\"object\":\"/Script/Engine.Default__Character\",\"names\":[\"JumpMaxCount\",\"JumpMaxHoldTime\"]}"));
	if (!R.bIsError)
	{
		const TSharedPtr<FJsonObject> P = R.Structured->GetObjectField(TEXT("properties"));
		TestEqual(TEXT("default JumpMaxCount"), P->GetNumberField(TEXT("JumpMaxCount")), 1.0);
		TestEqual(TEXT("default JumpMaxHoldTime"), P->GetNumberField(TEXT("JumpMaxHoldTime")), 0.0);
	}
	if (const UWorld* World = CursorToolHelpers::GetEditorWorld())
	{
		TestTrue(TEXT("default gravity is -980"), FMath::IsNearlyEqual(World->GetGravityZ(), -980.0f, 0.5f));
	}

	// How the skill finds the player character: level override, then project default, then the game mode's pawn.
	R = Tool(*this, TEXT("object_get_properties"), TEXT("{\"object\":\"WorldSettings\",\"names\":[\"DefaultGameMode\",\"KillZ\"]}"));
	Note(TEXT("WorldSettings -> ") + R.Text);
	R = Tool(*this, TEXT("object_get_properties"), TEXT("{\"object\":\"/Script/EngineSettings.Default__GameMapsSettings\",\"names\":[\"GlobalDefaultGameMode\"]}"));
	Note(TEXT("GameMapsSettings -> ") + R.Text);
	R = Tool(*this, TEXT("object_get_properties"), TEXT("{\"object\":\"/Script/Engine.Default__GameModeBase\",\"names\":[\"DefaultPawnClass\"]}"));
	Note(TEXT("GameModeBase -> ") + R.Text);
	TestTrue(TEXT("a plain GameModeBase gives a DefaultPawn, which is not a Character"), !R.bIsError && R.Text.Contains(TEXT("DefaultPawn")));

	// A Blueprint character: the same components are read, and tuned, on its own default object.
	const FString Hero = Assets.Add(TEXT("BP_Hero"));
	Tool(*this, TEXT("blueprint_create"), Args(TEXT("{\"path\":\"{0}\",\"parent_class\":\"Character\"}"), Hero));
	const FString HeroDefaults = FString::Printf(TEXT("%s.Default__%s_C"), *Hero, *FPackageName::GetShortName(Hero));
	Tool(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}:CharMoveComp\",\"properties\":{\"JumpZVelocity\":750,\"GravityScale\":1.75,\"MaxWalkSpeed\":500}}"), HeroDefaults));
	Compile(*this, Hero);
	R = Tool(*this, TEXT("object_get_properties"), Args(TEXT("{\"object\":\"{0}:CharMoveComp\",\"names\":[\"JumpZVelocity\",\"GravityScale\",\"MaxWalkSpeed\"]}"), HeroDefaults));
	if (!R.bIsError)
	{
		const TSharedPtr<FJsonObject> P = R.Structured->GetObjectField(TEXT("properties"));
		TestEqual(TEXT("Blueprint character keeps its JumpZVelocity through a compile"), P->GetNumberField(TEXT("JumpZVelocity")), 750.0);
		TestEqual(TEXT("Blueprint character keeps its GravityScale through a compile"), P->GetNumberField(TEXT("GravityScale")), 1.75);
	}
	R = Tool(*this, TEXT("object_get_properties"), Args(TEXT("{\"object\":\"{0}:CollisionCylinder\",\"names\":[\"CapsuleHalfHeight\",\"CapsuleRadius\"]}"), HeroDefaults));
	TestFalse(TEXT("Blueprint character capsule is readable"), R.bIsError);

	// The blockout cube is 100 cm on each side with its pivot in the middle.
	if (const UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, CubePath))
	{
		const FBox Box = Cube->GetBoundingBox();
		TestTrue(TEXT("cube is 100 cm on each side"), Box.GetSize().Equals(FVector(100.0), 0.01));
		TestTrue(TEXT("cube pivot is its centre"), Box.GetCenter().IsNearlyZero(0.01));
	}
	else
	{
		AddError(TEXT("/Engine/BasicShapes/Cube did not load."));
	}
	return true;
}

// Placing things in a level the way the skill describes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorLevelDesignBlockoutTest, "CursorUnreal.LevelDesign.Blockout", CursorTest::Flags)
bool FCursorLevelDesignBlockoutTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedTestAssets Assets;
	FScopedLevelActors Actors;
	FString Error;

	// A platform 1000 x 400 x 40 whose top is at Z=200: scale is size / 100, location Z is top minus half the thickness.
	const FString Platform = Actors.Add(TEXT("CULD_Platform"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[500,0,180],\"scale\":[10,4,0.4],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), CubePath, Platform));
	if (AActor* Actor = CursorToolHelpers::FindActor(Platform, Error))
	{
		TestTrue(TEXT("a mesh path spawns a StaticMeshActor"), Actor->IsA<AStaticMeshActor>());
		FVector Origin, Extent;
		Actor->GetActorBounds(false, Origin, Extent);
		TestTrue(TEXT("platform spans 1000 x 400 x 40"), Extent.Equals(FVector(500, 200, 20), 0.1));
		TestTrue(TEXT("platform top is at 200"), FMath::IsNearlyEqual(Origin.Z + Extent.Z, 200.0, 0.1));
		TestTrue(TEXT("platform starts at X=0"), FMath::IsNearlyEqual(Origin.X - Extent.X, 0.0, 0.1));

		// The grid material makes scale readable.
		Tool(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}.StaticMeshComponent0\",\"properties\":{\"OverrideMaterials\":[\"/Engine/EngineMaterials/WorldGridMaterial\"]}}"), Platform));
		const UMaterialInterface* Material = CastChecked<AStaticMeshActor>(Actor)->GetStaticMeshComponent()->GetMaterial(0);
		TestTrue(TEXT("grid material applied"), Material && Material->GetName() == TEXT("WorldGridMaterial"));
	}
	else
	{
		AddError(TEXT("The platform was not spawned: ") + Error);
	}

	// A crate the player can push and that triggers can feel.
	const FString Crate = Actors.Add(TEXT("CULD_Crate"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[300,0,235],\"scale\":[0.7,0.7,0.7],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), CubePath, Crate));
	Tool(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}.StaticMeshComponent0\",\"properties\":{\"Mobility\":\"Movable\",\"BodyInstance.bSimulatePhysics\":true,\"BodyInstance.LinearDamping\":3,\"BodyInstance.AngularDamping\":5,\"BodyInstance.DOFMode\":\"XZPlane\",\"bGenerateOverlapEvents\":true}}"), Crate));
	if (const AStaticMeshActor* Actor = Cast<AStaticMeshActor>(CursorToolHelpers::FindActor(Crate, Error)))
	{
		const UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
		TestTrue(TEXT("crate is movable"), Mesh->Mobility == EComponentMobility::Movable);
		TestTrue(TEXT("crate simulates physics"), Mesh->BodyInstance.bSimulatePhysics);
		TestTrue(TEXT("crate is damped so it stops when let go"), Mesh->BodyInstance.LinearDamping == 3.0f && Mesh->BodyInstance.AngularDamping == 5.0f);
		TestTrue(TEXT("crate is kept on the side-scrolling plane"), Mesh->BodyInstance.DOFMode == EDOFMode::XZPlane);
		TestTrue(TEXT("crate generates overlap events"), Mesh->GetGenerateOverlapEvents());
	}

	// Kit Blueprints placed and configured per instance.
	const FKit Kit = BuildKit(*this, Assets);

	const FString Lift = Actors.Add(TEXT("CULD_Lift"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[1400,0,185],\"scale\":[3,3,0.3],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), Kit.MovingPlatform, Lift));
	Tool(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}.Mover\",\"properties\":{")
		TEXT("\"ControlPoints\":[{\"PositionControlPoint\":{\"X\":0,\"Y\":0,\"Z\":0},\"bPositionIsRelative\":true},{\"PositionControlPoint\":{\"X\":400,\"Y\":0,\"Z\":0},\"bPositionIsRelative\":true}],")
		TEXT("\"Duration\":2,\"BehaviourType\":\"OneShot\",\"bAutoActivate\":false}}"), Lift));
	AActor* LiftActor = CursorToolHelpers::FindActor(Lift, Error);
	if (LiftActor)
	{
		const UStaticMeshComponent* Mesh = LiftActor->FindComponentByClass<UStaticMeshComponent>();
		TestTrue(TEXT("placed platform has the cube mesh from the Blueprint"), Mesh && Mesh->GetStaticMesh() && Mesh->GetStaticMesh()->GetName() == TEXT("Cube"));
		TestTrue(TEXT("the mesh is the root, so the mover moves it"), Mesh && LiftActor->GetRootComponent() == Mesh);
		TestTrue(TEXT("placed platform can move at runtime"), Mesh && Mesh->Mobility == EComponentMobility::Movable);
		FVector Origin, Extent;
		LiftActor->GetActorBounds(false, Origin, Extent);
		TestTrue(TEXT("placed platform spans 300 x 300 x 30"), Extent.Equals(FVector(150, 150, 15), 0.1));

		const UInterpToMovementComponent* Mover = LiftActor->FindComponentByClass<UInterpToMovementComponent>();
		if (TestNotNull(TEXT("placed platform has a mover"), Mover))
		{
			TestFalse(TEXT("Blueprint default: no sweep"), Mover->bSweep);
			TestEqual(TEXT("instance override: two control points"), Mover->ControlPoints.Num(), 2);
			TestTrue(TEXT("instance override: travels 400 along X"), Mover->ControlPoints.Num() == 2 && Mover->ControlPoints[1].PositionControlPoint.Equals(FVector(400, 0, 0)));
			TestTrue(TEXT("instance override: one shot"), Mover->BehaviourType == EInterpToBehaviourType::OneShot);
			TestFalse(TEXT("instance override: waits for a trigger"), Mover->bAutoActivate);
			TestTrue(TEXT("instance override: duration"), FMath::IsNearlyEqual(Mover->Duration, 2.0f));
		}
	}
	else
	{
		AddError(TEXT("The moving platform was not spawned: ") + Error);
	}

	// A second, untouched instance keeps the Blueprint's defaults.
	const FString Bobber = Actors.Add(TEXT("CULD_Bobber"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[2000,0,185],\"scale\":[2,2,0.3],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), Kit.MovingPlatform, Bobber));
	if (const AActor* Actor = CursorToolHelpers::FindActor(Bobber, Error))
	{
		const UInterpToMovementComponent* Mover = Actor->FindComponentByClass<UInterpToMovementComponent>();
		TestTrue(TEXT("Blueprint defaults: ping-pong 300 up over 3 s, starting on its own"), Mover && Mover->bAutoActivate && Mover->BehaviourType == EInterpToBehaviourType::PingPong
			&& Mover->ControlPoints.Num() == 2 && Mover->ControlPoints[1].PositionControlPoint.Equals(FVector(0, 0, 300)) && FMath::IsNearlyEqual(Mover->Duration, 3.0f));
	}

	// A trigger wired to the platform and restricted to the crate, by label.
	const FString Plate = Actors.Add(TEXT("CULD_Plate"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[900,0,225],\"scale\":[1.5,3,0.5],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), Kit.Trigger, Plate));
	Tool(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}\",\"properties\":{\"Target\":\"{1}\",\"RequiredActor\":\"{2}\"}}"), Plate, Lift, Crate));
	if (const AActor* Actor = CursorToolHelpers::FindActor(Plate, Error))
	{
		const FObjectProperty* Target = FindFProperty<FObjectProperty>(Actor->GetClass(), TEXT("Target"));
		const FObjectProperty* Required = FindFProperty<FObjectProperty>(Actor->GetClass(), TEXT("RequiredActor"));
		TestTrue(TEXT("trigger points at the platform"), Target && Target->GetObjectPropertyValue_InContainer(Actor) == LiftActor);
		TestTrue(TEXT("trigger is restricted to the crate"), Required && Required->GetObjectPropertyValue_InContainer(Actor) == CursorToolHelpers::FindActor(Crate, Error));

		const UBoxComponent* Box = Actor->FindComponentByClass<UBoxComponent>();
		if (TestNotNull(TEXT("trigger has its box"), Box))
		{
			TestTrue(TEXT("trigger box is 100 cm at scale 1"), Box->GetUnscaledBoxExtent().Equals(FVector(50.0)));
			TestTrue(TEXT("trigger box follows the actor's scale"), Box->GetScaledBoxExtent().Equals(FVector(75, 150, 25), 0.1));
			TestFalse(TEXT("trigger is drawn in game"), Box->bHiddenInGame);
			TestTrue(TEXT("trigger overlaps pawns"), Box->GetGenerateOverlapEvents() && Box->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Overlap);
		}
	}

	// Where the player starts, and a marker to come back to after a fall.
	const FString Start = Actors.Add(TEXT("CULD_Start"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"PlayerStart\",\"location\":[150,0,300],\"label\":\"{0}\",\"folder\":\"CursorUnrealTests\"}"), Start));
	const AActor* StartActor = CursorToolHelpers::FindActor(Start, Error);
	TestTrue(TEXT("a PlayerStart is placed where asked"), StartActor && StartActor->GetClass()->GetName() == TEXT("PlayerStart") && StartActor->GetActorLocation().Equals(FVector(150, 0, 300), 0.1));

	const FString Respawn = Actors.Add(TEXT("CULD_Respawn"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"TargetPoint\",\"location\":[150,0,300],\"label\":\"{0}\",\"folder\":\"CursorUnrealTests\"}"), Respawn));
	const AActor* RespawnActor = CursorToolHelpers::FindActor(Respawn, Error);
	TestTrue(TEXT("a TargetPoint marks a respawn position"), RespawnActor && RespawnActor->IsA<ATargetPoint>());

	const FString Pit = Actors.Add(TEXT("CULD_Pit"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[2600,0,-200],\"scale\":[6,10,1],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), Kit.KillZone, Pit));
	Tool(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}\",\"properties\":{\"RespawnPoint\":\"{1}\"}}"), Pit, Respawn));
	if (const AActor* Actor = CursorToolHelpers::FindActor(Pit, Error))
	{
		const FObjectProperty* Property = FindFProperty<FObjectProperty>(Actor->GetClass(), TEXT("RespawnPoint"));
		TestTrue(TEXT("kill zone sends the player back to the marker"), Property && Property->GetObjectPropertyValue_InContainer(Actor) == RespawnActor);
	}

	const FString Pad = Actors.Add(TEXT("CULD_Pad"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[3200,0,220],\"scale\":[1.5,1.5,0.4],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), Kit.JumpPad, Pad));
	Tool(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}\",\"properties\":{\"LaunchVelocity\":{\"X\":0,\"Y\":0,\"Z\":1200}}}"), Pad));
	if (const AActor* Actor = CursorToolHelpers::FindActor(Pad, Error))
	{
		const FStructProperty* Property = FindFProperty<FStructProperty>(Actor->GetClass(), TEXT("LaunchVelocity"));
		TestTrue(TEXT("jump pad strength is set per instance"), Property && Property->ContainerPtrToValuePtr<FVector>(Actor)->Equals(FVector(0, 0, 1200)));
	}

	const FString Goal = Actors.Add(TEXT("CULD_Goal"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[4000,0,300],\"scale\":[1,4,2],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), Kit.Goal, Goal));
	TestNotNull(TEXT("goal placed"), CursorToolHelpers::FindActor(Goal, Error));

	// The level's game mode is the first place the skill looks for the player character, and where it is set when the user agrees.
	Tool(*this, TEXT("object_set_properties"), TEXT("{\"object\":\"WorldSettings\",\"properties\":{\"DefaultGameMode\":\"/Script/Engine.GameMode\"}}"));
	const FCursorToolResult GameMode = CursorTest::Call(TEXT("object_get_properties"), TEXT("{\"object\":\"WorldSettings\",\"names\":[\"DefaultGameMode\"]}"));
	TestTrue(TEXT("the level's game mode can be set: ") + GameMode.Text, !GameMode.bIsError && GameMode.Text.Contains(TEXT("/Script/Engine.GameMode'")));
	Tool(*this, TEXT("object_set_properties"), TEXT("{\"object\":\"WorldSettings\",\"properties\":{\"DefaultGameMode\":\"None\"}}"));

	// The level's kill height, for levels that only need "falling off ends the run".
	const FCursorToolResult KillZ = CursorTest::Call(TEXT("object_get_properties"), TEXT("{\"object\":\"WorldSettings\",\"names\":[\"KillZ\"]}"));
	TestFalse(TEXT("WorldSettings is addressable by name: ") + KillZ.Text, KillZ.bIsError);
	return true;
}

// The kit Blueprints, played: does a character ride, trigger, die, bounce?
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorLevelDesignPlayTest, "CursorUnreal.LevelDesign.Play", CursorTest::Flags)
bool FCursorLevelDesignPlayTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedTestAssets Assets;
	const FKit Kit = BuildKit(*this, Assets);

	UClass* PlatformClass = GeneratedClass(Kit.MovingPlatform);
	UClass* TriggerClass = GeneratedClass(Kit.Trigger);
	UClass* KillZoneClass = GeneratedClass(Kit.KillZone);
	UClass* JumpPadClass = GeneratedClass(Kit.JumpPad);
	if (!PlatformClass || !TriggerClass || !KillZoneClass || !JumpPadClass)
	{
		AddError(TEXT("The kit did not build."));
		return false;
	}

	// --- 1. Jump metrics of the default character on flat ground.
	{
		FPlayWorld Play;
		Play.Cube(FVector(4000, 0, 0), FVector(10000, 1000, 100));
		ACharacter* Hero = Play.Player(FVector(0, 0, 0));
		Play.BeginPlay();
		Play.Tick(0.5f);
		TestTrue(TEXT("the character stands on the cube"), Hero->GetCharacterMovement()->IsMovingOnGround());
		const double Ground = FPlayWorld::FeetZ(Hero);

		// Standing jump: how high.
		double Apex = Ground;
		Hero->Jump();
		Play.Tick(1.5f, [&] { Apex = FMath::Max(Apex, FPlayWorld::FeetZ(Hero)); });
		Hero->StopJumping();
		Note(FString::Printf(TEXT("default character: standing jump rises %.1f cm"), Apex - Ground));
		TestTrue(TEXT("jump height is v^2 / 2g = 90 cm"), FMath::IsNearlyEqual(Apex - Ground, 90.0, 4.0));

		// Running jump: how far, take-off to landing, on flat ground.
		Play.Tick(1.0f, [&] { Hero->AddMovementInput(FVector::ForwardVector); });
		Note(FString::Printf(TEXT("default character: run speed %.0f cm/s"), Hero->GetVelocity().Size2D()));
		const double TakeOff = Hero->GetActorLocation().X;
		double Landing = TakeOff;
		bool bLeftGround = false;
		bool bLanded = false;
		Hero->Jump();
		Play.Tick(1.5f, [&]
		{
			Hero->AddMovementInput(FVector::ForwardVector);
			if (!bLanded)
			{
				const bool bOnGround = Hero->GetCharacterMovement()->IsMovingOnGround();
				bLeftGround |= !bOnGround;
				if (bLeftGround && bOnGround)
				{
					bLanded = true;
					Landing = Hero->GetActorLocation().X;
				}
			}
		});
		Hero->StopJumping();
		Note(FString::Printf(TEXT("default character: running jump covers %.1f cm"), Landing - TakeOff));
		TestTrue(TEXT("running jump length is speed x 2v/g = 514 cm"), bLanded && FMath::IsNearlyEqual(Landing - TakeOff, 514.0, 25.0));
	}

	// --- 2. Moving platforms carry the character, up and sideways.
	for (const bool bVertical : { true, false })
	{
		FPlayWorld Play;
		const FVector Travel = bVertical ? FVector(0, 0, 300) : FVector(400, 0, 0);
		AActor* Platform = Play.World->SpawnActor<AActor>(PlatformClass, FVector(0, 0, -15), FRotator::ZeroRotator);
		Platform->SetActorScale3D(FVector(3, 3, 0.3));
		UInterpToMovementComponent* Mover = Platform->FindComponentByClass<UInterpToMovementComponent>();
		Mover->ControlPoints = { FInterpControlPoint(FVector::ZeroVector, true), FInterpControlPoint(Travel, true) };
		ACharacter* Hero = Play.Player(FVector(0, 0, 0));
		Play.BeginPlay();
		const FVector Before = Hero->GetActorLocation();
		Play.Tick(2.9f); // Duration is 3 s: almost at the far end.
		const FVector Moved = Hero->GetActorLocation() - Before;
		const FVector PlatformMoved = Platform->GetActorLocation() - FVector(0, 0, -15);
		Note(FString::Printf(TEXT("moving platform (%s): platform moved %s, rider moved %s, standing=%d"),
			bVertical ? TEXT("up") : TEXT("sideways"), *PlatformMoved.ToCompactString(), *Moved.ToCompactString(), Hero->GetCharacterMovement()->IsMovingOnGround()));
		TestTrue(TEXT("the platform travels"), PlatformMoved.Equals(Travel * (2.9 / 3.0), 25.0));
		TestTrue(TEXT("the rider is carried with it"), Moved.Equals(PlatformMoved, 20.0));
		TestTrue(TEXT("the rider is still standing on it"), Hero->GetCharacterMovement()->GetMovementBase() == Platform->GetRootComponent());

		// And it comes back (ping-pong).
		Play.Tick(3.0f);
		TestTrue(TEXT("the platform returns"), (Platform->GetActorLocation() - FVector(0, 0, -15)).Size() < 40.0);
	}

	// What goes wrong with the component's default (bSweep on): recorded so the skill's warning stays true.
	{
		FPlayWorld Play;
		AActor* Platform = Play.World->SpawnActor<AActor>(PlatformClass, FVector(0, 0, -15), FRotator::ZeroRotator);
		Platform->SetActorScale3D(FVector(3, 3, 0.3));
		Platform->FindComponentByClass<UInterpToMovementComponent>()->bSweep = true;
		ACharacter* Hero = Play.Player(FVector(0, 0, 0));
		Play.BeginPlay();
		const double Before = Hero->GetActorLocation().Z;
		Play.Tick(2.9f);
		Note(FString::Printf(TEXT("moving platform with bSweep on: rider rose %.1f cm of 290"), Hero->GetActorLocation().Z - Before));
	}

	// A block sliding along the floor into a standing player, with and without sweep. Recorded, not asserted:
	// the skill describes what happens, and this is where to look if an engine version changes it.
	for (const bool bSweep : { true, false })
	{
		FPlayWorld Play;
		Play.Cube(FVector(0, 0, 0), FVector(4000, 1000, 100));
		AActor* Block = Play.World->SpawnActor<AActor>(PlatformClass, FVector(-300, 0, 52), FRotator::ZeroRotator);
		UInterpToMovementComponent* Mover = Block->FindComponentByClass<UInterpToMovementComponent>();
		Mover->ControlPoints = { FInterpControlPoint(FVector::ZeroVector, true), FInterpControlPoint(FVector(600, 0, 0), true) };
		Mover->bSweep = bSweep;
		ACharacter* Hero = Play.Player(FVector(0, 0, 0));
		Play.BeginPlay();
		Play.Tick(2.9f);
		Note(FString::Printf(TEXT("block sliding into a standing player, bSweep %s: block moved %.0f of 580 cm, player moved %s, standing=%d"),
			bSweep ? TEXT("on") : TEXT("off"), Block->GetActorLocation().X + 300.0, *Hero->GetActorLocation().ToCompactString(), Hero->GetCharacterMovement()->IsMovingOnGround()));
	}

	// --- 3. A trigger starts a waiting platform; RequiredActor restricts who can.
	{
		// A level instance with "Auto Activate" off never activates. Spawned at runtime the component activates
		// before the test could change it, so the Blueprint default is switched for the spawn instead.
		UInterpToMovementComponent* MoverDefaults = FindObject<UInterpToMovementComponent>(nullptr, *TemplatePath(Kit.MovingPlatform, TEXT("Mover")));
		if (!TestNotNull(TEXT("mover defaults found"), MoverDefaults))
		{
			return false;
		}
		MoverDefaults->bAutoActivate = false;
		ON_SCOPE_EXIT { MoverDefaults->bAutoActivate = true; };

		FPlayWorld Play;
		Play.Cube(FVector(0, 0, 0), FVector(4000, 1000, 100));
		auto SpawnWaitingPlatform = [&](const FVector& Location)
		{
			AActor* Platform = Play.World->SpawnActor<AActor>(PlatformClass, Location, FRotator::ZeroRotator);
			UInterpToMovementComponent* Mover = Platform->FindComponentByClass<UInterpToMovementComponent>();
			Mover->ControlPoints = { FInterpControlPoint(FVector::ZeroVector, true), FInterpControlPoint(FVector(0, 0, 200), true) };
			Mover->Duration = 1.0f;
			Mover->BehaviourType = EInterpToBehaviourType::OneShot;
			return Platform;
		};
		AActor* Door = SpawnWaitingPlatform(FVector(1000, 0, 100));
		AActor* Gate = SpawnWaitingPlatform(FVector(1500, 0, 100));
		AStaticMeshActor* Crate = Play.Cube(FVector(-1500, 0, 70), FVector(70, 70, 70));
		Crate->GetStaticMeshComponent()->SetGenerateOverlapEvents(true);

		AActor* AnyoneTrigger = Play.World->SpawnActor<AActor>(TriggerClass, FVector(500, 0, 60), FRotator::ZeroRotator);
		SetActorReference(AnyoneTrigger, TEXT("Target"), Door);
		AActor* CratePlate = Play.World->SpawnActor<AActor>(TriggerClass, FVector(500, 300, 60), FRotator::ZeroRotator);
		SetActorReference(CratePlate, TEXT("Target"), Gate);
		SetActorReference(CratePlate, TEXT("RequiredActor"), Crate);

		// A trigger in the door's path: kit platforms set triggers off too, which the skill warns about.
		AActor* Chained = SpawnWaitingPlatform(FVector(1000, 400, 100));
		AActor* InDoorsPath = Play.World->SpawnActor<AActor>(TriggerClass, FVector(1000, 0, 320), FRotator::ZeroRotator);
		SetActorReference(InDoorsPath, TEXT("Target"), Chained);

		// A lift that waits for the player to step on it: the trigger sits 10 cm above the lift's top, so the
		// lift does not set it off itself.
		AActor* Lift = SpawnWaitingPlatform(FVector(3000, 0, -15));
		Lift->SetActorScale3D(FVector(3, 3, 0.3));
		Lift->FindComponentByClass<UInterpToMovementComponent>()->BehaviourType = EInterpToBehaviourType::PingPong;
		AActor* BoardingTrigger = Play.World->SpawnActor<AActor>(TriggerClass, FVector(3000, 0, 60), FRotator::ZeroRotator);
		BoardingTrigger->SetActorScale3D(FVector(3, 3, 1));
		SetActorReference(BoardingTrigger, TEXT("Target"), Lift);

		ACharacter* Hero = Play.Player(FVector(-500, 0, 0));
		Play.BeginPlay();
		Play.Tick(1.0f);
		TestTrue(TEXT("a waiting platform stays put until triggered"), Door->GetActorLocation().Equals(FVector(1000, 0, 100), 1.0));
		TestTrue(TEXT("a lift under its own trigger does not start itself"), Lift->GetActorLocation().Equals(FVector(3000, 0, -15), 1.0));

		Hero->SetActorLocation(FVector(500, 0, 92));
		Play.Tick(1.5f);
		Note(FString::Printf(TEXT("trigger: door rose %.1f cm after the player entered"), Door->GetActorLocation().Z - 100.0));
		TestTrue(TEXT("the player entering the trigger starts its target"), FMath::IsNearlyEqual(Door->GetActorLocation().Z, 300.0, 5.0));

		Note(FString::Printf(TEXT("trigger in a platform's path: its target rose %.1f cm after the platform passed through"), Chained->GetActorLocation().Z - 100.0));
		TestTrue(TEXT("a kit platform passing through a trigger sets it off"), Chained->GetActorLocation().Z - 100.0 > 100.0);

		Hero->SetActorLocation(FVector(500, 300, 92));
		Play.Tick(1.5f);
		TestTrue(TEXT("a trigger with RequiredActor ignores the player"), Gate->GetActorLocation().Equals(FVector(1500, 0, 100), 1.0));

		Hero->SetActorLocation(FVector(-500, 0, 92));
		Crate->SetActorLocation(FVector(500, 300, 40));
		Play.Tick(1.5f);
		Note(FString::Printf(TEXT("trigger: gate rose %.1f cm after the crate was put on the plate"), Gate->GetActorLocation().Z - 100.0));
		TestTrue(TEXT("the required actor entering the trigger starts its target"), FMath::IsNearlyEqual(Gate->GetActorLocation().Z, 300.0, 5.0));

		Hero->SetActorLocation(FVector(3000, 0, 92));
		Play.Tick(0.9f);
		Note(FString::Printf(TEXT("boarding trigger: lift rose %.1f cm in 0.9 s after the player stepped on, player feet at Z=%.1f"), Lift->GetActorLocation().Z + 15.0, FPlayWorld::FeetZ(Hero)));
		TestTrue(TEXT("stepping onto a waiting lift starts it"), Lift->GetActorLocation().Z + 15.0 > 150.0);
		TestTrue(TEXT("and it carries the player"), Hero->GetCharacterMovement()->GetMovementBase() == Lift->GetRootComponent());
	}

	// --- 4. Kill zone and jump pad.
	{
		FPlayWorld Play;
		Play.Cube(FVector(0, 0, 0), FVector(4000, 1000, 100));
		ATargetPoint* Respawn = Play.World->SpawnActor<ATargetPoint>(FVector(-1000, 0, 150), FRotator::ZeroRotator);
		AActor* KillZone = Play.World->SpawnActor<AActor>(KillZoneClass, FVector(1000, 0, 60), FRotator::ZeroRotator);
		SetActorReference(KillZone, TEXT("RespawnPoint"), Respawn);
		AActor* JumpPad = Play.World->SpawnActor<AActor>(JumpPadClass, FVector(0, 300, 30), FRotator::ZeroRotator);
		JumpPad->SetActorScale3D(FVector(1.5, 1.5, 0.4));

		ACharacter* Hero = Play.Player(FVector(-500, 0, 0));
		Play.BeginPlay();
		Play.Tick(0.5f);

		Hero->SetActorLocation(FVector(1000, 0, 92));
		Note(FString::Printf(TEXT("kill zone: player is at %s after touching it"), *Hero->GetActorLocation().ToCompactString()));
		TestTrue(TEXT("touching a kill zone puts the player at its respawn point"), Hero->GetActorLocation().Equals(FVector(-1000, 0, 150), 1.0));
		Play.Tick(1.0f);
		TestTrue(TEXT("the player lands and can carry on"), Hero->GetCharacterMovement()->IsMovingOnGround());

		const double Ground = FPlayWorld::FeetZ(Hero);
		double Apex = Ground;
		Hero->SetActorLocation(FVector(0, 300, 92));
		Play.Tick(2.5f, [&] { Apex = FMath::Max(Apex, FPlayWorld::FeetZ(Hero)); });
		Note(FString::Printf(TEXT("jump pad: launch of 1000 cm/s rises %.1f cm"), Apex - Ground));
		TestTrue(TEXT("a jump pad launch rises v^2 / 2g = 510 cm"), FMath::IsNearlyEqual(Apex - Ground, 510.0, 25.0));

		// A pad aimed at a higher platform, the way the skill works it out: land 450 cm ahead on a top 360 cm up,
		// peaking one jump height (90) above it. Vz = sqrt(2 g 450) = 940, air time 1.39 s, Vx = 450 / 1.39 = 324.
		// The 450 is counted from where the player is launched, which is where their capsule first touches the pad's
		// box: its near edge (75 cm from its centre here) less the capsule radius (34), so X = -109, landing at 341.
		Play.Cube(FVector(580, -300, 360), FVector(600, 300, 40));
		AActor* AimedPad = Play.World->SpawnActor<AActor>(JumpPadClass, FVector(0, -300, 30), FRotator::ZeroRotator);
		AimedPad->SetActorScale3D(FVector(1.5, 1.5, 0.4));
		if (const FStructProperty* Velocity = FindFProperty<FStructProperty>(AimedPad->GetClass(), TEXT("LaunchVelocity")))
		{
			*Velocity->ContainerPtrToValuePtr<FVector>(AimedPad) = FVector(324, 0, 940);
		}
		// The player is still bouncing on the first pad (a pad that launches straight up catches them again on the way down).
		Hero->SetActorLocation(FVector(-300, -300, 92));
		Hero->GetCharacterMovement()->StopMovementImmediately();
		Play.Tick(1.0f);
		TestTrue(TEXT("the player is standing before the run-up"), Hero->GetCharacterMovement()->IsMovingOnGround());
		// Run onto the pad at full speed, then let go: the landing spot must not depend on the run-up.
		bool bLaunched = false;
		Play.Tick(3.0f, [&]
		{
			bLaunched |= Hero->GetCharacterMovement()->IsFalling();
			if (!bLaunched)
			{
				Hero->AddMovementInput(FVector::ForwardVector);
			}
		});
		Note(FString::Printf(TEXT("aimed jump pad (324, 0, 940): player came to rest at X=%.0f (lands at 341, then stops within about 25), feet Z=%.0f (360 expected), standing=%d"),
			Hero->GetActorLocation().X, FPlayWorld::FeetZ(Hero), Hero->GetCharacterMovement()->IsMovingOnGround()));
		TestTrue(TEXT("an aimed jump pad lands the player on the higher platform"), FMath::IsNearlyEqual(FPlayWorld::FeetZ(Hero), 360.0, 5.0) && FMath::IsNearlyEqual(Hero->GetActorLocation().X, 355.0, 40.0));

		// Things other than the player: a crate dropped on the pad leaves the player alone, and a crate that
		// falls into a kill zone is put back at the marker like the player is.
		Hero->SetActorLocation(FVector(-500, 0, 92));
		Play.Tick(1.0f);
		AStaticMeshActor* Crate = Play.Cube(FVector(0, 300, 200), FVector(70, 70, 70), /*bPhysics*/ true);
		double HeroApex = FPlayWorld::FeetZ(Hero);
		Play.Tick(1.5f, [&] { HeroApex = FMath::Max(HeroApex, FPlayWorld::FeetZ(Hero)); });
		TestTrue(TEXT("a crate on a jump pad does not launch the player"), HeroApex < 10.0);
		Crate->SetActorLocation(FVector(1000, 0, 250), false, nullptr, ETeleportType::TeleportPhysics);
		Play.Tick(2.0f);
		Note(FString::Printf(TEXT("kill zone: a crate dropped into it ended at %s"), *Crate->GetActorLocation().ToCompactString()));
		TestTrue(TEXT("a crate that falls into a kill zone comes back to the marker"), FVector::Dist2D(Crate->GetActorLocation(), FVector(-1000, 0, 0)) < 150.0);
	}

	// --- 5. The player can push a physics crate.
	{
		FPlayWorld Play;
		Play.Cube(FVector(0, 0, 0), FVector(4000, 1000, 100));
		AStaticMeshActor* Crate = Play.Cube(FVector(200, 0, 72), FVector(70, 70, 70), /*bPhysics*/ true);
		ACharacter* Hero = Play.Player(FVector(0, 0, 0));
		Play.BeginPlay();
		Play.Tick(1.0f);
		const double CrateStart = Crate->GetActorLocation().X;
		Note(FString::Printf(TEXT("crate: settled at Z=%.1f (35 expected)"), Crate->GetActorLocation().Z));
		Play.Tick(2.0f, [&] { Hero->AddMovementInput(FVector::ForwardVector); });
		Note(FString::Printf(TEXT("crate: pushed %.1f cm in 2 s, player advanced to X=%.1f"), Crate->GetActorLocation().X - CrateStart, Hero->GetActorLocation().X));
		TestTrue(TEXT("walking into a physics crate moves it"), Crate->GetActorLocation().X - CrateStart > 50.0);
	}

	// How the crate feels with different settings: pushed for 2 s, then left alone for 1 s. Recorded so the
	// skill's recommended values come from a measurement.
	struct FCrateTuning { float LinearDamping; float AngularDamping; bool bLockToSideScrollPlane; };
	for (const FCrateTuning& Tuning : { FCrateTuning{ 0.01f, 0, false }, FCrateTuning{ 3, 5, false }, FCrateTuning{ 3, 5, true } })
	{
		FPlayWorld Play;
		Play.Cube(FVector(0, 0, 0), FVector(8000, 1000, 100));
		AStaticMeshActor* Crate = Play.Cube(FVector(200, 0, 72), FVector(70, 70, 70), /*bPhysics*/ true);
		UStaticMeshComponent* Mesh = Crate->GetStaticMeshComponent();
		Mesh->SetLinearDamping(Tuning.LinearDamping);
		Mesh->SetAngularDamping(Tuning.AngularDamping);
		if (Tuning.bLockToSideScrollPlane)
		{
			Mesh->BodyInstance.DOFMode = EDOFMode::XZPlane;
			Mesh->RecreatePhysicsState();
		}
		// The player is 15 cm off the crate's centre line, as a real push always is.
		ACharacter* Hero = Play.Player(FVector(0, 15, 0));
		Play.BeginPlay();
		Play.Tick(1.0f);
		const double CrateStart = Crate->GetActorLocation().X;
		Play.Tick(2.0f, [&] { Hero->AddMovementInput(FVector::ForwardVector); });
		const double Pushed = Crate->GetActorLocation().X - CrateStart;
		const double HeroAt = Hero->GetActorLocation().X;
		Play.Tick(1.0f);
		Note(FString::Printf(TEXT("crate tuning: mass %.0f kg, linear damping %.2f, angular damping %.0f, locked to XZ plane %d -> pushed %.0f cm in 2 s (player walked %.0f), coasted %.0f cm more, drifted %.0f cm in Y, rotation %s"),
			Mesh->GetMass(), Tuning.LinearDamping, Tuning.AngularDamping, Tuning.bLockToSideScrollPlane, Pushed, HeroAt, Crate->GetActorLocation().X - CrateStart - Pushed, Crate->GetActorLocation().Y, *Crate->GetActorRotation().ToCompactString()));
	}

	// The crate as a step: the player stands on it, and a jump from it has full height.
	{
		FPlayWorld Play;
		Play.Cube(FVector(0, 0, 0), FVector(4000, 1000, 100));
		AStaticMeshActor* Crate = Play.Cube(FVector(0, 0, 72), FVector(70, 70, 70), /*bPhysics*/ true);
		Crate->GetStaticMeshComponent()->SetLinearDamping(5.0f);
		Play.BeginPlay();
		Play.Tick(1.0f);
		ACharacter* Hero = Play.Player(FVector(0, 0, 75));
		Play.Tick(1.0f);
		const double Ground = FPlayWorld::FeetZ(Hero);
		double Apex = Ground;
		Hero->Jump();
		Play.Tick(1.5f, [&] { Apex = FMath::Max(Apex, FPlayWorld::FeetZ(Hero)); });
		Hero->StopJumping();
		Note(FString::Printf(TEXT("crate as a step: player stands at Z=%.1f on a 70 cm crate, jump from it reaches Z=%.1f; crate ended at %s"),
			Ground, Apex, *Crate->GetActorLocation().ToCompactString()));
		TestTrue(TEXT("the player can stand on a 70 cm crate"), FMath::IsNearlyEqual(Ground, 70.0, 3.0));
		TestTrue(TEXT("a jump from the crate reaches 150 cm"), Apex > 150.0);
	}
	return true;
}

namespace CursorLevelDesignTest
{
	struct FPlayerKit
	{
		FString Character;
		FString GameMode;
	};

	/** Builds the playable side-on character and its game mode exactly as Skills/unreal-level-design/player.md tells the agent to. */
	FPlayerKit BuildPlayer(FAutomationTestBase& Test, FScopedTestAssets& Assets)
	{
		FPlayerKit Kit;
		const FString P = Kit.Character = Assets.Add(TEXT("BP_PlatformerCharacter"));
		const FString Defaults = FString::Printf(TEXT("%s.Default__%s_C"), *P, *FPackageName::GetShortName(P));

		// --- Structure: the engine's Character, a visible block for a body, a camera on an arm.
		Tool(Test, TEXT("blueprint_create"), Args(TEXT("{\"path\":\"{0}\",\"parent_class\":\"Character\"}"), P));
		Tool(Test, TEXT("blueprint_add_component"), Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"StaticMeshComponent\",\"name\":\"Body\"}"), P));
		Tool(Test, TEXT("blueprint_add_component"), Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"SpringArmComponent\",\"name\":\"CameraBoom\"}"), P));
		Tool(Test, TEXT("blueprint_add_component"), Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"CameraComponent\",\"name\":\"Camera\",\"parent\":\"CameraBoom\"}"), P));
		Compile(Test, P);

		// A 50 x 50 x 170 cm block inside the capsule. It is only there to be seen: the capsule does the colliding.
		Tool(Test, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}\",\"properties\":{\"StaticMesh\":\"{1}\",\"RelativeScale3D\":{\"X\":0.5,\"Y\":0.5,\"Z\":1.7},")
			TEXT("\"BodyInstance.CollisionProfileName\":\"NoCollision\",\"bGenerateOverlapEvents\":false}}"), TemplatePath(P, TEXT("Body")), CubePath));
		// Side-on: the arm points down -Y whichever way the character faces, so the camera sits 900 cm out on +Y.
		Tool(Test, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}\",\"properties\":{\"TargetArmLength\":900,\"bDoCollisionTest\":false,\"bUsePawnControlRotation\":false,")
			TEXT("\"bInheritPitch\":false,\"bInheritYaw\":false,\"bInheritRoll\":false,\"RelativeRotation\":{\"Pitch\":0,\"Yaw\":-90,\"Roll\":0}}}"), TemplatePath(P, TEXT("CameraBoom"))));
		// Face the way it runs, and stay on the course's plane (Y cannot change).
		Tool(Test, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}\",\"properties\":{\"bUseControllerRotationYaw\":false}}"), Defaults));
		Tool(Test, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}:CharMoveComp\",\"properties\":{\"bOrientRotationToMovement\":true,\"bConstrainToPlane\":true,\"PlaneConstraintAxisSetting\":\"Y\"}}"), Defaults));
		Compile(Test, P);

		// --- Controls: every frame, ask the player controller which keys are down and move accordingly.
		const FString Tick = AddNode(Test, P, TEXT("\"kind\":\"event\",\"name\":\"Tick\""));
		const FString Controller = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"GetPlayerController\",\"class\":\"GameplayStatics\""));
		auto KeyReader = [&](const TCHAR* Function, const TCHAR* Key)
		{
			const FString Node = AddNode(Test, P, *FString::Printf(TEXT("\"kind\":\"call_function\",\"function\":\"%s\",\"class\":\"PlayerController\""), Function));
			Connect(Test, P, Controller, TEXT("ReturnValue"), Node, TEXT("self"));
			PinDefault(Test, P, Node, TEXT("Key"), Key);
			return Node;
		};
		auto Math = [&](const TCHAR* Function)
		{
			return AddNode(Test, P, *FString::Printf(TEXT("\"kind\":\"call_function\",\"function\":\"%s\",\"class\":\"KismetMathLibrary\""), Function));
		};
		/** Either of two keys held gives 1, neither gives 0. */
		auto HeldAsNumber = [&](const TCHAR* KeyA, const TCHAR* KeyB)
		{
			const FString A = KeyReader(TEXT("IsInputKeyDown"), KeyA);
			const FString B = KeyReader(TEXT("IsInputKeyDown"), KeyB);
			const FString Either = Math(TEXT("BooleanOR"));
			const FString Number = Math(TEXT("SelectFloat"));
			Connect(Test, P, A, TEXT("ReturnValue"), Either, TEXT("A"));
			Connect(Test, P, B, TEXT("ReturnValue"), Either, TEXT("B"));
			Connect(Test, P, Either, TEXT("ReturnValue"), Number, TEXT("bPickA"));
			PinDefault(Test, P, Number, TEXT("A"), TEXT("1.0"));
			PinDefault(Test, P, Number, TEXT("B"), TEXT("0.0"));
			return Number;
		};
		auto Move = [&](const TCHAR* Direction, const FString& ScaleNode)
		{
			const FString Node = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"AddMovementInput\",\"class\":\"Pawn\""));
			PinDefault(Test, P, Node, TEXT("WorldDirection"), Direction);
			Connect(Test, P, ScaleNode, TEXT("ReturnValue"), Node, TEXT("ScaleValue"));
			return Node;
		};

		const FString MoveRight = Move(TEXT("(X=1,Y=0,Z=0)"), HeldAsNumber(TEXT("D"), TEXT("Right")));
		const FString MoveLeft = Move(TEXT("(X=-1,Y=0,Z=0)"), HeldAsNumber(TEXT("A"), TEXT("Left")));
		const FString MoveStick = Move(TEXT("(X=1,Y=0,Z=0)"), KeyReader(TEXT("GetInputAnalogKeyState"), TEXT("Gamepad_LeftX")));

		const FString JumpKey = KeyReader(TEXT("WasInputKeyJustPressed"), TEXT("SpaceBar"));
		const FString JumpButton = KeyReader(TEXT("WasInputKeyJustPressed"), TEXT("Gamepad_FaceButton_Bottom"));
		const FString JumpEither = Math(TEXT("BooleanOR"));
		const FString JumpBranch = AddNode(Test, P, TEXT("\"kind\":\"branch\""));
		const FString Jump = AddNode(Test, P, TEXT("\"kind\":\"call_function\",\"function\":\"Jump\",\"class\":\"Character\""));
		Connect(Test, P, JumpKey, TEXT("ReturnValue"), JumpEither, TEXT("A"));
		Connect(Test, P, JumpButton, TEXT("ReturnValue"), JumpEither, TEXT("B"));
		Connect(Test, P, JumpEither, TEXT("ReturnValue"), JumpBranch, TEXT("Condition"));

		Connect(Test, P, Tick, TEXT("then"), MoveRight, TEXT("execute"));
		Connect(Test, P, MoveRight, TEXT("then"), MoveLeft, TEXT("execute"));
		Connect(Test, P, MoveLeft, TEXT("then"), MoveStick, TEXT("execute"));
		Connect(Test, P, MoveStick, TEXT("then"), JumpBranch, TEXT("execute"));
		Connect(Test, P, JumpBranch, TEXT("then"), Jump, TEXT("execute"));
		Compile(Test, P);

		// --- Game mode: Play spawns this character at the Player Start.
		const FString G = Kit.GameMode = Assets.Add(TEXT("BP_PlatformerGameMode"));
		Tool(Test, TEXT("blueprint_create"), Args(TEXT("{\"path\":\"{0}\",\"parent_class\":\"GameModeBase\"}"), G));
		Compile(Test, G);
		Tool(Test, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}.Default__{1}_C\",\"properties\":{\"DefaultPawnClass\":\"{2}\"}}"), G, FPackageName::GetShortName(G), P));
		Compile(Test, G);
		return Kit;
	}
}

// The character the skill tells the agent to build is driven by real key presses: the thing a user checks first,
// and the thing that did not work, four times over, in the session this test came from.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorLevelDesignControlsTest, "CursorUnreal.LevelDesign.Controls", CursorTest::Flags)
bool FCursorLevelDesignControlsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedTestAssets Assets;
	const FPlayerKit Kit = BuildPlayer(*this, Assets);
	UClass* CharacterClass = GeneratedClass(Kit.Character);
	UClass* GameModeClass = GeneratedClass(Kit.GameMode);
	if (!CharacterClass || !GameModeClass || !CharacterClass->IsChildOf(ACharacter::StaticClass()))
	{
		AddError(TEXT("The player kit did not build."));
		return false;
	}
	TestTrue(TEXT("the game mode spawns the character"), GameModeClass->GetDefaultObject<AGameModeBase>()->DefaultPawnClass == CharacterClass);

	// The level uses the game mode: set through "WorldSettings", as the skill says, then put back.
	if (UWorld* EditorWorld = CursorToolHelpers::GetEditorWorld())
	{
		AWorldSettings* Settings = EditorWorld->GetWorldSettings();
		const TSubclassOf<AGameModeBase> Before = Settings->DefaultGameMode;
		ON_SCOPE_EXIT { Settings->DefaultGameMode = Before; };
		Tool(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"WorldSettings\",\"properties\":{\"DefaultGameMode\":\"{0}\"}}"), Kit.GameMode));
		TestTrue(TEXT("the level's game mode override is the new game mode"), Settings->DefaultGameMode == GameModeClass);
	}

	// The kit's invisible wall: not drawn, still solid.
	{
		FScopedLevelActors Placed;
		const FString Wall = Placed.Add(TEXT("Wall_NearEdge"));
		Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[0,250,6000],\"scale\":[10,0.2,3],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), CubePath, Wall));
		Tool(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}.StaticMeshComponent0\",\"properties\":{\"bVisible\":false}}"), Wall));
		FString Error;
		const AStaticMeshActor* Actor = Cast<AStaticMeshActor>(CursorToolHelpers::FindActor(Wall, Error));
		if (TestNotNull(TEXT("the wall was placed"), Actor))
		{
			const UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
			TestFalse(TEXT("the wall is not drawn"), Mesh->IsVisible());
			TestTrue(TEXT("the wall still collides"), Mesh->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics);
			TestTrue(TEXT("the wall still blocks pawns"), Mesh->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block);
		}
	}

	// Projects created in UE 5 use Enhanced Input's player input class; older ones, and this project after the
	// session's agent changed it, use the plain one. Reading keys from the controller has to work under both.
	for (const TCHAR* InputClassPath : { TEXT("/Script/Engine.PlayerInput"), TEXT("/Script/EnhancedInput.EnhancedPlayerInput") })
	{
		UClass* InputClass = LoadClass<UPlayerInput>(nullptr, InputClassPath);
		if (!InputClass)
		{
			Note(FString::Printf(TEXT("controls: %s is not available in this project, skipped"), InputClassPath));
			continue;
		}
		const FString With = FString::Printf(TEXT("[%s] "), *InputClass->GetName());

		FPlayWorld Play;
		Play.Cube(FVector(0, 0, 0), FVector(20000, 1000, 100));
		ACharacter* Hero = Play.World->SpawnActor<ACharacter>(CharacterClass, FVector(0, 0, 92), FRotator::ZeroRotator);
		APlayerController* Controller = Play.World->SpawnActor<APlayerController>();
		Controller->PlayerInput = NewObject<UPlayerInput>(Controller, InputClass);
		Controller->InitInputSystem();
		Controller->Possess(Hero);
		Play.BeginPlay();
		Play.Tick(0.5f);
		TestTrue(With + TEXT("the character stands on the floor"), Hero->GetCharacterMovement()->IsMovingOnGround());

		auto Press = [&](const FKey& Key) { Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Pressed, 1.0f)); };
		auto Release = [&](const FKey& Key) { Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.0f)); };
		auto Stick = [&](const FKey& Key, float Value) { Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Axis, Value)); };
		auto HeldFor = [&](const FKey& Key, float Seconds)
		{
			const double From = Hero->GetActorLocation().X;
			Press(Key);
			Play.Tick(Seconds);
			Release(Key);
			const double Moved = Hero->GetActorLocation().X - From;
			Play.Tick(0.6f); // come to rest
			return Moved;
		};

		// Nothing pressed: nothing happens.
		const FVector Rest = Hero->GetActorLocation();
		Play.Tick(0.5f);
		TestTrue(With + TEXT("no keys, no movement"), Hero->GetActorLocation().Equals(Rest, 1.0));

		const double ByD = HeldFor(EKeys::D, 1.0f);
		const double ByRight = HeldFor(EKeys::Right, 1.0f);
		const double ByA = HeldFor(EKeys::A, 1.0f);
		const double ByLeft = HeldFor(EKeys::Left, 1.0f);
		Note(FString::Printf(TEXT("controls %s: one second of D moves %.0f cm, Right %.0f, A %.0f, Left %.0f"), *InputClass->GetName(), ByD, ByRight, ByA, ByLeft));
		TestTrue(With + TEXT("D runs toward +X"), ByD > 400.0);
		TestTrue(With + TEXT("Right arrow runs toward +X"), ByRight > 400.0);
		TestTrue(With + TEXT("A runs toward -X"), ByA < -400.0);
		TestTrue(With + TEXT("Left arrow runs toward -X"), ByLeft < -400.0);
		TestTrue(With + TEXT("the character faces the way it last ran (-X)"), FMath::Abs(FMath::FindDeltaAngleDegrees(Hero->GetActorRotation().Yaw, 180.0)) < 10.0);

		// A key the graph does not read does nothing.
		TestTrue(With + TEXT("W is not bound"), FMath::Abs(HeldFor(EKeys::W, 0.5f)) < 1.0);

		// The stick: a gamepad reports its axes every frame.
		double From = Hero->GetActorLocation().X;
		Play.Tick(1.0f, [&] { Stick(EKeys::Gamepad_LeftX, 1.0f); });
		const double ByStickRight = Hero->GetActorLocation().X - From;
		Play.Tick(0.6f, [&] { Stick(EKeys::Gamepad_LeftX, 0.0f); });
		From = Hero->GetActorLocation().X;
		Play.Tick(1.0f, [&] { Stick(EKeys::Gamepad_LeftX, -1.0f); });
		const double ByStickLeft = Hero->GetActorLocation().X - From;
		Play.Tick(0.6f, [&] { Stick(EKeys::Gamepad_LeftX, 0.0f); });
		Note(FString::Printf(TEXT("controls %s: one second of left stick right moves %.0f cm, left %.0f"), *InputClass->GetName(), ByStickRight, ByStickLeft));
		TestTrue(With + TEXT("stick right runs toward +X"), ByStickRight > 400.0);
		TestTrue(With + TEXT("stick left runs toward -X"), ByStickLeft < -400.0);

		// Jump: Space, and the gamepad's bottom face button (A on an Xbox pad). Held keys do not jump again.
		for (const FKey& JumpKey : { EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom })
		{
			const double Ground = FPlayWorld::FeetZ(Hero);
			double Apex = Ground;
			int32 TakeOffs = 0;
			bool bWasOnGround = true;
			Press(JumpKey);
			Play.Tick(2.5f, [&]
			{
				Apex = FMath::Max(Apex, FPlayWorld::FeetZ(Hero));
				const bool bOnGround = Hero->GetCharacterMovement()->IsMovingOnGround();
				TakeOffs += (bWasOnGround && !bOnGround) ? 1 : 0;
				bWasOnGround = bOnGround;
			});
			Release(JumpKey);
			Play.Tick(0.2f);
			Note(FString::Printf(TEXT("controls %s: %s jumps %.1f cm, %d take-off(s) while held"), *InputClass->GetName(), *JumpKey.ToString(), Apex - Ground, TakeOffs));
			TestTrue(With + JumpKey.ToString() + TEXT(" jumps the default 90 cm"), FMath::IsNearlyEqual(Apex - Ground, 90.0, 5.0));
			TestEqual(With + JumpKey.ToString() + TEXT(" held jumps once"), TakeOffs, 1);
		}

		// Side-on: the camera is 900 cm out on +Y looking back at the character, whichever way it faces.
		const UCameraComponent* Camera = Hero->FindComponentByClass<UCameraComponent>();
		if (TestNotNull(With + TEXT("the character has a camera"), Camera))
		{
			const FVector Offset = Camera->GetComponentLocation() - Hero->GetActorLocation();
			Note(FString::Printf(TEXT("controls %s: camera offset %s, looking along %s"), *InputClass->GetName(), *Offset.ToCompactString(), *Camera->GetForwardVector().ToCompactString()));
			TestTrue(With + TEXT("the camera is 900 cm out on +Y"), Offset.Equals(FVector(0, 900, 0), 5.0));
			TestTrue(With + TEXT("the camera looks along -Y"), Camera->GetForwardVector().Equals(FVector(0, -1, 0), 0.02));
		}

		// The plane lock: a push across the course goes nowhere.
		const double YBefore = Hero->GetActorLocation().Y;
		Play.Tick(0.5f, [&] { Hero->AddMovementInput(FVector(0, 1, 0)); });
		TestTrue(With + TEXT("the character cannot leave the course's plane"), FMath::IsNearlyEqual(Hero->GetActorLocation().Y, YBefore, 1.0));

		// The body block is for looks only.
		const UStaticMeshComponent* Body = Hero->FindComponentByClass<UStaticMeshComponent>();
		if (TestNotNull(With + TEXT("the character has a body mesh"), Body))
		{
			TestTrue(With + TEXT("the body shows the cube"), Body->GetStaticMesh() != nullptr);
			TestTrue(With + TEXT("the body does not collide"), Body->GetCollisionEnabled() == ECollisionEnabled::NoCollision);
		}
	}
	return true;
}

namespace CursorLevelDesignTest
{
	/**
	 * Sprint for the player character, exactly as Skills/unreal-level-design/player.md tells the agent to add it:
	 * one batch, in which later calls name the nodes that earlier calls made.
	 */
	FCursorToolResult BuildSprint(FAutomationTestBase& Test, const FString& P)
	{
		return Tool(Test, TEXT("batch"), Args(TEXT("{\"brief\":true,\"calls\":[")
			TEXT("{\"tool\":\"blueprint_add_node\",\"as\":\"shift\",\"args\":{\"blueprint\":\"{0}\",\"kind\":\"input_key\",\"key\":\"LeftShift\"}},")
			TEXT("{\"tool\":\"blueprint_add_node\",\"as\":\"move\",\"args\":{\"blueprint\":\"{0}\",\"kind\":\"variable_get\",\"name\":\"CharacterMovement\"}},")
			TEXT("{\"tool\":\"blueprint_add_node\",\"as\":\"fast\",\"args\":{\"blueprint\":\"{0}\",\"kind\":\"variable_set\",\"name\":\"MaxWalkSpeed\",\"class\":\"CharacterMovementComponent\"}},")
			TEXT("{\"tool\":\"blueprint_add_node\",\"as\":\"slow\",\"args\":{\"blueprint\":\"{0}\",\"kind\":\"variable_set\",\"name\":\"MaxWalkSpeed\",\"class\":\"CharacterMovementComponent\"}},")
			TEXT("{\"tool\":\"blueprint_connect_pins\",\"args\":{\"blueprint\":\"{0}\",\"from_node\":\"$shift.id\",\"from_pin\":\"Pressed\",\"to_node\":\"$fast.id\",\"to_pin\":\"execute\"}},")
			TEXT("{\"tool\":\"blueprint_connect_pins\",\"args\":{\"blueprint\":\"{0}\",\"from_node\":\"$shift.id\",\"from_pin\":\"Released\",\"to_node\":\"$slow.id\",\"to_pin\":\"execute\"}},")
			TEXT("{\"tool\":\"blueprint_connect_pins\",\"args\":{\"blueprint\":\"{0}\",\"from_node\":\"$move.id\",\"from_pin\":\"CharacterMovement\",\"to_node\":\"$fast.id\",\"to_pin\":\"self\"}},")
			TEXT("{\"tool\":\"blueprint_connect_pins\",\"args\":{\"blueprint\":\"{0}\",\"from_node\":\"$move.id\",\"from_pin\":\"CharacterMovement\",\"to_node\":\"$slow.id\",\"to_pin\":\"self\"}},")
			TEXT("{\"tool\":\"blueprint_set_pin_default\",\"args\":{\"blueprint\":\"{0}\",\"node\":\"$fast.id\",\"pin\":\"MaxWalkSpeed\",\"value\":\"1200\"}},")
			TEXT("{\"tool\":\"blueprint_set_pin_default\",\"args\":{\"blueprint\":\"{0}\",\"node\":\"$slow.id\",\"pin\":\"MaxWalkSpeed\",\"value\":\"600\"}},")
			TEXT("{\"tool\":\"blueprint_compile\",\"args\":{\"blueprint\":\"{0}\"}}]}"), P));
	}

	bool HasPin(const FCursorToolResult& Node, const TCHAR* Name)
	{
		if (!Node.Structured.IsValid())
		{
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Pin : Node.Structured->GetArrayField(TEXT("pins")))
		{
			if (Pin->AsObject()->GetStringField(TEXT("name")) == Name)
			{
				return true;
			}
		}
		return false;
	}
}

// The node kinds added after the first real session could not build sprint, a camera that moves, or a loop:
// key events, properties of another object, casts and the standard macros. Built through the tools, then played.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorLevelDesignNodeKindsTest, "CursorUnreal.LevelDesign.NodeKinds", CursorTest::Flags)
bool FCursorLevelDesignNodeKindsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedTestAssets Assets;
	const FPlayerKit Kit = BuildPlayer(*this, Assets);
	const FString P = Kit.Character;

	// --- Sprint: a key event writing a property of the movement component.
	const FCursorToolResult Sprint = BuildSprint(*this, P);
	TestFalse(TEXT("the sprint batch runs: ") + Sprint.Text, Sprint.bIsError);
	TestTrue(TEXT("a brief batch does not echo the node descriptions"), Sprint.Text.Len() < 1500);
	Compile(*this, P);

	// The same key twice is the same node; an axis is not a key event; a property Blueprints cannot write says what to use.
	FCursorToolResult R = Tool(*this, TEXT("blueprint_add_node"), Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"input_key\",\"key\":\"left shift\"}"), P));
	TestTrue(TEXT("a second event for the same key returns the first"), R.Structured.IsValid() && R.Structured->HasField(TEXT("already_existed")));
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"input_key\",\"key\":\"Gamepad_LeftX\"}"), P));
	TestTrue(TEXT("an axis is refused as a key event: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("GetInputAnalogKeyState")));
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"variable_set\",\"name\":\"WalkSpeed\",\"class\":\"CharacterMovementComponent\"}"), P));
	TestTrue(TEXT("an unknown property names the close ones: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("MaxWalkSpeed")));
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"variable_set\",\"name\":\"RelativeLocation\",\"class\":\"SceneComponent\"}"), P));
	TestTrue(TEXT("a read-only property names its setter: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("SetRelativeLocation")));
	R = Tool(*this, TEXT("blueprint_add_node"), Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"variable_get\",\"name\":\"OrientRotationToMovement\",\"class\":\"CharacterMovementComponent\"}"), P));
	TestTrue(TEXT("a bool can be named without its b: ") + R.Text, HasPin(R, TEXT("bOrientRotationToMovement")) && HasPin(R, TEXT("self")));
	Tool(*this, TEXT("blueprint_remove_node"), Args(TEXT("{\"blueprint\":\"{0}\",\"node\":\"{1}\"}"), P, R.Structured.IsValid() ? R.Structured->GetStringField(TEXT("id")) : FString()));
	// A class this Blueprint already is means its own variable, with no Target pin to wire.
	R = Tool(*this, TEXT("blueprint_add_node"), Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"variable_get\",\"name\":\"bUseControllerRotationYaw\",\"class\":\"Pawn\"}"), P));
	TestTrue(TEXT("its own class gives a plain variable node"), HasPin(R, TEXT("bUseControllerRotationYaw")) && !HasPin(R, TEXT("self")));
	Tool(*this, TEXT("blueprint_remove_node"), Args(TEXT("{\"blueprint\":\"{0}\",\"node\":\"{1}\"}"), P, R.Structured.IsValid() ? R.Structured->GetStringField(TEXT("id")) : FString()));
	Compile(*this, P);

	// --- A cast and a loop, on an actor that moves itself so the test can see that they ran:
	// BeginPlay -> ForLoop 0..2 { rise 10 cm } -> Completed -> cast the player's pawn to Character -> step 100 cm along X.
	const FString A = Assets.Add(TEXT("BP_NodeKinds"));
	Tool(*this, TEXT("blueprint_create"), Args(TEXT("{\"path\":\"{0}\",\"parent_class\":\"Actor\"}"), A));
	const FCursorToolResult Graph = Tool(*this, TEXT("batch"), Args(TEXT("{\"calls\":[")
		TEXT("{\"tool\":\"blueprint_add_node\",\"as\":\"begin\",\"args\":{\"blueprint\":\"{0}\",\"kind\":\"event\",\"name\":\"BeginPlay\"}},")
		TEXT("{\"tool\":\"blueprint_add_node\",\"as\":\"loop\",\"args\":{\"blueprint\":\"{0}\",\"kind\":\"macro\",\"name\":\"ForLoop\"}},")
		TEXT("{\"tool\":\"blueprint_add_node\",\"as\":\"rise\",\"args\":{\"blueprint\":\"{0}\",\"kind\":\"call_function\",\"function\":\"AddActorWorldOffset\",\"class\":\"Actor\"}},")
		TEXT("{\"tool\":\"blueprint_add_node\",\"as\":\"pawn\",\"args\":{\"blueprint\":\"{0}\",\"kind\":\"call_function\",\"function\":\"GetPlayerPawn\",\"class\":\"GameplayStatics\"}},")
		TEXT("{\"tool\":\"blueprint_add_node\",\"as\":\"cast\",\"args\":{\"blueprint\":\"{0}\",\"kind\":\"cast\",\"class\":\"Character\"}},")
		TEXT("{\"tool\":\"blueprint_add_node\",\"as\":\"step\",\"args\":{\"blueprint\":\"{0}\",\"kind\":\"call_function\",\"function\":\"AddActorWorldOffset\",\"class\":\"Actor\"}},")
		TEXT("{\"tool\":\"blueprint_set_pin_default\",\"args\":{\"blueprint\":\"{0}\",\"node\":\"$loop.id\",\"pin\":\"LastIndex\",\"value\":\"2\"}},")
		TEXT("{\"tool\":\"blueprint_set_pin_default\",\"args\":{\"blueprint\":\"{0}\",\"node\":\"$rise.id\",\"pin\":\"DeltaLocation\",\"value\":\"(X=0,Y=0,Z=10)\"}},")
		TEXT("{\"tool\":\"blueprint_set_pin_default\",\"args\":{\"blueprint\":\"{0}\",\"node\":\"$step.id\",\"pin\":\"DeltaLocation\",\"value\":\"(X=100,Y=0,Z=0)\"}},")
		TEXT("{\"tool\":\"blueprint_connect_pins\",\"args\":{\"blueprint\":\"{0}\",\"from_node\":\"$begin.id\",\"from_pin\":\"then\",\"to_node\":\"$loop.id\",\"to_pin\":\"execute\"}},")
		TEXT("{\"tool\":\"blueprint_connect_pins\",\"args\":{\"blueprint\":\"{0}\",\"from_node\":\"$loop.id\",\"from_pin\":\"LoopBody\",\"to_node\":\"$rise.id\",\"to_pin\":\"execute\"}},")
		TEXT("{\"tool\":\"blueprint_connect_pins\",\"args\":{\"blueprint\":\"{0}\",\"from_node\":\"$loop.id\",\"from_pin\":\"Completed\",\"to_node\":\"$cast.id\",\"to_pin\":\"execute\"}},")
		TEXT("{\"tool\":\"blueprint_connect_pins\",\"args\":{\"blueprint\":\"{0}\",\"from_node\":\"$pawn.id\",\"from_pin\":\"ReturnValue\",\"to_node\":\"$cast.id\",\"to_pin\":\"Object\"}},")
		TEXT("{\"tool\":\"blueprint_connect_pins\",\"args\":{\"blueprint\":\"{0}\",\"from_node\":\"$cast.id\",\"from_pin\":\"then\",\"to_node\":\"$step.id\",\"to_pin\":\"execute\"}},")
		TEXT("{\"tool\":\"blueprint_compile\",\"args\":{\"blueprint\":\"{0}\"}}]}"), A));
	TestFalse(TEXT("the cast-and-loop batch runs: ") + Graph.Text, Graph.bIsError);
	Compile(*this, A);
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"macro\",\"name\":\"ForEver\"}"), A));
	TestTrue(TEXT("an unknown macro lists the ones there are: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("ForEachLoop")) && R.Text.Contains(TEXT("DoOnce")));

	UClass* ActorClass = GeneratedClass(A);
	UClass* CharacterClass = GeneratedClass(P);
	if (!ActorClass || !CharacterClass)
	{
		AddError(TEXT("The Blueprints did not build."));
		return false;
	}

	{
		FPlayWorld Play;
		Play.Cube(FVector(0, 0, 0), FVector(2000, 2000, 100));
		Play.Player(FVector(500, 0, 0));
		AActor* WithPlayer = Play.World->SpawnActor<AActor>(ActorClass, FVector(0, 0, 500), FRotator::ZeroRotator);
		Play.BeginPlay();
		Play.Tick(0.1f);
		Note(FString::Printf(TEXT("node kinds: with a Character as the player the actor moved to %s"), *WithPlayer->GetActorLocation().ToCompactString()));
		TestTrue(TEXT("the loop ran three times and the cast succeeded"), WithPlayer->GetActorLocation().Equals(FVector(100, 0, 530), 0.5));
	}
	{
		FPlayWorld Play;
		AActor* Alone = Play.World->SpawnActor<AActor>(ActorClass, FVector(0, 0, 500), FRotator::ZeroRotator);
		Play.BeginPlay();
		Play.Tick(0.1f);
		TestTrue(TEXT("with no player the cast fails and only the loop moves the actor"), Alone->GetActorLocation().Equals(FVector(0, 0, 530), 0.5));
	}

	// --- Sprint, played, under both kinds of player input.
	for (const TCHAR* InputClassPath : { TEXT("/Script/Engine.PlayerInput"), TEXT("/Script/EnhancedInput.EnhancedPlayerInput") })
	{
		UClass* InputClass = LoadClass<UPlayerInput>(nullptr, InputClassPath);
		if (!InputClass)
		{
			continue;
		}
		const FString With = FString::Printf(TEXT("[%s] "), *InputClass->GetName());
		FPlayWorld Play;
		Play.Cube(FVector(0, 0, 0), FVector(40000, 1000, 100));
		ACharacter* Hero = Play.World->SpawnActor<ACharacter>(CharacterClass, FVector(0, 0, 92), FRotator::ZeroRotator);
		APlayerController* Controller = Play.World->SpawnActor<APlayerController>();
		Controller->PlayerInput = NewObject<UPlayerInput>(Controller, InputClass);
		Controller->InitInputSystem();
		Controller->Possess(Hero);
		Play.BeginPlay();
		Play.Tick(0.5f);

		auto Press = [&](const FKey& Key) { Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Pressed, 1.0f)); };
		auto Release = [&](const FKey& Key) { Controller->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.0f)); };
		auto RunFor = [&](float Seconds)
		{
			const double From = Hero->GetActorLocation().X;
			Press(EKeys::D);
			Play.Tick(Seconds);
			Release(EKeys::D);
			const double Moved = Hero->GetActorLocation().X - From;
			Play.Tick(0.8f);
			return Moved;
		};
		const double Walk = RunFor(1.0f);
		Press(EKeys::LeftShift);
		Play.Tick(0.1f);
		const double SprintSpeed = Hero->GetCharacterMovement()->MaxWalkSpeed;
		const double Sprinted = RunFor(1.0f);
		Release(EKeys::LeftShift);
		Play.Tick(0.1f);
		const double WalkAgain = RunFor(1.0f);
		Note(FString::Printf(TEXT("sprint %s: one second walking %.0f cm, with LeftShift held %.0f cm (MaxWalkSpeed %.0f), after letting go %.0f cm"),
			*InputClass->GetName(), Walk, Sprinted, SprintSpeed, WalkAgain));
		TestTrue(With + TEXT("LeftShift pressed sets the faster speed"), FMath::IsNearlyEqual(SprintSpeed, 1200.0, 1.0));
		TestTrue(With + TEXT("sprinting covers much more ground"), Sprinted > Walk * 1.4);
		TestTrue(With + TEXT("letting go of LeftShift walks again"), FMath::IsNearlyEqual(WalkAgain, Walk, 20.0));
	}
	return true;
}

// Volumes and brushes placed by actor_spawn have a shape of the size asked for, and can be given a new one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorLevelDesignVolumesTest, "CursorUnreal.LevelDesign.Volumes", CursorTest::Flags)
bool FCursorLevelDesignVolumesTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedLevelActors Placed;
	auto SizeOf = [&](const FString& Label)
	{
		FString Error;
		const AActor* Actor = CursorToolHelpers::FindActor(Label, Error);
		return Actor ? Actor->GetComponentsBoundingBox(true).GetSize() : FVector::ZeroVector;
	};

	const FString Wall = Placed.Add(TEXT("Blocker"));
	FCursorToolResult R = Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"BlockingVolume\",\"location\":[0,300,7000],\"size\":[1000,40,600],\"label\":\"{0}\",\"folder\":\"CursorUnrealTests\"}"), Wall));
	TestTrue(TEXT("a blocking volume is the size asked for: ") + SizeOf(Wall).ToCompactString(), SizeOf(Wall).Equals(FVector(1000, 40, 600), 1.0));
	TestTrue(TEXT("the result reports the size and no warning: ") + R.Text, R.Text.Contains(TEXT("\"size\":[1000,40,600]")) && !R.Text.Contains(TEXT("no shape")));
	{
		FString Error;
		const AActor* Actor = CursorToolHelpers::FindActor(Wall, Error);
		const UPrimitiveComponent* Shape = Actor ? Cast<UPrimitiveComponent>(Actor->GetRootComponent()) : nullptr;
		if (TestNotNull(TEXT("the volume has a shape component"), Shape))
		{
			TestTrue(TEXT("the blocking volume blocks pawns"), Shape->GetCollisionResponseToChannel(ECC_Pawn) == ECR_Block);
			TestTrue(TEXT("the volume's centre is where it was placed"), Actor->GetComponentsBoundingBox(true).GetCenter().Equals(FVector(0, 300, 7000), 1.0));
		}
	}

	R = Tool(*this, TEXT("actor_update"), Args(TEXT("{\"name\":\"{0}\",\"size\":[2000,40,300]}"), Wall));
	TestTrue(TEXT("a volume can be given a new size: ") + SizeOf(Wall).ToCompactString(), SizeOf(Wall).Equals(FVector(2000, 40, 300), 1.0));

	const FString Pit = Placed.Add(TEXT("KillZ"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"KillZVolume\",\"location\":[0,0,6500],\"label\":\"{0}\",\"folder\":\"CursorUnrealTests\"}"), Pit));
	TestTrue(TEXT("a volume placed with no size is a 200 cm cube, not a point: ") + SizeOf(Pit).ToCompactString(), SizeOf(Pit).Equals(FVector(200, 200, 200), 1.0));

	const FString Block = Placed.Add(TEXT("GeometryBrush"));
	R = Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"Brush\",\"location\":[0,-600,7000],\"size\":[300,300,100],\"label\":\"{0}\",\"folder\":\"CursorUnrealTests\"}"), Block));
	TestTrue(TEXT("a brush is the size asked for: ") + SizeOf(Block).ToCompactString(), SizeOf(Block).Equals(FVector(300, 300, 100), 1.0));
	TestTrue(TEXT("the result says a brush cannot move in play: ") + R.Text, R.Text.Contains(TEXT("cannot move")));

	const FString Cube = Placed.Add(TEXT("NotABrush"));
	R = CursorTest::Call(TEXT("actor_spawn"), *Args(TEXT("{\"class\":\"{0}\",\"location\":[0,0,7500],\"size\":[300,300,100],\"label\":\"{1}\"}"), CubePath, Cube));
	TestTrue(TEXT("size on a mesh actor is refused and points at scale: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("scale")));
	FString Error;
	TestNull(TEXT("the refused actor was not left in the level"), CursorToolHelpers::FindActor(Cube, Error));
	R = CursorTest::Call(TEXT("actor_spawn"), TEXT("{\"class\":\"TriggerVolume\",\"location\":[0,0,7500],\"size\":[0,100,100]}"));
	TestTrue(TEXT("a zero size is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("at least 1 cm")));
	return true;
}

namespace CursorLevelDesignTest
{
	/** Everything a play test has to keep alive, and put back, across the frames it takes. */
	struct FPlayTestState
	{
		CursorTest::FScopedPolicy Policy{ NAME_None, ECursorApprovalPolicy::AllowAll };
		FScopedTestAssets Assets;
		FScopedLevelActors Placed;
		FPlayerKit Kit;
		TSubclassOf<AGameModeBase> GameModeBefore;
		FCursorToolResult Result;
		bool bDone = false;
		double StartedAt = 0.0;

		~FPlayTestState()
		{
			if (UWorld* EditorWorld = CursorToolHelpers::GetEditorWorld())
			{
				EditorWorld->GetWorldSettings()->DefaultGameMode = GameModeBefore;
			}
		}
	};
}

// play_test presses Play, holds the keys it is told to, and reports what the player did: the check the agent could
// not make in the session where it said three times that controls worked.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorLevelDesignPlayToolTest, "CursorUnreal.LevelDesign.PlayTool", CursorTest::Flags)
bool FCursorLevelDesignPlayToolTest::RunTest(const FString&)
{
	// Bad scripts are refused before Play starts.
	{
		CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
		FCursorToolResult R = CursorTest::Call(TEXT("play_test"), TEXT("{\"steps\":[{\"keys\":[\"Jump\"]}]}"));
		TestTrue(TEXT("a name that is not a key is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("not a key name")));
		R = CursorTest::Call(TEXT("play_test"), TEXT("{\"steps\":[{\"keys\":[\"Gamepad_LeftX\"]}]}"));
		TestTrue(TEXT("an axis under keys is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("axes")));
		R = CursorTest::Call(TEXT("play_test"), TEXT("{\"steps\":[{\"axes\":{\"D\":1}}]}"));
		TestTrue(TEXT("a key under axes is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("Buttons and keys go under 'keys'")));
		R = CursorTest::Call(TEXT("play_test"), TEXT("{\"steps\":[{\"seconds\":50},{\"seconds\":50}]}"));
		TestTrue(TEXT("a test longer than a minute is refused: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("at most")));
		R = CursorTest::Call(TEXT("play_test"), TEXT("{\"watch\":[\"NoSuchActor_123\"]}"));
		TestTrue(TEXT("watching an actor that is not there is refused"), R.bIsError);
		TestFalse(TEXT("none of those started Play"), GEditor->IsPlaySessionInProgress());
	}

	UWorld* EditorWorld = CursorToolHelpers::GetEditorWorld();
	if (!TestNotNull(TEXT("a level is open"), EditorWorld))
	{
		return false;
	}
	const TSharedRef<FPlayTestState> State = MakeShared<FPlayTestState>();
	State->GameModeBefore = EditorWorld->GetWorldSettings()->DefaultGameMode;
	State->Kit = BuildPlayer(*this, State->Assets);
	BuildSprint(*this, State->Kit.Character);
	Tool(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"WorldSettings\",\"properties\":{\"DefaultGameMode\":\"{0}\"}}"), State->Kit.GameMode));

	// A floor far above the template level's landscape, and a crate on it to watch.
	const FString Floor = State->Placed.Add(TEXT("PlayFloor"));
	const FString Crate = State->Placed.Add(TEXT("PlayCrate"));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[0,0,8950],\"scale\":[400,10,1],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), CubePath, Floor));
	Tool(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"{0}\",\"location\":[-500,0,9050],\"label\":\"{1}\",\"folder\":\"CursorUnrealTests\"}"), CubePath, Crate));

	const bool bFixedBefore = FApp::UseFixedTimeStep();
	State->StartedAt = FPlatformTime::Seconds();
	FCursorToolContext Ctx;
	Ctx.Caller = FCursorToolRegistry::CallerNative;
	Ctx.SessionId = TEXT("test");
	FCursorToolRegistry::Get().Invoke(TEXT("play_test"), CursorJson::Parse(Args(TEXT("{\"start\":[0,0,9100],\"watch\":[\"{0}\"],\"steps\":[")
		TEXT("{\"keys\":[\"D\"],\"seconds\":1.0},")
		TEXT("{\"seconds\":0.8},")
		TEXT("{\"keys\":[\"Space\"],\"seconds\":0.1},")
		TEXT("{\"seconds\":1.4},")
		TEXT("{\"keys\":[\"LeftShift\",\"D\"],\"seconds\":1.0},")
		TEXT("{\"seconds\":0.8},")
		TEXT("{\"axes\":{\"Gamepad_LeftX\":-1.0},\"seconds\":1.0}]}"), Crate)), Ctx,
		[State](const FCursorToolResult& Result) { State->Result = Result; State->bDone = true; });
	TestFalse(TEXT("the test does not finish in the call that starts it"), State->bDone);

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, State, bFixedBefore]()
	{
		if (!State->bDone)
		{
			if (FPlatformTime::Seconds() - State->StartedAt > 240.0)
			{
				AddError(TEXT("play_test did not finish within four minutes."));
				return true;
			}
			return false;
		}
		const FCursorToolResult& R = State->Result;
		Note(FString::Printf(TEXT("play_test took %.1f s of real time and returned: %s"), FPlatformTime::Seconds() - State->StartedAt, *R.Text));
		TestFalse(TEXT("play_test succeeds: ") + R.Text, R.bIsError);
		TestFalse(TEXT("Play has stopped again"), GEditor->IsPlaySessionInProgress());
		TestEqual(TEXT("the editor's time step setting is put back"), FApp::UseFixedTimeStep(), bFixedBefore);
		if (R.bIsError || !R.Structured.IsValid())
		{
			return true;
		}
		const FJsonObject& Out = *R.Structured;
		TestTrue(TEXT("the level's game mode was used"), Out.GetStringField(TEXT("game_mode")).StartsWith(TEXT("BP_PlatformerGameMode")));
		TestTrue(TEXT("the player is the character from the kit"), Out.GetStringField(TEXT("pawn")).StartsWith(TEXT("BP_PlatformerCharacter")));
		const TArray<TSharedPtr<FJsonValue>>& Steps = Out.GetArrayField(TEXT("steps"));
		if (!TestEqual(TEXT("every step ran"), Steps.Num(), 7))
		{
			return true;
		}
		auto Moved = [&](int32 Index, int32 Axis) { return Steps[Index]->AsObject()->GetArrayField(TEXT("moved"))[Axis]->AsNumber(); };
		auto Field = [&](int32 Index, const TCHAR* Name) { return Steps[Index]->AsObject()->GetNumberField(Name); };
		TestTrue(TEXT("the player started on the floor where it was told to"), FMath::IsNearlyEqual(Steps[0]->AsObject()->GetArrayField(TEXT("from"))[2]->AsNumber(), 9092.0, 5.0));
		TestTrue(TEXT("one second of D runs about 5 m along +X"), Moved(0, 0) > 400.0 && Moved(0, 0) < 650.0);
		TestTrue(TEXT("with no keys the player comes to rest"), FMath::Abs(Field(1, TEXT("speed_at_end"))) < 1.0);
		const double Rise = FMath::Max(Field(2, TEXT("highest_rise")), Field(3, TEXT("highest_rise")) + Moved(2, 2));
		TestTrue(FString::Printf(TEXT("a tap of Space jumps about 90 cm (%.1f)"), Rise), FMath::IsNearlyEqual(Rise, 90.0, 6.0));
		TestTrue(TEXT("the player is back on the ground after the jump"), Steps[3]->AsObject()->GetBoolField(TEXT("on_ground_at_end")));
		TestTrue(TEXT("LeftShift and D together sprint"), Moved(4, 0) > Moved(0, 0) * 1.4);
		TestTrue(TEXT("the stick pushed left runs along -X"), Moved(6, 0) < -400.0);
		TestTrue(TEXT("the watched crate is reported where it is"), Steps[0]->AsObject()->GetObjectField(TEXT("watched"))->Values.Num() == 1);
		TestTrue(TEXT("the camera is reported side-on, 900 cm out on +Y"),
			FMath::IsNearlyEqual(Out.GetObjectField(TEXT("camera_at_end"))->GetArrayField(TEXT("offset_from_pawn"))[1]->AsNumber(), 900.0, 10.0));
		return true;
	}));
	return true;
}


#endif // WITH_DEV_AUTOMATION_TESTS
