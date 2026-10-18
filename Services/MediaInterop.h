#pragma once
#include "../Core/Platform.h"
#include <winrt/Windows.Foundation.h>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <type_traits>
#include <utility>

namespace winisland::media_interop
{
template<class T> using Abi = winrt::impl::abi_t<winrt::default_interface<std::remove_cvref_t<T>>>;

template<class T> T Empty() noexcept
{
    if constexpr (std::is_constructible_v<T, std::nullptr_t>) return T{ nullptr };
    else return T{};
}

// Player shutdown is an expected COM failure. Preserve HRESULT instead of
// converting it into a C++ exception that breaks before the worker's catch.
template<class Object, class Method, class Result, class... Args>
HRESULT Read(const Object& object, Method method, Result& result, Args&&... args) noexcept
{
    result = Empty<Result>();
    if (!object) return RO_E_CLOSED;
    const HRESULT code = (static_cast<Abi<Object>*>(winrt::get_abi(object))->*method)(
        std::forward<Args>(args)..., winrt::put_abi(result));
    if (FAILED(code)) result = Empty<Result>();
    return code;
}

template<class Result> struct AsyncResult
{
    HRESULT code = S_OK;
    Result value = Empty<Result>();
};

struct Completion
{
    std::mutex mutex;
    std::condition_variable_any ready;
    bool done = false;
};

template<class Object> void Close(const Object& object) noexcept
{
    if (object)
        if (const auto closeable = object.template try_as<winrt::Windows::Foundation::IClosable>())
            static_cast<Abi<decltype(closeable)>*>(winrt::get_abi(closeable))->Close();
}

template<class Object, class Method, class Callback, class Revoker>
HRESULT Subscribe(const Object& object, Method method, const Callback& callback, Revoker& revoker)
{
    if (!object) return RO_E_CLOSED;
    winrt::event_token token{};
    const HRESULT code = (static_cast<Abi<Object>*>(winrt::get_abi(object))->*method)(winrt::get_abi(callback), &token);
    if (SUCCEEDED(code)) revoker = Revoker{ object, token };
    return code;
}

template<class Operation>
auto Await(const Operation& operation, std::stop_token stop, std::chrono::steady_clock::time_point deadline)
    -> AsyncResult<decltype(operation.GetResults())>
{
    using Result = decltype(operation.GetResults());
    if (!operation) return { RO_E_CLOSED };
    auto completion = std::make_shared<Completion>();
    const decltype(operation.Completed()) callback{
        [completion](const auto&, auto) noexcept
        {
            { const std::lock_guard lock(completion->mutex); completion->done = true; }
            completion->ready.notify_all();
        }};
    auto* abi = static_cast<Abi<Operation>*>(winrt::get_abi(operation));
    const HRESULT registered = abi->put_Completed(winrt::get_abi(callback));
    if (FAILED(registered)) return { registered };
    std::unique_lock lock(completion->mutex);
    const bool done = completion->ready.wait_until(lock, stop, deadline, [&] { return completion->done; });
    lock.unlock();
    if (!done || stop.stop_requested())
    {
        if (const auto info = operation.template try_as<winrt::Windows::Foundation::IAsyncInfo>())
            static_cast<Abi<decltype(info)>*>(winrt::get_abi(info))->Cancel();
        return { stop.stop_requested() ? HRESULT_FROM_WIN32(ERROR_CANCELLED) : HRESULT_FROM_WIN32(ERROR_TIMEOUT) };
    }
    AsyncResult<Result> result;
    result.code = Read(operation, &Abi<Operation>::GetResults, result.value);
    return result;
}
}
