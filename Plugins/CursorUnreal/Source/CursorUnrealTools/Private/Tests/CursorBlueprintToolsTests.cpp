#if WITH_DEV_AUTOMATION_TESTS

#include "CursorPackTestSupport.h"
#include "Modules/ModuleManager.h"

using namespace CursorPackTest;

namespace
{
	/** The first node in a get_nodes / add_node result with this title fragment, or empty. */
	FString NodeIdByTitle(const FCursorToolResult& NodesResult, const FString& TitleContains)
	{
		if (NodesResult.Structured.IsValid() && NodesResult.Structured->HasField(TEXT("nodes")))
		{
			for (const TSharedPtr<FJsonValue>& Value : NodesResult.Structured->GetArrayField(TEXT("nodes")))
			{
				const TSharedPtr<FJsonObject> Node = Value->AsObject();
				if (Node->GetStringField(TEXT("title")).Contains(TitleContains) || Node->GetStringField(TEXT("class")).Contains(TitleContains))
				{
					return Node->GetStringField(TEXT("id"));
				}
			}
		}
		return FString();
	}
}

// Every Phase 4a tool is registered, in the right pack, with the risk and audience the approval gate relies on.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorPhase4aRegistrationTest, "CursorUnreal.Tools.Phase4aRegistration", CursorTest::Flags)
bool FCursorPhase4aRegistrationTest::RunTest(const FString&)
{
	struct FExpected
	{
		const TCHAR* Name;
		const TCHAR* Pack;
		ECursorToolRisk Risk;
	};
	const FExpected Expected[] =
	{
		{ TEXT("blueprint_create"), TEXT("blueprint"), ECursorToolRisk::Mutating },
		{ TEXT("blueprint_get_info"), TEXT("blueprint"), ECursorToolRisk::ReadOnly },
		{ TEXT("blueprint_list_graphs"), TEXT("blueprint"), ECursorToolRisk::ReadOnly },
		{ TEXT("blueprint_get_nodes"), TEXT("blueprint"), ECursorToolRisk::ReadOnly },
		{ TEXT("blueprint_add_variable"), TEXT("blueprint"), ECursorToolRisk::Mutating },
		{ TEXT("blueprint_add_component"), TEXT("blueprint"), ECursorToolRisk::Mutating },
		{ TEXT("blueprint_add_function"), TEXT("blueprint"), ECursorToolRisk::Mutating },
		{ TEXT("blueprint_add_node"), TEXT("blueprint"), ECursorToolRisk::Mutating },
		{ TEXT("blueprint_remove_node"), TEXT("blueprint"), ECursorToolRisk::Mutating },
		{ TEXT("blueprint_connect_pins"), TEXT("blueprint"), ECursorToolRisk::Mutating },
		{ TEXT("blueprint_set_pin_default"), TEXT("blueprint"), ECursorToolRisk::Mutating },
		{ TEXT("blueprint_compile"), TEXT("blueprint"), ECursorToolRisk::Mutating },
		{ TEXT("cpp_create_class"), TEXT("cpp"), ECursorToolRisk::Mutating },
		{ TEXT("cpp_build"), TEXT("cpp"), ECursorToolRisk::Dangerous },
		{ TEXT("source_read"), TEXT("cpp"), ECursorToolRisk::ReadOnly },
		{ TEXT("source_search"), TEXT("cpp"), ECursorToolRisk::ReadOnly },
		{ TEXT("source_write"), TEXT("cpp"), ECursorToolRisk::Dangerous },
		{ TEXT("material_create"), TEXT("materials"), ECursorToolRisk::Mutating },
		{ TEXT("material_create_instance"), TEXT("materials"), ECursorToolRisk::Mutating },
		{ TEXT("material_get_info"), TEXT("materials"), ECursorToolRisk::ReadOnly },
		{ TEXT("material_add_expression"), TEXT("materials"), ECursorToolRisk::Mutating },
		{ TEXT("material_remove_expression"), TEXT("materials"), ECursorToolRisk::Mutating },
		{ TEXT("material_connect"), TEXT("materials"), ECursorToolRisk::Mutating },
		{ TEXT("material_set_parameter"), TEXT("materials"), ECursorToolRisk::Mutating },
		{ TEXT("material_compile"), TEXT("materials"), ECursorToolRisk::Mutating },
		// Phase 4b packs (Niagara lives in CursorUnrealToolsNiagara; assert when that module is loaded).
		{ TEXT("animbp_create"), TEXT("animation"), ECursorToolRisk::Mutating },
		{ TEXT("animbp_get_info"), TEXT("animation"), ECursorToolRisk::ReadOnly },
		{ TEXT("animbp_list_graphs"), TEXT("animation"), ECursorToolRisk::ReadOnly },
		{ TEXT("animbp_list_nodes"), TEXT("animation"), ECursorToolRisk::ReadOnly },
		{ TEXT("animbp_compile"), TEXT("animation"), ECursorToolRisk::Mutating },
		{ TEXT("sound_create_cue"), TEXT("audio"), ECursorToolRisk::Mutating },
		{ TEXT("sound_get_info"), TEXT("audio"), ECursorToolRisk::ReadOnly },
		{ TEXT("sound_create_attenuation"), TEXT("audio"), ECursorToolRisk::Mutating },
		{ TEXT("sound_configure_attenuation"), TEXT("audio"), ECursorToolRisk::Mutating },
		{ TEXT("sound_set_attenuation"), TEXT("audio"), ECursorToolRisk::Mutating },
		{ TEXT("sound_play"), TEXT("audio"), ECursorToolRisk::Mutating },
		{ TEXT("sound_stop"), TEXT("audio"), ECursorToolRisk::Mutating },
		{ TEXT("sound_spawn_ambient"), TEXT("audio"), ECursorToolRisk::Mutating },
		{ TEXT("optim_level_stats"), TEXT("optimisation"), ECursorToolRisk::ReadOnly },
		{ TEXT("optim_list_heavy_actors"), TEXT("optimisation"), ECursorToolRisk::ReadOnly },
		{ TEXT("optim_mesh_info"), TEXT("optimisation"), ECursorToolRisk::ReadOnly },
		{ TEXT("optim_suggest_lods"), TEXT("optimisation"), ECursorToolRisk::ReadOnly },
		{ TEXT("niagara_create_system"), TEXT("niagara"), ECursorToolRisk::Mutating },
		{ TEXT("niagara_get_info"), TEXT("niagara"), ECursorToolRisk::ReadOnly },
		{ TEXT("niagara_compile"), TEXT("niagara"), ECursorToolRisk::Mutating },
		{ TEXT("niagara_add_emitter"), TEXT("niagara"), ECursorToolRisk::Mutating },
		{ TEXT("niagara_remove_emitter"), TEXT("niagara"), ECursorToolRisk::Mutating },
		{ TEXT("niagara_set_emitter_enabled"), TEXT("niagara"), ECursorToolRisk::Mutating },
		{ TEXT("niagara_list_parameters"), TEXT("niagara"), ECursorToolRisk::ReadOnly },
		{ TEXT("niagara_add_user_parameter"), TEXT("niagara"), ECursorToolRisk::Mutating },
		{ TEXT("niagara_set_parameter"), TEXT("niagara"), ECursorToolRisk::Mutating },
		{ TEXT("niagara_spawn_in_level"), TEXT("niagara"), ECursorToolRisk::Mutating },
	};

	for (const FExpected& Entry : Expected)
	{
		const bool bNiagara = FString(Entry.Pack) == TEXT("niagara");
		if (bNiagara && !FModuleManager::Get().IsModuleLoaded(TEXT("CursorUnrealToolsNiagara")))
		{
			AddWarning(TEXT("CursorUnrealToolsNiagara not loaded; skipping Niagara registration checks."));
			continue;
		}
		const TSharedPtr<ICursorTool> Tool = FCursorToolRegistry::Get().Find(Entry.Name);
		TestTrue(FString(Entry.Name) + TEXT(" is registered"), Tool.IsValid());
		if (!Tool.IsValid())
		{
			continue;
		}
		TestEqual(FString(Entry.Name) + TEXT(" pack"), Tool->GetPack().ToString(), FString(Entry.Pack));
		TestTrue(FString(Entry.Name) + TEXT(" risk is ") + LexToString(Entry.Risk), Tool->GetRisk() == Entry.Risk);
		const bool bSourceTool = FString(Entry.Name).StartsWith(TEXT("source_"));
		TestTrue(FString(Entry.Name) + TEXT(" audience"), Tool->GetAudience() == (bSourceTool ? ECursorToolAudience::NativeAgentOnly : ECursorToolAudience::All));
		TestTrue(FString(Entry.Name) + TEXT(" has a description"), !Tool->GetDescription().IsEmpty());
		TestTrue(FString(Entry.Name) + TEXT(" has an object schema"), Tool->GetInputSchema()->HasField(TEXT("properties")));
	}

	// Switching a pack off hides all of it, and the packs can be switched off independently.
	{
		CursorTest::FScopedPolicy Policy;
		UCursorUnrealSettings::GetMutable()->DisabledToolPacks = { FName(TEXT("blueprint")), FName(TEXT("materials")), FName(TEXT("audio")) };
		TArray<TSharedRef<ICursorTool>> Offered = FCursorToolRegistry::Get().GetTools(FCursorToolRegistry::CallerMcp);
		TestFalse(TEXT("disabled blueprint pack is hidden"), Offered.ContainsByPredicate([](const TSharedRef<ICursorTool>& T) { return T->GetPack() == FName(TEXT("blueprint")); }));
		TestFalse(TEXT("disabled materials pack is hidden"), Offered.ContainsByPredicate([](const TSharedRef<ICursorTool>& T) { return T->GetPack() == FName(TEXT("materials")); }));
		TestFalse(TEXT("disabled audio pack is hidden"), Offered.ContainsByPredicate([](const TSharedRef<ICursorTool>& T) { return T->GetPack() == FName(TEXT("audio")); }));
		TestTrue(TEXT("the cpp pack is still offered"), Offered.ContainsByPredicate([](const TSharedRef<ICursorTool>& T) { return T->GetPack() == FName(TEXT("cpp")); }));
		TestTrue(TEXT("the animation pack is still offered"), Offered.ContainsByPredicate([](const TSharedRef<ICursorTool>& T) { return T->GetPack() == FName(TEXT("animation")); }));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorBlueprintToolsTest, "CursorUnreal.Tools.Blueprint", CursorTest::Flags)
bool FCursorBlueprintToolsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedTestAssets Assets;
	const FString Path = Assets.Add(TEXT("BP_CUTest"));

	// ---- create ---------------------------------------------------------------------------------------------------------
	FCursorToolResult R = CursorTest::Call(TEXT("blueprint_create"), *Args(TEXT("{\"path\":\"{0}\",\"parent_class\":\"Actor\"}"), Path));
	TestFalse(TEXT("create succeeds: ") + R.Text, R.bIsError);
	if (R.bIsError)
	{
		return false;
	}
	R = CursorTest::Call(TEXT("blueprint_create"), *Args(TEXT("{\"path\":\"{0}\"}"), Path));
	TestTrue(TEXT("creating over an existing asset is an error: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_create"), TEXT("{\"path\":\"/Engine/CursorUnrealTests/BP_Nope\"}"));
	TestTrue(TEXT("engine content is not written to"), R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_create"), *Args(TEXT("{\"path\":\"{0}_Bad\",\"parent_class\":\"NoSuchClassAnywhere\"}"), Path));
	TestTrue(TEXT("unknown parent class is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("blueprint_list_graphs"), *Args(TEXT("{\"blueprint\":\"{0}\"}"), Path));
	TestTrue(TEXT("a new Actor Blueprint has an event graph: ") + R.Text, !R.bIsError && R.Text.Contains(TEXT("EventGraph")));

	R = CursorTest::Call(TEXT("blueprint_get_info"), TEXT("{\"blueprint\":\"/Game/No/Such/Blueprint\"}"));
	TestTrue(TEXT("missing Blueprint is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_get_info"), TEXT("{\"blueprint\":\"/Engine/BasicShapes/Cube\"}"));
	TestTrue(TEXT("a non-Blueprint asset is an error that says so: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("not a Blueprint")));

	// ---- variables and components ---------------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("blueprint_add_variable"), *Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"Health\",\"type\":\"float\",\"default_value\":\"100\",\"instance_editable\":true}"), Path));
	TestFalse(TEXT("add float variable: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_add_variable"), *Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"Targets\",\"type\":\"object:Actor\",\"is_array\":true}"), Path));
	TestFalse(TEXT("add array-of-actors variable: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_add_variable"), *Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"Home\",\"type\":\"vector\"}"), Path));
	TestFalse(TEXT("add struct variable: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_add_variable"), *Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"Health\",\"type\":\"int\"}"), Path));
	TestTrue(TEXT("duplicate variable is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_add_variable"), *Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"Odd\",\"type\":\"banana\"}"), Path));
	TestTrue(TEXT("unknown type is an error that lists the choices: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("vector")));
	R = CursorTest::Call(TEXT("blueprint_add_variable"), *Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"has space\",\"type\":\"int\"}"), Path));
	TestTrue(TEXT("invalid variable name is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("blueprint_add_component"), *Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"StaticMeshComponent\",\"name\":\"Body\"}"), Path));
	TestFalse(TEXT("add root component: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_add_component"), *Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"PointLightComponent\",\"name\":\"Glow\",\"parent\":\"Body\"}"), Path));
	TestFalse(TEXT("add attached component: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_add_component"), *Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"PointLightComponent\",\"name\":\"Body\"}"), Path));
	TestTrue(TEXT("duplicate component name is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_add_component"), *Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"Actor\"}"), Path));
	TestTrue(TEXT("a non-component class is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_add_component"), *Args(TEXT("{\"blueprint\":\"{0}\",\"class\":\"PointLightComponent\",\"parent\":\"NoSuchParent\"}"), Path));
	TestTrue(TEXT("unknown parent component is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("blueprint_get_info"), *Args(TEXT("{\"blueprint\":\"{0}\"}"), Path));
	TestFalse(TEXT("get_info succeeds: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		TestEqual(TEXT("three variables are reported"), R.Structured->GetArrayField(TEXT("variables")).Num(), 3);
		TestTrue(TEXT("components are reported with their parent"), R.Text.Contains(TEXT("\"Glow\"")) && R.Text.Contains(TEXT("\"parent\":\"Body\"")));
		TestTrue(TEXT("array type is described"), R.Text.Contains(TEXT("array of object:Actor")));
	}

	// ---- function graph -------------------------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("blueprint_add_function"), *Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"DoThing\"}"), Path));
	TestFalse(TEXT("add function: ") + R.Text, R.bIsError);
	const FString EntryId = R.bIsError ? FString() : NodeIdByTitle(R, TEXT("FunctionEntry"));
	TestFalse(TEXT("the function has an entry node"), EntryId.IsEmpty());
	R = CursorTest::Call(TEXT("blueprint_add_function"), *Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"DoThing\"}"), Path));
	TestTrue(TEXT("duplicate function name is an error"), R.bIsError);
	if (!EntryId.IsEmpty())
	{
		R = CursorTest::Call(TEXT("blueprint_remove_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"graph\":\"DoThing\",\"node\":\"{1}\"}"), Path, EntryId));
		TestTrue(TEXT("a function's entry node cannot be deleted: ") + R.Text, R.bIsError);
	}

	// ---- nodes and wiring -----------------------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"event\",\"name\":\"BeginPlay\"}"), Path));
	TestFalse(TEXT("add BeginPlay event: ") + R.Text, R.bIsError);
	const FString EventId = R.bIsError ? FString() : R.Structured->GetStringField(TEXT("id"));
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"event\",\"name\":\"BeginPlay\"}"), Path));
	TestTrue(TEXT("a second BeginPlay returns the first: ") + R.Text, !R.bIsError && R.Structured->HasField(TEXT("already_existed")) && R.Structured->GetStringField(TEXT("id")) == EventId);
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"event\",\"name\":\"NoSuchEvent\"}"), Path));
	TestTrue(TEXT("unknown event is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"event\",\"name\":\"BeginPlay\",\"graph\":\"DoThing\"}"), Path));
	TestTrue(TEXT("events cannot go in a function graph"), R.bIsError);

	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"call_function\",\"class\":\"KismetSystemLibrary\",\"function\":\"PrintString\"}"), Path));
	TestFalse(TEXT("add PrintString: ") + R.Text, R.bIsError);
	const FString PrintId = R.bIsError ? FString() : R.Structured->GetStringField(TEXT("id"));
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"call_function\",\"function\":\"NoSuchFunction\"}"), Path));
	TestTrue(TEXT("unknown function is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"call_function\",\"function\":\"SetActorLocation\"}"), Path));
	TestFalse(TEXT("a K2_ function is found by its display name: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"variable_set\",\"name\":\"Health\"}"), Path));
	TestFalse(TEXT("add variable set: ") + R.Text, R.bIsError);
	const FString SetId = R.bIsError ? FString() : R.Structured->GetStringField(TEXT("id"));
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"variable_get\",\"name\":\"Health\"}"), Path));
	TestFalse(TEXT("add variable get: ") + R.Text, R.bIsError);
	const FString GetId = R.bIsError ? FString() : R.Structured->GetStringField(TEXT("id"));
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"variable_get\",\"name\":\"NoSuchVariable\"}"), Path));
	TestTrue(TEXT("unknown variable is an error"), R.bIsError);
	for (const TCHAR* Kind : { TEXT("branch"), TEXT("sequence"), TEXT("self") })
	{
		R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"{1}\"}"), Path, Kind));
		TestFalse(FString::Printf(TEXT("add %s: "), Kind) + R.Text, R.bIsError);
	}
	R = CursorTest::Call(TEXT("blueprint_add_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"kind\":\"teleport\"}"), Path));
	TestTrue(TEXT("unknown kind is an error"), R.bIsError);

	if (EventId.IsEmpty() || PrintId.IsEmpty() || SetId.IsEmpty() || GetId.IsEmpty())
	{
		AddError(TEXT("Could not create the nodes the wiring checks need."));
		return false;
	}

	R = CursorTest::Call(TEXT("blueprint_connect_pins"), *Args(TEXT("{\"blueprint\":\"{0}\",\"from_node\":\"{1}\",\"from_pin\":\"then\",\"to_node\":\"{2}\",\"to_pin\":\"execute\"}"), Path, EventId, PrintId));
	TestFalse(TEXT("connect BeginPlay to PrintString: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_connect_pins"), *Args(TEXT("{\"blueprint\":\"{0}\",\"from_node\":\"{1}\",\"from_pin\":\"then\",\"to_node\":\"{2}\",\"to_pin\":\"InString\"}"), Path, EventId, PrintId));
	TestTrue(TEXT("an exec pin cannot feed a string pin: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_connect_pins"), *Args(TEXT("{\"blueprint\":\"{0}\",\"from_node\":\"{1}\",\"from_pin\":\"nope\",\"to_node\":\"{2}\",\"to_pin\":\"execute\"}"), Path, EventId, PrintId));
	TestTrue(TEXT("an unknown pin is an error that lists the pins: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("then")));
	R = CursorTest::Call(TEXT("blueprint_connect_pins"), *Args(TEXT("{\"blueprint\":\"{0}\",\"from_node\":\"no-such-node\",\"from_pin\":\"then\",\"to_node\":\"{1}\",\"to_pin\":\"execute\"}"), Path, PrintId));
	TestTrue(TEXT("an unknown node is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("blueprint_connect_pins"), *Args(TEXT("{\"blueprint\":\"{0}\",\"from_node\":\"{1}\",\"from_pin\":\"Health\",\"to_node\":\"{2}\",\"to_pin\":\"Health\"}"), Path, GetId, SetId));
	TestFalse(TEXT("connect a variable getter to a setter: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("blueprint_set_pin_default"), *Args(TEXT("{\"blueprint\":\"{0}\",\"node\":\"{1}\",\"pin\":\"InString\",\"value\":\"Hello from a test\"}"), Path, PrintId));
	TestFalse(TEXT("set a string default: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_set_pin_default"), *Args(TEXT("{\"blueprint\":\"{0}\",\"node\":\"{1}\",\"pin\":\"Duration\",\"value\":\"not a number\"}"), Path, PrintId));
	TestTrue(TEXT("a bad value for a float pin is an error: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_set_pin_default"), *Args(TEXT("{\"blueprint\":\"{0}\",\"node\":\"{1}\",\"pin\":\"Health\",\"value\":\"5\"}"), Path, SetId));
	TestTrue(TEXT("a connected pin refuses a literal: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("blueprint_get_nodes"), *Args(TEXT("{\"blueprint\":\"{0}\"}"), Path));
	TestFalse(TEXT("get_nodes succeeds: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		TestTrue(TEXT("the string default is reported"), R.Text.Contains(TEXT("Hello from a test")));
		TestTrue(TEXT("links are reported"), R.Text.Contains(TEXT("linked_to")) && R.Text.Contains(PrintId));
		TestTrue(TEXT("node total is positive"), R.Structured->GetIntegerField(TEXT("total")) >= 7);
	}
	R = CursorTest::Call(TEXT("blueprint_get_nodes"), *Args(TEXT("{\"blueprint\":\"{0}\",\"graph\":\"DoThing\",\"include_pins\":false}"), Path));
	TestTrue(TEXT("a function graph can be listed without pins"), !R.bIsError && !R.Text.Contains(TEXT("\"pins\"")));
	R = CursorTest::Call(TEXT("blueprint_get_nodes"), *Args(TEXT("{\"blueprint\":\"{0}\",\"graph\":\"NoSuchGraph\"}"), Path));
	TestTrue(TEXT("an unknown graph is an error that lists the graphs: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("EventGraph")));

	// ---- compile --------------------------------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("blueprint_compile"), *Args(TEXT("{\"blueprint\":\"{0}\"}"), Path));
	TestFalse(TEXT("the Blueprint compiles: ") + R.Text, R.bIsError);
	TestTrue(TEXT("compile reports a status and message arrays"), R.Structured.IsValid() && R.Structured->HasField(TEXT("status")) && R.Structured->HasField(TEXT("errors")));

	// ---- remove ---------------------------------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("blueprint_remove_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"node\":\"{1}\"}"), Path, PrintId));
	TestFalse(TEXT("remove a node: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("blueprint_remove_node"), *Args(TEXT("{\"blueprint\":\"{0}\",\"node\":\"{1}\"}"), Path, PrintId));
	TestTrue(TEXT("removing it again is an error"), R.bIsError);

	// ---- policy ---------------------------------------------------------------------------------------------------------
	{
		CursorTest::FScopedPolicy ReadOnly(NAME_None, ECursorApprovalPolicy::ReadOnly);
		R = CursorTest::Call(TEXT("blueprint_add_variable"), *Args(TEXT("{\"blueprint\":\"{0}\",\"name\":\"Blocked\",\"type\":\"int\"}"), Path));
		TestTrue(TEXT("the read-only policy refuses Blueprint edits"), R.bIsError);
		R = CursorTest::Call(TEXT("blueprint_get_info"), *Args(TEXT("{\"blueprint\":\"{0}\"}"), Path));
		TestFalse(TEXT("the read-only policy allows reading"), R.bIsError);
	}

	// Engine Blueprints may be read but never edited.
	R = CursorTest::Call(TEXT("blueprint_add_variable"), TEXT("{\"blueprint\":\"/Engine/EngineSky/BP_Sky_Sphere\",\"name\":\"X\",\"type\":\"int\"}"));
	TestTrue(TEXT("engine Blueprints are not edited"), R.bIsError);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
