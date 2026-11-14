#pragma once

// Engine-version shims. Anything that differs between 5.x releases goes through here so the
// rest of the plugin never tests ENGINE_MINOR_VERSION directly.

#include "Runtime/Launch/Resources/Version.h"

#define CURSORUNREAL_UE_AT_LEAST(Major, Minor) \
	(ENGINE_MAJOR_VERSION > (Major) || (ENGINE_MAJOR_VERSION == (Major) && ENGINE_MINOR_VERSION >= (Minor)))

// IHttpRequest::SetResponseBodyReceiveStreamDelegateV2 arrived in 5.5; the original delegate
// (which returns bool instead of taking the length by reference) is deprecated from then on.
#define CURSORUNREAL_HAS_HTTP_STREAM_DELEGATE_V2 CURSORUNREAL_UE_AT_LEAST(5, 5)
