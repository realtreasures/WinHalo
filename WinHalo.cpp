#include "Core/Application.h"
#include <exception>
#include <roapi.h>
#include <system_error>

namespace
{
struct Apartment
{
    Apartment()
    {
        const HRESULT result = RoInitialize(RO_INIT_SINGLETHREADED);
        if (FAILED(result)) throw std::system_error(static_cast<int>(result), std::system_category(), "RoInitialize");
    }
    ~Apartment() { RoUninitialize(); }
};
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    try
    {
        Apartment apartment;
        winisland::Application application(instance);
        return application.Run();
    }
    catch (const std::exception& error)
    {
        OutputDebugStringA(error.what());
        MessageBoxA(nullptr, error.what(), "WinHalo - startup failed", MB_OK | MB_ICONERROR);
        return 1;
    }
}
