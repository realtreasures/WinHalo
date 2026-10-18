#pragma once
#include "../Services/MediaInterop.h"
#include <winrt/Windows.Media.Control.h>
#include <atomic>
#include <stdexcept>

namespace media_interop_tests
{
using Properties = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties;
using Timeline = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionTimelineProperties;
using Operation = winrt::Windows::Foundation::IAsyncOperation<Properties>;

template<class Object> struct Stub : winisland::media_interop::Abi<Object>
{
    std::atomic<uint32_t> references{ 1 };
    int32_t __stdcall QueryInterface(const winrt::guid& iid, void** result) noexcept override
    {
        *result = nullptr;
        if (iid != winrt::guid_of<winrt::default_interface<Object>>() &&
            iid != winrt::guid_of<winrt::Windows::Foundation::IUnknown>() &&
            iid != winrt::guid_of<winrt::Windows::Foundation::IInspectable>()) return E_NOINTERFACE;
        *result = this; AddRef(); return S_OK;
    }
    uint32_t __stdcall AddRef() noexcept override { return ++references; }
    uint32_t __stdcall Release() noexcept override
    {
        const auto left = --references;
        if (!left) delete this;
        return left;
    }
    int32_t __stdcall GetIids(uint32_t* count, winrt::guid** ids) noexcept override
    { *count = 0; *ids = nullptr; return S_OK; }
    int32_t __stdcall GetRuntimeClassName(void** name) noexcept override { *name = nullptr; return S_OK; }
    int32_t __stdcall GetTrustLevel(winrt::Windows::Foundation::TrustLevel* level) noexcept override
    { *level = winrt::Windows::Foundation::TrustLevel::BaseTrust; return S_OK; }
protected:
    virtual ~Stub() = default;
};

struct DisconnectedTimeline final : Stub<Timeline>
{
    int32_t __stdcall get_StartTime(int64_t* value) noexcept override { *value = 0; return RPC_E_DISCONNECTED; }
    int32_t __stdcall get_EndTime(int64_t* value) noexcept override { *value = 0; return RPC_E_DISCONNECTED; }
    int32_t __stdcall get_MinSeekTime(int64_t* value) noexcept override { *value = 0; return RPC_E_DISCONNECTED; }
    int32_t __stdcall get_MaxSeekTime(int64_t* value) noexcept override { *value = 0; return RPC_E_DISCONNECTED; }
    int32_t __stdcall get_Position(int64_t* value) noexcept override { *value = 0; return RPC_E_DISCONNECTED; }
    int32_t __stdcall get_LastUpdatedTime(int64_t* value) noexcept override { *value = 0; return RPC_E_DISCONNECTED; }
};

struct FailedOperation final : Stub<Operation>
{
    int32_t __stdcall put_Completed(void* value) noexcept override
    {
        winrt::Windows::Foundation::AsyncOperationCompletedHandler<Properties> callback{ nullptr };
        winrt::copy_from_abi(callback, value);
        AddRef();
        const Operation operation{ this, winrt::take_ownership_from_abi };
        callback(operation, winrt::Windows::Foundation::AsyncStatus::Error);
        return S_OK;
    }
    int32_t __stdcall get_Completed(void** value) noexcept override { *value = nullptr; return S_OK; }
    int32_t __stdcall GetResults(void** value) noexcept override { *value = nullptr; return RO_E_CLOSED; }
};

inline std::atomic<unsigned int> exceptions{ 0 };
inline LONG CALLBACK Observe(EXCEPTION_POINTERS* info) noexcept
{
    if (info->ExceptionRecord->ExceptionCode == 0xE06D7363) ++exceptions;
    return EXCEPTION_CONTINUE_SEARCH;
}

inline void Run()
{
    using namespace winisland::media_interop;
    const Timeline timeline{ new DisconnectedTimeline, winrt::take_ownership_from_abi };
    const Operation operation{ new FailedOperation, winrt::take_ownership_from_abi };
    void* observer = AddVectoredExceptionHandler(1, Observe);
    if (!observer) throw std::runtime_error("Cannot observe first-chance exceptions");
    exceptions = 0;
    winrt::Windows::Foundation::TimeSpan position{};
    HRESULT code = E_FAIL;
    AsyncResult<Properties> result;
    try
    {
        code = Read(timeline, &Abi<Timeline>::get_Position, position);
        result = Await(operation, {}, std::chrono::steady_clock::now() + std::chrono::seconds(1));
    }
    catch (...) { RemoveVectoredExceptionHandler(observer); throw; }
    RemoveVectoredExceptionHandler(observer);
    if (code != RPC_E_DISCONNECTED || result.code != RO_E_CLOSED || result.value || exceptions != 0)
        throw std::runtime_error("Media shutdown HRESULT became a first-chance C++ exception");
    const Operation missing{ nullptr };
    if (Await(missing, {}, std::chrono::steady_clock::now()).code != RO_E_CLOSED)
        throw std::runtime_error("Missing async operation was not handled");
    std::stop_source stopped; stopped.request_stop();
    if (Await(operation, stopped.get_token(), std::chrono::steady_clock::now()).code != HRESULT_FROM_WIN32(ERROR_CANCELLED))
        throw std::runtime_error("Cancellation was not handled as a return code");
}
}
