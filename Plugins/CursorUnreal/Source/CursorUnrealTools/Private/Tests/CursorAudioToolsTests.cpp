#if WITH_DEV_AUTOMATION_TESTS

#include "CursorPackTestSupport.h"

using namespace CursorPackTest;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCursorAudioToolsTest, "CursorUnreal.Tools.Audio", CursorTest::Flags)
bool FCursorAudioToolsTest::RunTest(const FString&)
{
	CursorTest::FScopedPolicy Policy(NAME_None, ECursorApprovalPolicy::AllowAll);
	FScopedTestAssets Assets;
	const FString Path = Assets.Add(TEXT("SC_CUTest"));
	const FString AttenPath = Assets.Add(TEXT("SA_CUTest"));

	FCursorToolResult R = CursorTest::Call(TEXT("sound_create_cue"), *Args(TEXT("{\"path\":\"{0}\"}"), Path));
	TestFalse(TEXT("sound_create_cue succeeds: ") + R.Text, R.bIsError);
	if (R.bIsError)
	{
		return false;
	}

	R = CursorTest::Call(TEXT("sound_create_cue"), *Args(TEXT("{\"path\":\"{0}\"}"), Path));
	TestTrue(TEXT("duplicate create is an error"), R.bIsError);

	R = CursorTest::Call(TEXT("sound_get_info"), *Args(TEXT("{\"sound\":\"{0}\"}"), Path));
	TestFalse(TEXT("get_info succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("sound_create_attenuation"), *Args(TEXT("{\"path\":\"{0}\"}"), AttenPath));
	TestFalse(TEXT("sound_create_attenuation succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("sound_set_attenuation"),
		*Args(TEXT("{\"sound\":\"{0}\",\"attenuation\":\"{1}\"}"), Path, AttenPath));
	TestFalse(TEXT("sound_set_attenuation succeeds: ") + R.Text, R.bIsError);

	R = CursorTest::Call(TEXT("sound_play"), *Args(TEXT("{\"sound\":\"{0}\"}"), Path));
	// Headless / no-audio editors return an error; that is acceptable.
	if (R.bIsError)
	{
		AddWarning(TEXT("sound_play skipped (no editor audio device): ") + R.Text);
	}

	R = CursorTest::Call(TEXT("sound_stop"), TEXT("{}"));
	TestFalse(TEXT("sound_stop does not error: ") + R.Text, R.bIsError);

	return true;
}

#endif
