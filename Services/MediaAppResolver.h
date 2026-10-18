#pragma once
#include "MediaTypes.h"

namespace winisland
{
// Resolve once on the media worker; immutable icon bytes are shared with the UI.
MediaAppInfo ResolveMediaApp(const std::wstring& source);
bool OpenMediaApp(const MediaAppInfo& app);
}
