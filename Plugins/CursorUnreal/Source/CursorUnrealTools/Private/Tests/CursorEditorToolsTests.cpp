#if WITH_DEV_AUTOMATION_TESTS

#include "CursorTestSupport.h"
#include "CursorToolHelpers.h"
#include "CursorUnrealLog.h"
#include "Editor.h"
#include "GameFramework/Actor.h"
#include "IPythonScriptPlugin.h"
#include "Misc/Guid.h"
#include "Subsystems/EditorActorSubsystem.h"

namespace CursorToolsTests
{
	FString UniqueLabel(const TCHAR* Prefix)
	{
		return FString(Prefix) + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Short);
	}

	bool ActorExists(const FString& Label)
	{
		FString Error;
		return CursorToolHelpers::FindActor(Label, Error) != nullptr;
	}

	/** Removes whatever a test left in the level, so tests do not see each other's actors. */
	struct FScopedActors
	{
		TArray<FString> Labels;
		~FScopedActors()
		{
			UEditorActorSubsystem* Actors = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
			for (const FString& Label : Labels)
			{
				FString Error;
				if (AActor* Actor = CursorToolHelpers::FindActor(Label, Error))
				{
					if (Actors)
					{
						Actors->DestroyActor(Actor);
					}
				}
			}
		}
		FString Add(const TCHAR* Prefix) { return Labels.Add_GetRef(UniqueLabel(Prefix)); }
	};

	FString EdtArgs(const TCHAR* Format, const FString& A, const FString& B = FString())
	{
		// Plain replacement: the JSON around the placeholders is full of braces of its own.
		return FString(Format).Replace(TEXT("{0}"), *A).Replace(TEXT("{1}"), *B);
	}
}

using namespace CursorToolsTests;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorActorToolsTest, "CursorUnreal.Tools.Actors", CursorTest::Flags)
bool FCursorActorToolsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedActors Cleanup;
	const FString Label = Cleanup.Add(TEXT("CUTestLight"));

	FCursorToolResult R = CursorTest::Call(TEXT("actor_spawn"),
		*EdtArgs(TEXT("{\"class\":\"PointLight\",\"location\":[100,200,300],\"label\":\"{0}\",\"folder\":\"CursorUnrealTests\"}"), Label));
	TestFalse(TEXT("spawn succeeds: ") + R.Text, R.bIsError);
	TestTrue(TEXT("spawned actor is in the level"), ActorExists(Label));

	FString Error;
	AActor* Light = CursorToolHelpers::FindActor(Label, Error);
	if (!Light)
	{
		return false;
	}
	TestTrue(TEXT("spawn location applied"), Light->GetActorLocation().Equals(FVector(100, 200, 300), 0.1));
	TestEqual(TEXT("folder applied"), Light->GetFolderPath().ToString(), FString(TEXT("CursorUnrealTests")));

	// Lookup by label, case-insensitively, and by unique object name.
	TestTrue(TEXT("found by lower-case label"), CursorToolHelpers::FindActor(Label.ToLower(), Error) == Light);
	TestTrue(TEXT("found by object name"), CursorToolHelpers::FindActor(Light->GetName(), Error) == Light);

	R = CursorTest::Call(TEXT("actor_update"), *EdtArgs(TEXT("{\"name\":\"{0}\",\"location\":[1,2,3],\"rotation\":[0,90,0]}"), Label));
	TestFalse(TEXT("update succeeds: ") + R.Text, R.bIsError);
	TestTrue(TEXT("update moved the actor"), Light->GetActorLocation().Equals(FVector(1, 2, 3), 0.1));
	TestTrue(TEXT("update rotated the actor"), FMath::IsNearlyEqual(Light->GetActorRotation().Yaw, 90.0, 0.1));

	R = CursorTest::Call(TEXT("actor_update"), *EdtArgs(TEXT("{\"name\":\"{0}\"}"), Label));
	TestTrue(TEXT("update with nothing to change is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("actor_get"), *EdtArgs(TEXT("{\"name\":\"{0}\"}"), Label));
	TestTrue(TEXT("actor_get lists components"), !R.bIsError && R.Structured.IsValid() && R.Structured->HasField(TEXT("components")));

	R = CursorTest::Call(TEXT("actor_list"), *EdtArgs(TEXT("{\"class\":\"Light\",\"name_contains\":\"{0}\"}"), Label));
	TestTrue(TEXT("actor_list finds it through a base class"), !R.bIsError && R.Structured->GetIntegerField(TEXT("total")) == 1);

	R = CursorTest::Call(TEXT("actor_list"), TEXT("{\"class\":\"NoSuchClassAnywhere\"}"));
	TestTrue(TEXT("unknown class is an error"), R.bIsError);

	// Properties on a component, through the same notifications the Details panel sends.
	const FString ComponentPath = Label + TEXT(".LightComponent0");
	R = CursorTest::Call(TEXT("object_set_properties"), *EdtArgs(TEXT("{\"object\":\"{0}\",\"properties\":{\"Intensity\":1234,\"LightColor\":{\"R\":255,\"G\":0,\"B\":0,\"A\":255}}}"), ComponentPath));
	TestFalse(TEXT("set properties succeeds: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("object_get_properties"), *EdtArgs(TEXT("{\"object\":\"{0}\",\"names\":[\"Intensity\",\"LightColor\",\"Nope\"]}"), ComponentPath));
	TestFalse(TEXT("get properties succeeds: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		const TSharedPtr<FJsonObject> Properties = R.Structured->GetObjectField(TEXT("properties"));
		TestEqual(TEXT("number round-trips"), Properties->GetNumberField(TEXT("Intensity")), 1234.0);
		TestEqual(TEXT("struct round-trips"), Properties->GetObjectField(TEXT("LightColor"))->GetIntegerField(TEXT("G")), 0);
		TestTrue(TEXT("unknown names are reported"), R.Structured->HasField(TEXT("not_found")));
	}

	R = CursorTest::Call(TEXT("object_set_properties"), *EdtArgs(TEXT("{\"object\":\"{0}\",\"properties\":{\"NoSuchProperty\":1}}"), ComponentPath));
	TestTrue(TEXT("unknown property is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("object_set_properties"), *EdtArgs(TEXT("{\"object\":\"{0}\",\"properties\":{\"Intensity\":{\"not\":\"a number\"}}}"), ComponentPath));
	TestTrue(TEXT("wrong value type is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("object_get_properties"), *EdtArgs(TEXT("{\"object\":\"{0}.NoSuchComponent\"}"), Label));
	TestTrue(TEXT("unknown component is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("object_set_properties"), *EdtArgs(TEXT("{\"object\":\"{0}\",\"properties\":{\"LightColor\":{\"Red\":1}}}"), ComponentPath));
	TestTrue(TEXT("unknown struct member is an error, not silently ignored: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("Red")));
	R = CursorTest::Call(TEXT("object_set_properties"), *EdtArgs(TEXT("{\"object\":\"{0}\",\"properties\":{\"Mobility\":\"Sideways\"}}"), ComponentPath));
	TestTrue(TEXT("unknown enum value is an error that lists the choices: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("Movable")));
	R = CursorTest::Call(TEXT("object_set_properties"), *EdtArgs(TEXT("{\"object\":\"{0}\",\"properties\":{\"Mobility\":\"Movable\"}}"), ComponentPath));
	TestFalse(TEXT("enum by name succeeds: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("object_set_properties"), *EdtArgs(TEXT("{\"object\":\"{0}\",\"properties\":{\"IESTexture\":\"/Game/No/Such/Texture\"}}"), ComponentPath));
	TestTrue(TEXT("a reference to a missing asset is an error, not a silent null: ") + R.Text, R.bIsError);
	TestTrue(TEXT("a failed edit leaves earlier values alone"), FMath::IsNearlyEqual(
		CursorTest::Call(TEXT("object_get_properties"), *EdtArgs(TEXT("{\"object\":\"{0}\",\"names\":[\"Intensity\"]}"), ComponentPath))
			.Structured->GetObjectField(TEXT("properties"))->GetNumberField(TEXT("Intensity")), 1234.0));

	// Nested struct member by dotted path.
	R = CursorTest::Call(TEXT("object_set_properties"), *EdtArgs(TEXT("{\"object\":\"{0}\",\"properties\":{\"RelativeLocation.Z\":777}}"), ComponentPath));
	TestFalse(TEXT("dotted path set succeeds: ") + R.Text, R.bIsError);
	TestTrue(TEXT("dotted path changed only Z"), Light->GetActorLocation().Equals(FVector(1, 2, 777), 0.1));

	R = CursorTest::Call(TEXT("actor_duplicate"), *EdtArgs(TEXT("{\"name\":\"{0}\",\"offset\":[0,0,50]}"), Label));
	TestFalse(TEXT("duplicate succeeds: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		Cleanup.Labels.Add(R.Structured->GetStringField(TEXT("name")));
	}

	R = CursorTest::Call(TEXT("selection_set"), *EdtArgs(TEXT("{\"names\":[\"{0}\"]}"), Label));
	TestFalse(TEXT("selection_set succeeds: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("editor_get_context"));
	TestTrue(TEXT("context reports the selection"), !R.bIsError && R.Text.Contains(Light->GetName()));

	R = CursorTest::Call(TEXT("actor_delete"), *EdtArgs(TEXT("{\"names\":[\"{0}\",\"definitely-not-an-actor\"]}"), Label));
	TestTrue(TEXT("delete with a bad name is an error"), R.bIsError);
	TestTrue(TEXT("and deletes nothing"), ActorExists(Label));

	R = CursorTest::Call(TEXT("actor_delete"), *EdtArgs(TEXT("{\"names\":[\"{0}\"]}"), Label));
	TestFalse(TEXT("delete succeeds: ") + R.Text, R.bIsError);
	TestFalse(TEXT("deleted actor is gone"), ActorExists(Label));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorToolsSafetyTest, "CursorUnreal.Tools.Safety", CursorTest::Flags)
bool FCursorToolsSafetyTest::RunTest(const FString&)
{
	FScopedActors Cleanup;
	const FString Label = Cleanup.Add(TEXT("CUTestKeep"));

	{
		// Default policy, and nobody to ask: dangerous tools must be refused, for every caller.
		CursorTest::FScopedPolicy Policy;
		FCursorToolResult R = CursorTest::Call(TEXT("actor_spawn"), *EdtArgs(TEXT("{\"class\":\"PointLight\",\"label\":\"{0}\"}"), Label));
		TestFalse(TEXT("undoable tools run under the default policy: ") + R.Text, R.bIsError);

		for (const FString& Caller : { FCursorToolRegistry::CallerNative, FCursorToolRegistry::CallerCursorSdk, FCursorToolRegistry::CallerMcp })
		{
			R = CursorTest::Call(TEXT("actor_delete"), *EdtArgs(TEXT("{\"names\":[\"{0}\"]}"), Label), Caller);
			TestTrue(TEXT("delete is refused without approval for ") + Caller, R.bIsError);
			R = CursorTest::Call(TEXT("python_exec"), TEXT("{\"code\":\"import unreal\"}"), Caller);
			TestTrue(TEXT("python is refused without approval for ") + Caller, R.bIsError);
			R = CursorTest::Call(TEXT("console_command"), TEXT("{\"command\":\"stat fps\"}"), Caller);
			TestTrue(TEXT("console is refused without approval for ") + Caller, R.bIsError);
			// Hiding a dangerous call inside a batch must not get it past the gate.
			R = CursorTest::Call(TEXT("batch"), *EdtArgs(TEXT("{\"calls\":[{\"tool\":\"actor_delete\",\"args\":{\"names\":[\"{0}\"]}}]}"), Label), Caller);
			TestTrue(TEXT("batched delete is refused without approval for ") + Caller, R.bIsError);
		}
		TestTrue(TEXT("the actor survived every refused delete"), ActorExists(Label));
	}

	{
		// Read-only policy: nothing changes, reading still works.
		CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::ReadOnly);
		FCursorToolResult R = CursorTest::Call(TEXT("actor_update"), *EdtArgs(TEXT("{\"name\":\"{0}\",\"location\":[9,9,9]}"), Label));
		TestTrue(TEXT("read-only policy refuses edits"), R.bIsError);
		R = CursorTest::Call(TEXT("actor_get"), *EdtArgs(TEXT("{\"name\":\"{0}\"}"), Label));
		TestFalse(TEXT("read-only policy allows reads"), R.bIsError);
	}

	{
		// Engine content is off limits to the asset-changing tools even when everything is allowed.
		CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
		FCursorToolResult R = CursorTest::Call(TEXT("asset_delete"), TEXT("{\"path\":\"/Engine/BasicShapes/Cube\",\"force\":true}"));
		TestTrue(TEXT("engine assets are not deleted"), R.bIsError);
		R = CursorTest::Call(TEXT("asset_rename"), TEXT("{\"source\":\"/Engine/BasicShapes/Cube\",\"destination\":\"/Game/Cube\"}"));
		TestTrue(TEXT("engine assets are not moved"), R.bIsError);
		R = CursorTest::Call(TEXT("asset_duplicate"), TEXT("{\"source\":\"/Engine/BasicShapes/Cube\",\"destination\":\"/Engine/BasicShapes/Cube2\"}"));
		TestTrue(TEXT("nothing is written into engine content"), R.bIsError);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorBatchRollbackTest, "CursorUnreal.Tools.BatchRollback", CursorTest::Flags)
bool FCursorBatchRollbackTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedActors Cleanup;
	const FString A = Cleanup.Add(TEXT("CUTestBatchA"));
	const FString B = Cleanup.Add(TEXT("CUTestBatchB"));

	if (!GEditor || !GEditor->CanTransact())
	{
		AddWarning(TEXT("The editor cannot record transactions in this run, so rollback was not exercised."));
		return true;
	}

	// Third call fails: the two spawns before it must be undone.
	FCursorToolResult R = CursorTest::Call(TEXT("batch"), *EdtArgs(
		TEXT("{\"calls\":[")
		TEXT("{\"tool\":\"actor_spawn\",\"args\":{\"class\":\"PointLight\",\"label\":\"{0}\"}},")
		TEXT("{\"tool\":\"actor_spawn\",\"args\":{\"class\":\"PointLight\",\"label\":\"{1}\"}},")
		TEXT("{\"tool\":\"actor_update\",\"args\":{\"name\":\"no-such-actor-anywhere\",\"location\":[0,0,0]}}]}"), A, B));
	TestTrue(TEXT("batch reports the failure"), R.bIsError);
	TestTrue(TEXT("batch says it rolled back: ") + R.Text, R.Text.Contains(TEXT("\"rolled_back\":true")));
	TestFalse(TEXT("first spawn was undone"), ActorExists(A));
	TestFalse(TEXT("second spawn was undone"), ActorExists(B));

	// Same batch with rollback off keeps what succeeded.
	R = CursorTest::Call(TEXT("batch"), *EdtArgs(
		TEXT("{\"rollback_on_error\":false,\"calls\":[")
		TEXT("{\"tool\":\"actor_spawn\",\"args\":{\"class\":\"PointLight\",\"label\":\"{0}\"}},")
		TEXT("{\"tool\":\"actor_update\",\"args\":{\"name\":\"no-such-actor-anywhere\",\"location\":[0,0,0]}}]}"), A));
	TestTrue(TEXT("batch still reports the failure"), R.bIsError);
	TestTrue(TEXT("without rollback the spawn stays"), ActorExists(A));

	// A clean batch is one undo step.
	R = CursorTest::Call(TEXT("batch"), *EdtArgs(
		TEXT("{\"calls\":[")
		TEXT("{\"tool\":\"actor_spawn\",\"args\":{\"class\":\"PointLight\",\"label\":\"{0}\"}},")
		TEXT("{\"tool\":\"actor_update\",\"args\":{\"name\":\"{0}\",\"location\":[5,5,5]}}]}"), B));
	TestFalse(TEXT("clean batch succeeds: ") + R.Text, R.bIsError);
	TestTrue(TEXT("clean batch made the actor"), ActorExists(B));
	GEditor->UndoTransaction();
	TestFalse(TEXT("one undo reverses the whole batch"), ActorExists(B));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAssetToolsTest, "CursorUnreal.Tools.Assets", CursorTest::Flags)
bool FCursorAssetToolsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedActors Cleanup;

	FCursorToolResult R = CursorTest::Call(TEXT("asset_search"), TEXT("{\"path\":\"/Engine/BasicShapes\",\"class\":\"StaticMesh\",\"query\":\"cube\"}"));
	TestFalse(TEXT("search succeeds: ") + R.Text, R.bIsError);
	TestTrue(TEXT("search finds the engine cube"), R.Text.Contains(TEXT("/Engine/BasicShapes/Cube.Cube")));

	R = CursorTest::Call(TEXT("asset_search"), TEXT("{\"path\":\"/Engine/BasicShapes\",\"limit\":1}"));
	TestTrue(TEXT("limit is honoured"), !R.bIsError && R.Structured->GetArrayField(TEXT("assets")).Num() == 1
		&& R.Structured->GetIntegerField(TEXT("total")) > 1);

	R = CursorTest::Call(TEXT("asset_get_info"), TEXT("{\"path\":\"/Engine/BasicShapes/Cube\"}"));
	TestFalse(TEXT("info succeeds: ") + R.Text, R.bIsError);
	TestTrue(TEXT("info reports the class"), R.Text.Contains(TEXT("StaticMesh")));

	R = CursorTest::Call(TEXT("asset_get_info"), TEXT("{\"path\":\"/Game/No/Such/Asset\"}"));
	TestTrue(TEXT("missing asset is an error"), R.bIsError);

	// Placing an asset uses the editor's own actor factories.
	const FString Label = Cleanup.Add(TEXT("CUTestCube"));
	R = CursorTest::Call(TEXT("actor_spawn"), *EdtArgs(TEXT("{\"class\":\"/Engine/BasicShapes/Cube\",\"label\":\"{0}\"}"), Label));
	TestFalse(TEXT("spawn from a mesh asset succeeds: ") + R.Text, R.bIsError);
	TestTrue(TEXT("it made a StaticMeshActor"), R.Text.Contains(TEXT("StaticMeshActor")));

	R = CursorTest::Call(TEXT("asset_save"), TEXT("{\"paths\":[\"/Game/No/Such/Asset\"]}"));
	TestTrue(TEXT("saving a missing asset is an error"), R.bIsError);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorEscapeHatchTest, "CursorUnreal.Tools.EscapeHatches", CursorTest::Flags)
bool FCursorEscapeHatchTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);

	const FString Marker = TEXT("tail-marker-") + FGuid::NewGuid().ToString();
	UE_LOG(LogCursorUnreal, Display, TEXT("%s"), *Marker);
	FCursorToolResult R = CursorTest::Call(TEXT("editor_log_tail"), *EdtArgs(TEXT("{\"contains\":\"{0}\",\"lines\":5}"), Marker));
	TestTrue(TEXT("log tail finds a fresh line"), !R.bIsError && R.Text.Contains(Marker));

	R = CursorTest::Call(TEXT("console_command"), TEXT("{\"command\":\"t.MaxFPS\"}"));
	TestFalse(TEXT("console command runs: ") + R.Text, R.bIsError);
	TestTrue(TEXT("console output is returned: ") + R.Text, R.Text.Contains(TEXT("t.MaxFPS")));

	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (Python && Python->IsPythonAvailable())
	{
		R = CursorTest::Call(TEXT("python_exec"), TEXT("{\"code\":\"6 * 7\",\"mode\":\"eval\"}"));
		TestTrue(TEXT("python eval returns the value: ") + R.Text, !R.bIsError && R.Text.Contains(TEXT("42")));

		R = CursorTest::Call(TEXT("python_exec"), TEXT("{\"code\":\"import unreal\\nprint('from-python', unreal.SystemLibrary.get_engine_version()[:1])\"}"));
		TestTrue(TEXT("python exec returns printed output: ") + R.Text, !R.bIsError && R.Text.Contains(TEXT("from-python")));

		// The traceback is also written to the editor log as errors, which is what we want in real use.
		AddExpectedError(TEXT("Traceback"), EAutomationExpectedErrorFlags::Contains, 0);
		AddExpectedError(TEXT("in <module>"), EAutomationExpectedErrorFlags::Contains, 0);
		AddExpectedError(TEXT("boom-from-test"), EAutomationExpectedErrorFlags::Contains, 0);
		R = CursorTest::Call(TEXT("python_exec"), TEXT("{\"code\":\"raise RuntimeError('boom-from-test')\"}"));
		TestTrue(TEXT("python errors come back as errors with the message: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("boom-from-test")));
	}
	else
	{
		AddInfo(TEXT("Python is not available in this run; python_exec was not exercised."));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
