#if WITH_DEV_AUTOMATION_TESTS

#include "CursorPackTestSupport.h"

using namespace CursorPackTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorOptimisationToolsTest, "CursorUnreal.Tools.Optimisation", CursorTest::Flags)
bool FCursorOptimisationToolsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);

	const FString Cube = TEXT("/Engine/BasicShapes/Cube");

	FCursorToolResult R = CursorTest::Call(TEXT("optim_level_stats"), TEXT("{}"));
	TestFalse(TEXT("optim_level_stats succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("optim_list_heavy_actors"), TEXT("{\"limit\":5}"));
	TestFalse(TEXT("optim_list_heavy_actors succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("optim_mesh_info"), *Args(TEXT("{\"mesh\":\"{0}\"}"), Cube));
	TestFalse(TEXT("optim_mesh_info on engine cube: ") + R.Text, R.bIsError);
	if (!R.bIsError)
	{
		TestTrue(TEXT("reports at least one LOD"), R.Structured->GetNumberField(TEXT("lod_count")) >= 1);
	}

	R = CursorTest::Call(TEXT("optim_suggest_lods"), *Args(TEXT("{\"mesh\":\"{0}\"}"), Cube));
	TestFalse(TEXT("optim_suggest_lods succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("optim_suggest_lods"), TEXT("{\"mesh\":\"/Game/DoesNotExist/NoMesh\"}"));
	TestTrue(TEXT("missing mesh is an error"), R.bIsError);

	return true;
}

#endif
