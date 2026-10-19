#pragma once
#include "../Core/Platform.h"
#include <string_view>

namespace winisland
{
inline bool englishLanguage = false;
inline void SetUiLanguage(std::wstring_view language) noexcept
{
    englishLanguage = language == L"en-US" || (language == L"system" &&
        PRIMARYLANGID(GetUserDefaultUILanguage()) != LANG_CHINESE);
}
inline const wchar_t* UiText(const wchar_t* chinese, const wchar_t* english) noexcept
{
    return englishLanguage ? english : chinese;
}
}
