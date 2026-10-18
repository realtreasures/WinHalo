#pragma once
#include "../Core/Platform.h"
#include <system_error>

namespace winisland
{
[[noreturn]] inline void ThrowLastError(const char* operation)
{
    const DWORD code = GetLastError();
    throw std::system_error(static_cast<int>(code ? code : ERROR_GEN_FAILURE),
        std::system_category(), operation);
}
inline void CheckWin32(BOOL success, const char* operation)
{
    if (!success) ThrowLastError(operation);
}
}
