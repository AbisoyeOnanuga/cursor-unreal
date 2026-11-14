#if WITH_DEV_AUTOMATION_TESTS

#include "CursorPackTestSupport.h"

using namespace CursorPackTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorMaterialToolsTest, "CursorUnreal.Tools.Material", CursorTest::Flags)
bool FCursorMaterialToolsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedTestAssets Assets;
	const FString Path = Assets.Add(TEXT("M_CUTest"));

	// ---- create ---------------------------------------------------------------------------------------------------------
	FCursorToolResult R = CursorTest::Call(TEXT("material_create"), *Args(TEXT("{\"path\":\"{0}\",\"shading_model\":\"unlit\",\"two_sided\":true}"), Path));
	TestFalse(TEXT("create succeeds: ") + R.Text, R.bIsError);
	if (R.bIsError)
	{
		return false;
	}
	R = CursorTest::Call(TEXT("material_create"), *Args(TEXT("{\"path\":\"{0}\"}"), Path));
	TestTrue(TEXT("creating over an existing material is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("material_create"), TEXT("{\"path\":\"/Engine/CursorUnrealTests/M_Nope\"}"));
	TestTrue(TEXT("engine content is not written to"), R.bIsError);
	R = CursorTest::Call(TEXT("material_create"), *Args(TEXT("{\"path\":\"{0}_Bad\",\"blend_mode\":\"fluffy\"}"), Path));
	TestTrue(TEXT("unknown blend mode is an error"), R.bIsError);
	TestFalse(TEXT("a rejected create leaves no asset behind"), FindPackage(nullptr, *(Path + TEXT("_Bad"))) && FindObject<UObject>(FindPackage(nullptr, *(Path + TEXT("_Bad"))), *(FPackageName::GetShortName(Path) + TEXT("_Bad"))));

	// ---- expressions ----------------------------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("material_add_expression"), *Args(TEXT("{\"material\":\"{0}\",\"type\":\"ScalarParameter\",\"name\":\"Rough\",\"value\":0.4}"), Path));
	TestFalse(TEXT("add scalar parameter: ") + R.Text, R.bIsError);
	const FString RoughId = R.bIsError ? FString() : R.Structured->GetStringField(TEXT("name"));
	TestTrue(TEXT("the default value is applied"), !R.bIsError && FMath::IsNearlyEqual(R.Structured->GetNumberField(TEXT("default_value")), 0.4, 0.001));

	R = CursorTest::Call(TEXT("material_add_expression"), *Args(TEXT("{\"material\":\"{0}\",\"type\":\"MaterialExpressionVectorParameter\",\"name\":\"Tint\",\"value\":[1,0,0,1]}"), Path));
	TestFalse(TEXT("add vector parameter by full class name: ") + R.Text, R.bIsError);
	const FString TintId = R.bIsError ? FString() : R.Structured->GetStringField(TEXT("name"));

	R = CursorTest::Call(TEXT("material_add_expression"), *Args(TEXT("{\"material\":\"{0}\",\"type\":\"Multiply\"}"), Path));
	TestFalse(TEXT("add Multiply: ") + R.Text, R.bIsError);
	const FString MultiplyId = R.bIsError ? FString() : R.Structured->GetStringField(TEXT("name"));
	TestTrue(TEXT("Multiply reports its inputs"), !R.bIsError && R.Text.Contains(TEXT("\"A\"")) && R.Text.Contains(TEXT("\"B\"")));

	R = CursorTest::Call(TEXT("material_add_expression"), *Args(TEXT("{\"material\":\"{0}\",\"type\":\"TextureSampleParameter2D\",\"name\":\"Albedo\",\"value\":\"/Engine/EngineResources/DefaultTexture\"}"), Path));
	TestFalse(TEXT("add texture parameter with a texture: ") + R.Text, R.bIsError);
	TestTrue(TEXT("the texture is reported"), !R.bIsError && R.Text.Contains(TEXT("DefaultTexture")));

	R = CursorTest::Call(TEXT("material_add_expression"), *Args(TEXT("{\"material\":\"{0}\",\"type\":\"Constant3Vector\",\"value\":[0.2,0.4,0.6]}"), Path));
	TestFalse(TEXT("add Constant3Vector: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("material_add_expression"), *Args(TEXT("{\"material\":\"{0}\",\"type\":\"Constant\",\"properties\":{\"R\":0.75}}"), Path));
	TestFalse(TEXT("generic properties are applied: ") + R.Text, R.bIsError);
	TestTrue(TEXT("and show in the description"), !R.bIsError && FMath::IsNearlyEqual(R.Structured->GetNumberField(TEXT("value")), 0.75, 0.001));

	R = CursorTest::Call(TEXT("material_add_expression"), *Args(TEXT("{\"material\":\"{0}\",\"type\":\"NotARealExpression\"}"), Path));
	TestTrue(TEXT("unknown expression type is an error that suggests some: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("Multiply")));
	R = CursorTest::Call(TEXT("material_add_expression"), *Args(TEXT("{\"material\":\"{0}\",\"type\":\"Multiply\",\"name\":\"NotAParameter\"}"), Path));
	TestTrue(TEXT("'name' on a non-parameter is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("material_add_expression"), *Args(TEXT("{\"material\":\"{0}\",\"type\":\"ScalarParameter\",\"name\":\"Bad\",\"value\":\"text\"}"), Path));
	TestTrue(TEXT("a wrong value type is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("material_get_info"), *Args(TEXT("{\"material\":\"{0}\"}"), Path));
	TestFalse(TEXT("get_info succeeds: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		TestEqual(TEXT("a rejected expression is not left in the graph"), R.Structured->GetArrayField(TEXT("expressions")).Num(), 6);
	}

	if (RoughId.IsEmpty() || TintId.IsEmpty() || MultiplyId.IsEmpty())
	{
		AddError(TEXT("Could not create the expressions the wiring checks need."));
		return false;
	}

	// ---- connections ----------------------------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("material_connect"), *Args(TEXT("{\"material\":\"{0}\",\"from\":\"{1}\",\"to_expression\":\"{2}\",\"to_input\":\"A\"}"), Path, TintId, MultiplyId));
	TestFalse(TEXT("connect an expression to an expression input: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("material_connect"), *Args(TEXT("{\"material\":\"{0}\",\"from\":\"{1}\",\"to_expression\":\"{2}\",\"to_input\":\"B\"}"), Path, RoughId, MultiplyId));
	TestFalse(TEXT("connect the second input: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("material_connect"), *Args(TEXT("{\"material\":\"{0}\",\"from\":\"{1}\",\"to_property\":\"EmissiveColor\"}"), Path, MultiplyId));
	TestFalse(TEXT("connect an expression to a material input: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("material_connect"), *Args(TEXT("{\"material\":\"{0}\",\"from\":\"{1}\",\"to_expression\":\"{2}\",\"to_input\":\"Nope\"}"), Path, TintId, MultiplyId));
	TestTrue(TEXT("an unknown input is an error that lists the inputs: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("A")));
	R = CursorTest::Call(TEXT("material_connect"), *Args(TEXT("{\"material\":\"{0}\",\"from\":\"{1}\",\"to_property\":\"Sparkle\"}"), Path, TintId));
	TestTrue(TEXT("an unknown material input is an error that lists the choices: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("BaseColor")));
	R = CursorTest::Call(TEXT("material_connect"), *Args(TEXT("{\"material\":\"{0}\",\"from\":\"{1}\"}"), Path, TintId));
	TestTrue(TEXT("a connection with no target is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("material_connect"), *Args(TEXT("{\"material\":\"{0}\",\"from\":\"{1}\",\"to_property\":\"BaseColor\",\"to_expression\":\"{1}\"}"), Path, TintId));
	TestTrue(TEXT("two targets at once is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("material_connect"), *Args(TEXT("{\"material\":\"{0}\",\"from\":\"{1}\",\"to_expression\":\"{1}\"}"), Path, MultiplyId));
	TestTrue(TEXT("an expression cannot feed itself"), R.bIsError);
	R = CursorTest::Call(TEXT("material_connect"), *Args(TEXT("{\"material\":\"{0}\",\"from\":\"NoSuchExpression\",\"to_property\":\"BaseColor\"}"), Path));
	TestTrue(TEXT("an unknown source expression is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("material_get_info"), *Args(TEXT("{\"material\":\"{0}\"}"), Path));
	if (!R.bIsError)
	{
		const TSharedPtr<FJsonObject> Connected = R.Structured->GetObjectField(TEXT("connected_inputs"));
		TestEqual(TEXT("EmissiveColor is driven by Multiply"), Connected->GetStringField(TEXT("EmissiveColor")), MultiplyId);
		const TSharedPtr<FJsonObject> Parameters = R.Structured->GetObjectField(TEXT("parameters"));
		TestEqual(TEXT("one scalar parameter is listed"), Parameters->GetArrayField(TEXT("scalar")).Num(), 1);
		TestEqual(TEXT("one vector parameter is listed"), Parameters->GetArrayField(TEXT("vector")).Num(), 1);
		TestEqual(TEXT("one texture parameter is listed"), Parameters->GetArrayField(TEXT("texture")).Num(), 1);
	}

	// ---- parameters on the base material --------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"rough\",\"value\":0.9}"), Path));
	TestFalse(TEXT("set a scalar default (name is case-insensitive): ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"Tint\",\"value\":[0,0,1,1]}"), Path));
	TestFalse(TEXT("set a vector default: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"Albedo\",\"value\":\"/Engine/EngineMaterials/DefaultWhiteGrid\"}"), Path));
	TestFalse(TEXT("set a texture default: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"NoSuchParam\",\"value\":1}"), Path));
	TestTrue(TEXT("an unknown parameter is an error that lists the existing ones: ") + R.Text, R.bIsError && R.Text.Contains(TEXT("Rough")));
	R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"Rough\",\"value\":[1,2,3]}"), Path));
	TestTrue(TEXT("a vector value for a scalar parameter is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"Rough\",\"value\":\"/Game/No/Such/Texture\",\"type\":\"texture\"}"), Path));
	TestTrue(TEXT("a missing texture is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("material_get_info"), *Args(TEXT("{\"material\":\"{0}\"}"), Path));
	if (!R.bIsError)
	{
		bool bRoughOk = false;
		bool bTintOk = false;
		bool bAlbedoOk = false;
		for (const TSharedPtr<FJsonValue>& Value : R.Structured->GetArrayField(TEXT("expressions")))
		{
			const TSharedPtr<FJsonObject> Expression = Value->AsObject();
			FString ParameterName;
			if (!Expression->TryGetStringField(TEXT("parameter_name"), ParameterName))
			{
				continue;
			}
			if (ParameterName == TEXT("Rough"))
			{
				bRoughOk = FMath::IsNearlyEqual(Expression->GetNumberField(TEXT("default_value")), 0.9, 0.001);
			}
			else if (ParameterName == TEXT("Tint"))
			{
				const TArray<TSharedPtr<FJsonValue>>& Color = Expression->GetArrayField(TEXT("default_value"));
				bTintOk = Color.Num() == 4 && FMath::IsNearlyEqual(Color[2]->AsNumber(), 1.0, 0.001) && FMath::IsNearlyEqual(Color[0]->AsNumber(), 0.0, 0.001);
			}
			else if (ParameterName == TEXT("Albedo"))
			{
				bAlbedoOk = Expression->GetStringField(TEXT("texture")).Contains(TEXT("DefaultWhiteGrid"));
			}
		}
		TestTrue(TEXT("scalar default changed"), bRoughOk);
		TestTrue(TEXT("vector default changed"), bTintOk);
		TestTrue(TEXT("texture default changed"), bAlbedoOk);
	}

	// ---- compile and save -----------------------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("material_compile"), *Args(TEXT("{\"material\":\"{0}\",\"layout\":true}"), Path));
	TestFalse(TEXT("the material compiles: ") + R.Text, R.bIsError);
	TestTrue(TEXT("compile reports an errors array"), R.Structured.IsValid() && R.Structured->HasField(TEXT("errors")));

	R = CursorTest::Call(TEXT("material_compile"), *Args(TEXT("{\"material\":\"{0}\",\"save\":true}"), Path));
	TestFalse(TEXT("compile with save: ") + R.Text, R.bIsError);
	TestTrue(TEXT("save reports success: ") + R.Text, R.Structured.IsValid() && R.Structured->HasField(TEXT("saved")) && R.Structured->GetBoolField(TEXT("saved")));
	TestTrue(TEXT("the file is on disk"), FPackageName::DoesPackageExist(Path));

	// ---- removal --------------------------------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("material_remove_expression"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"{1}\"}"), Path, MultiplyId));
	TestFalse(TEXT("remove an expression: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("material_remove_expression"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"{1}\"}"), Path, MultiplyId));
	TestTrue(TEXT("removing it again is an error"), R.bIsError);
	R = CursorTest::Call(TEXT("material_get_info"), *Args(TEXT("{\"material\":\"{0}\"}"), Path));
	TestTrue(TEXT("removing an expression clears what it drove"), !R.bIsError && !R.Structured->GetObjectField(TEXT("connected_inputs"))->HasField(TEXT("EmissiveColor")));

	// ---- instances ------------------------------------------------------------------------------------------------------
	const FString InstancePath = Assets.Add(TEXT("MI_CUTest"));
	R = CursorTest::Call(TEXT("material_create_instance"), *Args(TEXT("{\"path\":\"{0}\",\"parent\":\"{1}\"}"), InstancePath, Path));
	TestFalse(TEXT("create an instance: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"Rough\",\"value\":0.2}"), InstancePath));
		TestFalse(TEXT("override a scalar: ") + R.Text, R.bIsError);
		R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"Tint\",\"value\":{\"r\":0,\"g\":1,\"b\":0,\"a\":1}}"), InstancePath));
		TestFalse(TEXT("override a vector given as an object: ") + R.Text, R.bIsError);
		R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"Albedo\",\"value\":\"/Engine/EngineResources/DefaultTexture\"}"), InstancePath));
		TestFalse(TEXT("override a texture: ") + R.Text, R.bIsError);
		R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"NoSuchParam\",\"value\":1}"), InstancePath));
		TestTrue(TEXT("an instance refuses a parameter its parent does not have"), R.bIsError);

		R = CursorTest::Call(TEXT("material_get_info"), *Args(TEXT("{\"material\":\"{0}\"}"), InstancePath));
		TestFalse(TEXT("instance info succeeds: ") + R.Text, R.bIsError);
		if (!R.bIsError)
		{
			const TSharedPtr<FJsonObject> Overrides = R.Structured->GetObjectField(TEXT("overrides"));
			TestTrue(TEXT("scalar override recorded"), FMath::IsNearlyEqual(Overrides->GetNumberField(TEXT("Rough")), 0.2, 0.001));
			TestEqual(TEXT("vector override recorded"), Overrides->GetArrayField(TEXT("Tint"))[1]->AsNumber(), 1.0);
			TestTrue(TEXT("texture override recorded"), Overrides->GetStringField(TEXT("Albedo")).Contains(TEXT("DefaultTexture")));
			TestTrue(TEXT("the parent is reported"), R.Structured->GetStringField(TEXT("parent")).Contains(FPackageName::GetShortName(Path)));
		}
		R = CursorTest::Call(TEXT("material_compile"), *Args(TEXT("{\"material\":\"{0}\"}"), InstancePath));
		TestFalse(TEXT("an instance can be refreshed: ") + R.Text, R.bIsError);
		R = CursorTest::Call(TEXT("material_add_expression"), *Args(TEXT("{\"material\":\"{0}\",\"type\":\"Constant\"}"), InstancePath));
		TestTrue(TEXT("an instance has no expression graph to edit: ") + R.Text, R.bIsError);
	}

	// ---- reading and safety ---------------------------------------------------------------------------------------------
	R = CursorTest::Call(TEXT("material_get_info"), TEXT("{\"material\":\"/Engine/EngineMaterials/WorldGridMaterial\"}"));
	TestFalse(TEXT("engine materials can be read: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("material_set_parameter"), TEXT("{\"material\":\"/Engine/EngineMaterials/WorldGridMaterial\",\"name\":\"X\",\"value\":1}"));
	TestTrue(TEXT("engine materials cannot be changed: ") + R.Text, R.bIsError);
	R = CursorTest::Call(TEXT("material_get_info"), TEXT("{\"material\":\"/Engine/BasicShapes/Cube\"}"));
	TestTrue(TEXT("a non-material asset is an error"), R.bIsError);

	{
		CursorTest::FScopedPolicy ReadOnly(NAME_None, ECursorApprovalPolicy::ReadOnly);
		R = CursorTest::Call(TEXT("material_set_parameter"), *Args(TEXT("{\"material\":\"{0}\",\"name\":\"Rough\",\"value\":0.1}"), Path));
		TestTrue(TEXT("the read-only policy refuses material edits"), R.bIsError);
		R = CursorTest::Call(TEXT("material_get_info"), *Args(TEXT("{\"material\":\"{0}\"}"), Path));
		TestFalse(TEXT("the read-only policy allows reading"), R.bIsError);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
