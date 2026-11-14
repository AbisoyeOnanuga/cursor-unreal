#if WITH_DEV_AUTOMATION_TESTS

#include "CursorPackTestSupport.h"

using namespace CursorPackTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAnimationToolsTest, "CursorUnreal.Tools.Animation", CursorTest::Flags)
bool FCursorAnimationToolsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedTestAssets Assets;
	const FString Path = Assets.Add(TEXT("ABP_CUTest"));
	const FString Skeleton = TEXT("/Engine/EditorMeshes/SkeletalMesh/DefaultSkeletalMesh_Skeleton");

	FCursorToolResult R = CursorTest::Call(TEXT("animbp_create"),
		*Args(TEXT("{\"path\":\"{0}\",\"skeleton\":\"{1}\"}"), Path, Skeleton));
	TestFalse(TEXT("animbp_create succeeds: ") + R.Text, R.bIsError);
	if (R.bIsError)
	{
		AddWarning(TEXT("Skipping remaining AnimBP checks; skeleton may be unavailable headless."));
		return true;
	}

	R = CursorTest::Call(TEXT("animbp_create"), *Args(TEXT("{\"path\":\"{0}\",\"skeleton\":\"{1}\"}"), Path, Skeleton));
	TestTrue(TEXT("duplicate create is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("animbp_get_info"), *Args(TEXT("{\"animbp\":\"{0}\"}"), Path));
	TestFalse(TEXT("get_info succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("animbp_list_graphs"), *Args(TEXT("{\"animbp\":\"{0}\"}"), Path));
	TestFalse(TEXT("list_graphs succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("animbp_list_nodes"), *Args(TEXT("{\"animbp\":\"{0}\"}"), Path));
	TestFalse(TEXT("list_nodes succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("animbp_compile"), *Args(TEXT("{\"animbp\":\"{0}\"}"), Path));
	TestFalse(TEXT("compile succeeds: ") + R.Text, R.bIsError);

	return true;
}

#endif
