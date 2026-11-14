#if WITH_DEV_AUTOMATION_TESTS

// Each check here is a tool call that went wrong in a real session with the in-editor agent, replayed.
//
// The pattern behind most of them: the tool accepted something it could not use and reported success
// (a key written as a struct, a value no struct could read), or refused with a message that pointed
// away from the cause ("Object path is empty" for a misnamed argument, "Pins: " with nothing after it
// for a hidden pin). The agent then spent turns, and the user's patience, on the wrong problem.

#include "CursorPackTestSupport.h"
#include "CursorToolHelpers.h"
#include "CursorToolRegistry.h"
#include "CursorUnrealLog.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/ScopeExit.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace CursorSessionFixTest
{
	/** Arguments as JSON text. Built from fields, not written by hand, because the values under test are full of quotes. */
	FString Fields(std::initializer_list<TPair<const TCHAR*, FString>> Pairs)
	{
		const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TPair<const TCHAR*, FString>& Pair : Pairs)
		{
			Object->SetStringField(Pair.Key, Pair.Value);
		}
		return CursorJson::ToString(Object);
	}

	/** A call that is expected to work: a tool error is a test error. */
	FCursorToolResult Works(FAutomationTestBase& Test, const TCHAR* Name, const FString& ArgsJson)
	{
		const FCursorToolResult Result = CursorTest::Call(Name, *ArgsJson);
		if (Result.bIsError)
		{
			Test.AddError(FString::Printf(TEXT("%s %s failed: %s"), Name, *ArgsJson, *Result.Text));
		}
		return Result;
	}

	/** A call that is expected to be refused, with a message that says each of these things. */
	void Refused(FAutomationTestBase& Test, const TCHAR* What, const TCHAR* Name, const FString& ArgsJson, std::initializer_list<const TCHAR*> MustSay)
	{
		const FCursorToolResult Result = CursorTest::Call(Name, *ArgsJson);
		Test.TestTrue(FString::Printf(TEXT("%s: refused"), What), Result.bIsError);
		for (const TCHAR* Text : MustSay)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: the message says '%s'. It was: %s"), What, Text, *Result.Text), Result.Text.Contains(Text));
		}
		UE_LOG(LogCursorUnreal, Display, TEXT("[SessionFix] %s -> %s"), What, *Result.Text.Left(700));
	}

	FString NewNode(FAutomationTestBase& Test, const FString& Blueprint, const TCHAR* Function, const TCHAR* Class)
	{
		const FCursorToolResult Result = Works(Test, TEXT("blueprint_add_node"),
			Fields({ { TEXT("blueprint"), Blueprint }, { TEXT("kind"), TEXT("call_function") }, { TEXT("function"), Function }, { TEXT("class"), Class } }));
		return Result.Structured.IsValid() ? Result.Structured->GetStringField(TEXT("id")) : FString();
	}

	/** What blueprint_get_nodes reports as the literal on a pin: what the agent sees when it checks its own work. */
	FString StoredDefault(const FString& Blueprint, const FString& NodeId, const TCHAR* PinName)
	{
		const FCursorToolResult Result = CursorTest::Call(TEXT("blueprint_get_nodes"), *Fields({ { TEXT("blueprint"), Blueprint } }));
		if (!Result.Structured.IsValid() || !Result.Structured->HasField(TEXT("nodes")))
		{
			return TEXT("<no nodes>");
		}
		for (const TSharedPtr<FJsonValue>& NodeValue : Result.Structured->GetArrayField(TEXT("nodes")))
		{
			const TSharedPtr<FJsonObject> Node = NodeValue->AsObject();
			if (Node->GetStringField(TEXT("id")) != NodeId)
			{
				continue;
			}
			for (const TSharedPtr<FJsonValue>& PinValue : Node->GetArrayField(TEXT("pins")))
			{
				const TSharedPtr<FJsonObject> Pin = PinValue->AsObject();
				if (Pin->GetStringField(TEXT("name")) == PinName)
				{
					FString Default;
					Pin->TryGetStringField(TEXT("default"), Default);
					return Default;
				}
			}
		}
		return TEXT("<no such pin>");
	}

	FString SetPin(const FString& Blueprint, const FString& Node, const TCHAR* Pin, const TCHAR* Value)
	{
		return Fields({ { TEXT("blueprint"), Blueprint }, { TEXT("node"), Node }, { TEXT("pin"), Pin }, { TEXT("value"), Value } });
	}
}

// A literal typed onto a pin is either stored as something the pin's type can read, or refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorPinValuesTest, "CursorUnreal.Tools.BlueprintPinValues", CursorTest::Flags)
bool FCursorPinValuesTest::RunTest(const FString&)
{
	using namespace CursorSessionFixTest;
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	CursorPackTest::FScopedTestAssets Assets;
	const FString P = Assets.Add(TEXT("BP_PinValues"));
	Works(*this, TEXT("blueprint_create"), Fields({ { TEXT("path"), P }, { TEXT("parent_class"), TEXT("PlayerController") } }));

	// --- Key pins. In the session every key was written (KeyName="W"): accepted, reported as set, and read by
	// the engine as no key, so three rounds of "the controls work now" were three rounds of nothing happening.
	const FString Down = NewNode(*this, P, TEXT("IsInputKeyDown"), TEXT("PlayerController"));
	struct FKeyCase
	{
		const TCHAR* Given;
		const TCHAR* Stored;
	};
	const FKeyCase KeyCases[] =
	{
		{ TEXT("D"), TEXT("D") },
		{ TEXT("(KeyName=\"W\")"), TEXT("W") },
		{ TEXT("(KeyName=SpaceBar)"), TEXT("SpaceBar") },
		{ TEXT("\"Right\""), TEXT("Right") },
		{ TEXT("EKeys::LeftShift"), TEXT("LeftShift") },
		{ TEXT("spacebar"), TEXT("SpaceBar") },
		{ TEXT("Space"), TEXT("SpaceBar") },
		{ TEXT("Gamepad_A"), TEXT("Gamepad_FaceButton_Bottom") },
		{ TEXT("Gamepad_RightX"), TEXT("Gamepad_RightX") },
		{ TEXT("MouseX"), TEXT("MouseX") },
	};
	for (const FKeyCase& Case : KeyCases)
	{
		const FCursorToolResult Result = Works(*this, TEXT("blueprint_set_pin_default"), SetPin(P, Down, TEXT("Key"), Case.Given));
		TestEqual(FString::Printf(TEXT("key written as %s is stored as the engine's name"), Case.Given), StoredDefault(P, Down, TEXT("Key")), FString(Case.Stored));
		TestTrue(FString::Printf(TEXT("the result names the stored key %s: %s"), Case.Stored, *Result.Text), Result.Text.Contains(FString::Printf(TEXT("'%s'"), Case.Stored)));
	}
	Refused(*this, TEXT("a word that is not a key"), TEXT("blueprint_set_pin_default"), SetPin(P, Down, TEXT("Key"), TEXT("Jumpy")), { TEXT("not a key name"), TEXT("SpaceBar") });
	Refused(*this, TEXT("part of a key name"), TEXT("blueprint_set_pin_default"), SetPin(P, Down, TEXT("Key"), TEXT("Shoulder")), { TEXT("Close matches"), TEXT("Gamepad_RightShoulder") });
	TestEqual(TEXT("a refused value leaves the last good one in place"), StoredDefault(P, Down, TEXT("Key")), FString(TEXT("MouseX")));

	// --- Any other struct: text the struct cannot read is refused instead of being kept and dropped at compile time.
	const FString ToVector = NewNode(*this, P, TEXT("Conv_Vector2DToVector"), TEXT("KismetMathLibrary"));
	Works(*this, TEXT("blueprint_set_pin_default"), SetPin(P, ToVector, TEXT("InVector2D"), TEXT("(X=3,Y=4)")));
	Refused(*this, TEXT("words on a Vector2D pin"), TEXT("blueprint_set_pin_default"), SetPin(P, ToVector, TEXT("InVector2D"), TEXT("three, four")), { TEXT("cannot be read as a Vector2D"), TEXT("looks like") });
	TestTrue(TEXT("the Vector2D pin keeps the good value"), StoredDefault(P, ToVector, TEXT("InVector2D")).Contains(TEXT("3")));

	// --- Vector and rotator pins take the (X=..) spelling the tool's own description promises.
	const FString Move = NewNode(*this, P, TEXT("SetActorLocationAndRotation"), TEXT("Actor"));
	Works(*this, TEXT("blueprint_set_pin_default"), SetPin(P, Move, TEXT("NewLocation"), TEXT("(X=1,Y=-2,Z=3.5)")));
	TestEqual(TEXT("vector written as (X=..,Y=..,Z=..)"), StoredDefault(P, Move, TEXT("NewLocation")), FString(TEXT("1.000000,-2.000000,3.500000")));
	Works(*this, TEXT("blueprint_set_pin_default"), SetPin(P, Move, TEXT("NewLocation"), TEXT("10,20,30")));
	TestTrue(TEXT("vector written as bare numbers"), StoredDefault(P, Move, TEXT("NewLocation")).StartsWith(TEXT("10")));
	Works(*this, TEXT("blueprint_set_pin_default"), SetPin(P, Move, TEXT("NewRotation"), TEXT("(Pitch=0,Yaw=-90,Roll=0)")));
	TestEqual(TEXT("rotator written as (Pitch=..,Yaw=..,Roll=..)"), StoredDefault(P, Move, TEXT("NewRotation")), FString(TEXT("0.000000,-90.000000,0.000000")));
	Refused(*this, TEXT("words on a vector pin"), TEXT("blueprint_set_pin_default"), SetPin(P, Move, TEXT("NewLocation"), TEXT("up a bit")), { TEXT("not a valid value") });

	// --- Hidden pins: say that the pin is hidden, not that it does not exist next to an empty list.
	const FString Print = NewNode(*this, P, TEXT("PrintString"), TEXT("KismetSystemLibrary"));
	Refused(*this, TEXT("a hidden pin"), TEXT("blueprint_set_pin_default"), SetPin(P, Print, TEXT("WorldContextObject"), TEXT("x")), { TEXT("is hidden"), TEXT("InString") });
	Refused(*this, TEXT("a pin that is not there"), TEXT("blueprint_set_pin_default"), SetPin(P, Print, TEXT("Message"), TEXT("x")), { TEXT("has no input pin 'Message'"), TEXT("InString") });

	// --- Functions that only work through the editor's own node are refused with what to use instead.
	Refused(*this, TEXT("GetInputAxisValue as a plain call"), TEXT("blueprint_add_node"),
		Fields({ { TEXT("blueprint"), P }, { TEXT("kind"), TEXT("call_function") }, { TEXT("function"), TEXT("GetInputAxisValue") }, { TEXT("class"), TEXT("Actor") } }),
		{ TEXT("always returns 0"), TEXT("IsInputKeyDown"), TEXT("GetInputAnalogKeyState") });

	// --- A function that is not there: where it is, what it really is, or what is close.
	Refused(*this, TEXT("right function, wrong class"), TEXT("blueprint_add_node"),
		Fields({ { TEXT("blueprint"), P }, { TEXT("kind"), TEXT("call_function") }, { TEXT("function"), TEXT("GetGameTimeInSeconds") }, { TEXT("class"), TEXT("GameplayStatics") } }),
		{ TEXT("KismetSystemLibrary"), TEXT("Pass that as 'class'") });
	Refused(*this, TEXT("a C++ accessor that is a property in Blueprint"), TEXT("blueprint_add_node"),
		Fields({ { TEXT("blueprint"), P }, { TEXT("kind"), TEXT("call_function") }, { TEXT("function"), TEXT("GetCharacterMovement") }, { TEXT("class"), TEXT("Character") } }),
		{ TEXT("'CharacterMovement' is a property"), TEXT("variable_get") });
	Refused(*this, TEXT("a near miss on the name"), TEXT("blueprint_add_node"),
		Fields({ { TEXT("blueprint"), P }, { TEXT("kind"), TEXT("call_function") }, { TEXT("function"), TEXT("SetLocation") }, { TEXT("class"), TEXT("Actor") } }),
		{ TEXT("Similar functions on Actor"), TEXT("SetActorLocation") });

	const FCursorToolResult Compiled = Works(*this, TEXT("blueprint_compile"), Fields({ { TEXT("blueprint"), P } }));
	TestTrue(FString::Printf(TEXT("the Blueprint still compiles: %s"), *Compiled.Text), Compiled.Structured.IsValid() && Compiled.Structured->GetStringField(TEXT("status")) != TEXT("error"));
	return true;
}

// The ways an agent names an object that should all reach it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAddressingTest, "CursorUnreal.Tools.Addressing", CursorTest::Flags)
bool FCursorAddressingTest::RunTest(const FString&)
{
	using namespace CursorSessionFixTest;
	using CursorPackTest::Args;
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	CursorPackTest::FScopedTestAssets Assets;

	// --- "WorldSettings" is the level's settings actor whatever the level calls it. In the session's level it
	// had another name and the lookup failed, which is the first step of the level-design skill.
	{
		UWorld* World = CursorToolHelpers::GetEditorWorld();
		AWorldSettings* Settings = World ? World->GetWorldSettings() : nullptr;
		if (TestNotNull(TEXT("the editor level has a settings actor"), Settings))
		{
			const FString Original = Settings->GetName();
			const FString Renamed = MakeUniqueObjectName(Settings->GetOuter(), AWorldSettings::StaticClass(), TEXT("WorldInfo")).ToString();
			Settings->Rename(*Renamed, nullptr, REN_DontCreateRedirectors | REN_NonTransactional);
			ON_SCOPE_EXIT { Settings->Rename(*Original, nullptr, REN_DontCreateRedirectors | REN_NonTransactional); };

			const FCursorToolResult Result = Works(*this, TEXT("object_get_properties"), TEXT("{\"object\":\"WorldSettings\",\"names\":[\"DefaultGameMode\",\"KillZ\"]}"));
			TestTrue(FString::Printf(TEXT("'WorldSettings' reaches a settings actor named %s: %s"), *Renamed, *Result.Text),
				Result.Structured.IsValid() && Result.Structured->GetStringField(TEXT("object")) == Settings->GetPathName());
		}
	}

	// --- A component a Blueprint added, addressed by the component's own name.
	{
		const FString P = Assets.Add(TEXT("BP_Addressing"));
		const FString Short = FPackageName::GetShortName(P);
		Works(*this, TEXT("blueprint_create"), Fields({ { TEXT("path"), P }, { TEXT("parent_class"), TEXT("Character") } }));
		Works(*this, TEXT("blueprint_add_component"), Fields({ { TEXT("blueprint"), P }, { TEXT("class"), TEXT("StaticMeshComponent") }, { TEXT("name"), TEXT("Body") } }));
		Works(*this, TEXT("blueprint_compile"), Fields({ { TEXT("blueprint"), P } }));

		FString Error;
		const UObject* Canonical = CursorToolHelpers::ResolveObject(FString::Printf(TEXT("%s.%s_C:Body_GEN_VARIABLE"), *P, *Short), Error);
		TestNotNull(TEXT("the documented form resolves"), Canonical);
		for (const FString& Form : {
			FString::Printf(TEXT("%s.Default__%s_C.Body"), *P, *Short),
			FString::Printf(TEXT("%s.Default__%s_C:Body"), *P, *Short),
			FString::Printf(TEXT("%s.%s_C:Body"), *P, *Short) })
		{
			Error.Reset();
			const UObject* Found = CursorToolHelpers::ResolveObject(Form, Error);
			TestTrue(FString::Printf(TEXT("%s is the Body component's defaults (%s)"), *Form, Found ? *Found->GetPathName() : *Error), Found && Found == Canonical);
		}
		// The form that failed in the session, through the tool, on a component the C++ parent made and on one the Blueprint added.
		Works(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}.Default__{1}_C.Body\",\"properties\":{\"CastShadow\":false}}"), P, Short));
		TestFalse(TEXT("the value reached the Body template"), Canonical && CastChecked<UStaticMeshComponent>(Canonical)->CastShadow);
		Works(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}.Default__{1}_C.CharMoveComp\",\"properties\":{\"MaxWalkSpeed\":450}}"), P, Short));
		const UBlueprint* Blueprint = Cast<UBlueprint>(CursorToolHelpers::LoadAsset(P, Error));
		const ACharacter* Defaults = Blueprint && Blueprint->GeneratedClass ? Cast<ACharacter>(Blueprint->GeneratedClass->GetDefaultObject()) : nullptr;
		TestTrue(TEXT("the value reached the character movement defaults"), Defaults && FMath::IsNearlyEqual(Defaults->GetCharacterMovement()->MaxWalkSpeed, 450.0f));

		// Something that is not there still says so.
		Error.Reset();
		TestNull(TEXT("a component that does not exist is not found"), CursorToolHelpers::ResolveObject(FString::Printf(TEXT("%s.Default__%s_C.Wings"), *P, *Short), Error));
		TestFalse(TEXT("and the reason is given"), Error.IsEmpty());
	}

	// --- A property path that goes through something the object owns (the session tried BrushBuilder.X on a brush).
	{
		UEditorActorSubsystem* Actors = GEditor->GetEditorSubsystem<UEditorActorSubsystem>();
		const FString Hero = CursorPackTest::UniqueName(TEXT("AddrHero"));
		const FString Box = CursorPackTest::UniqueName(TEXT("AddrBox"));
		Works(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"Character\",\"location\":[0,0,5000],\"label\":\"{0}\"}"), Hero));
		Works(*this, TEXT("actor_spawn"), Args(TEXT("{\"class\":\"/Engine/BasicShapes/Cube\",\"location\":[300,0,5000],\"label\":\"{0}\"}"), Box));
		FString Error;
		ACharacter* HeroActor = Cast<ACharacter>(CursorToolHelpers::FindActor(Hero, Error));
		AActor* BoxActor = CursorToolHelpers::FindActor(Box, Error);
		ON_SCOPE_EXIT
		{
			if (HeroActor) { Actors->DestroyActor(HeroActor); }
			if (BoxActor) { Actors->DestroyActor(BoxActor); }
		};

		Works(*this, TEXT("object_set_properties"), Args(TEXT("{\"object\":\"{0}\",\"properties\":{\"CharacterMovement.JumpZVelocity\":777}}"), Hero));
		TestTrue(TEXT("a property of a component, reached through the actor"), HeroActor && FMath::IsNearlyEqual(HeroActor->GetCharacterMovement()->JumpZVelocity, 777.0f));

		// A mesh asset is something the component points at, not something it owns: editing it through an actor
		// would change every actor that uses the mesh.
		Refused(*this, TEXT("a path through a shared asset"), TEXT("object_set_properties"),
			Args(TEXT("{\"object\":\"{0}\",\"properties\":{\"StaticMeshComponent.StaticMesh.LightMapResolution\":128}}"), Box),
			{ TEXT("refers to another object"), TEXT("/Engine/BasicShapes/Cube") });
	}

	// --- A misnamed argument: say which name was wrong and what the names are.
	Refused(*this, TEXT("'blueprint' where the tool takes 'object'"), TEXT("object_get_properties"), TEXT("{\"blueprint\":\"/Game/Anything\"}"),
		{ TEXT("without its required argument 'object'"), TEXT("'blueprint', which it does not take"), TEXT("object (required)"), TEXT("names") });
	// An extra argument on an otherwise complete call is not this check's business.
	Works(*this, TEXT("object_get_properties"), TEXT("{\"object\":\"WorldSettings\",\"names\":[\"KillZ\"],\"why\":\"checking\"}"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
