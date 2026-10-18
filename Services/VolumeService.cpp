#include "VolumeService.h"
#include "../Core/Platform.h"
#include "../Utils/Logger.h"
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <limits>
#include <thread>

namespace winisland
{
namespace
{
using Microsoft::WRL::ComPtr;

std::wstring AudioError(const wchar_t* operation, HRESULT result)
{
    wchar_t code[16]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned int>(result));
    return std::wstring(operation) + L"（" + code + L"）";
}

// 回调只持有共享信箱，不接触 VolumeService 或 UI 的裸指针。
// 自动复位事件能保留先于 worker 等待发生的通知，不会丢失唤醒。
struct Mailbox
{
    Mailbox() : wake(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~Mailbox() { if (wake) CloseHandle(wake); }
    void Wake() const noexcept { if (wake) SetEvent(wake); }
    HANDLE wake = nullptr;
    std::atomic<bool> accepting{ true };
    std::atomic<bool> rebind{ true };
    std::atomic<float> requestedLevel{ std::numeric_limits<float>::quiet_NaN() };
    std::atomic<unsigned int> muteRequests{ 0 };
};

struct EndpointSignal
{
    std::atomic<bool> active{ true };
    std::atomic<float> level{ 0 };
    std::atomic<bool> muted{ false };
    std::atomic<unsigned long long> sequence{ 0 };
};

class EndpointCallback final : public IAudioEndpointVolumeCallback
{
public:
    EndpointCallback(std::shared_ptr<Mailbox> mailbox, std::shared_ptr<EndpointSignal> signal)
        : mailbox_(std::move(mailbox)), signal_(std::move(signal)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IAudioEndpointVolumeCallback)) return E_NOINTERFACE;
        *object = static_cast<IAudioEndpointVolumeCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override
    {
        if (!data) return E_POINTER;
        if (!signal_->active.load(std::memory_order_acquire)) return S_OK;
        // Core Audio 要求回调非阻塞：这里没有锁、日志、COM 查询或 UI 调用。
        signal_->level.store(data->fMasterVolume, std::memory_order_relaxed);
        signal_->muted.store(data->bMuted != FALSE, std::memory_order_relaxed);
        signal_->sequence.fetch_add(1, std::memory_order_release);
        mailbox_->Wake();
        return S_OK;
    }
private:
    std::atomic<ULONG> references_{ 1 };
    std::shared_ptr<Mailbox> mailbox_;
    std::shared_ptr<EndpointSignal> signal_;
};

class DeviceCallback final : public IMMNotificationClient
{
public:
    explicit DeviceCallback(std::shared_ptr<Mailbox> mailbox) : mailbox_(std::move(mailbox)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMMNotificationClient)) return E_NOINTERFACE;
        *object = static_cast<IMMNotificationClient*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override
    {
        if (flow == eRender && role == eMultimedia) Changed();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { Changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { Changed(); return S_OK; }
private:
    void Changed() const noexcept
    {
        mailbox_->rebind.store(true, std::memory_order_release);
        mailbox_->Wake();
    }
    std::atomic<ULONG> references_{ 1 };
    std::shared_ptr<Mailbox> mailbox_;
};

struct DeviceMonitor
{
    ~DeviceMonitor() { Close(); }
    void Close() noexcept
    {
        if (registered) enumerator->UnregisterEndpointNotificationCallback(callback.Get());
        registered = false;
        enumerator.Reset();
        callback.Reset();
    }
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<DeviceCallback> callback;
    bool registered = false;
};

struct Endpoint
{
    ~Endpoint() { Close(); }
    void Close() noexcept
    {
        // 先禁止旧设备回调，再由 worker 撤销注册，最后释放 COM 对象。
        if (signal) signal->active.store(false, std::memory_order_release);
        if (registered) volume->UnregisterControlChangeNotify(callback.Get());
        registered = false;
        volume.Reset();
        callback.Reset();
        signal.reset();
        id.clear();
        name.clear();
        sequence = 0;
    }
    ComPtr<IAudioEndpointVolume> volume;
    ComPtr<EndpointCallback> callback;
    std::shared_ptr<EndpointSignal> signal;
    std::wstring id;
    std::wstring name;
    unsigned long long sequence = 0;
    bool registered = false;
};

std::wstring DeviceName(IMMDevice* device)
{
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return L"默认音频输出";
    PROPVARIANT value{};
    const HRESULT result = properties->GetValue(PKEY_Device_FriendlyName, &value);
    struct ClearValue
    {
        PROPVARIANT& value;
        ~ClearValue() { PropVariantClear(&value); }
    } clear{ value };
    std::wstring name;
    if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal) name = value.pwszVal;
    return name.empty() ? L"默认音频输出" : name;
}

HRESULT BindEndpoint(DeviceMonitor& monitor, Endpoint& endpoint, const std::shared_ptr<Mailbox>& mailbox)
{
    if (!monitor.enumerator)
    {
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&monitor.enumerator));
        if (FAILED(result)) return result;
        monitor.callback.Attach(new DeviceCallback(mailbox));
    }
    if (!monitor.registered)
    {
        const HRESULT result = monitor.enumerator->RegisterEndpointNotificationCallback(monitor.callback.Get());
        if (FAILED(result)) return result;
        monitor.registered = true;
    }
    ComPtr<IMMDevice> device;
    HRESULT result = monitor.enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (FAILED(result)) { endpoint.Close(); return result; }
    LPWSTR rawId = nullptr;
    result = device->GetId(&rawId);
    if (FAILED(result)) { endpoint.Close(); return result; }
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> releaseId(rawId, CoTaskMemFree);
    std::wstring id(rawId ? rawId : L"");
    const auto name = DeviceName(device.Get());
    if (endpoint.volume && endpoint.id == id)
    {
        endpoint.name = name;
        return S_OK;
    }
    endpoint.Close();
    result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(endpoint.volume.GetAddressOf()));
    if (FAILED(result)) return result;
    endpoint.signal = std::make_shared<EndpointSignal>();
    endpoint.callback.Attach(new EndpointCallback(mailbox, endpoint.signal));
    result = endpoint.volume->RegisterControlChangeNotify(endpoint.callback.Get());
    if (FAILED(result)) { endpoint.Close(); return result; }
    endpoint.registered = true;
    endpoint.id = std::move(id);
    endpoint.name = name;
    return S_OK;
}

HRESULT ReadEndpoint(Endpoint& endpoint, VolumeSnapshot& snapshot)
{
    // 记录查询前的序列号，查询期间发生的新通知留待下一轮处理。
    endpoint.sequence = endpoint.signal->sequence.load(std::memory_order_acquire);
    float level = 0;
    BOOL muted = FALSE;
    HRESULT result = endpoint.volume->GetMasterVolumeLevelScalar(&level);
    if (SUCCEEDED(result)) result = endpoint.volume->GetMute(&muted);
    if (FAILED(result)) return result;
    snapshot.available = true;
    snapshot.level = std::clamp(level, 0.0f, 1.0f);
    snapshot.muted = muted != FALSE;
    snapshot.deviceName = endpoint.name;
    return S_OK;
}

void RunVolumeWorker(Logger& logger, std::shared_ptr<Mailbox> mailbox,
    std::function<void(VolumeSnapshot)> sink, std::stop_token stop)
{
    struct StopAccepting
    {
        Mailbox& mailbox;
        ~StopAccepting() { mailbox.accepting.store(false, std::memory_order_release); }
    } stopAccepting{ *mailbox };
    bool first = true;
    VolumeSnapshot previous;
    auto publish = [&](VolumeSnapshot snapshot)
    {
        if (stop.stop_requested()) return;
        const bool changed = first || snapshot.available != previous.available || snapshot.muted != previous.muted
            || std::abs(snapshot.level - previous.level) > 0.00001f
            || snapshot.deviceName != previous.deviceName || snapshot.error != previous.error;
        if (!changed) return;
        snapshot.notify = !first && (snapshot.available || previous.available);
        if (first || snapshot.available != previous.available)
            logger.Write(snapshot.available ? "Core Audio default output available" : "Core Audio default output unavailable");
        first = false;
        previous = snapshot;
        try { if (sink) sink(std::move(snapshot)); }
        catch (...) { logger.Write("Volume snapshot receiver failed"); }
    };
    if (!mailbox->wake)
    {
        VolumeSnapshot snapshot;
        snapshot.error = L"无法创建音量工作线程事件";
        publish(std::move(snapshot));
        return;
    }
    std::stop_callback wakeOnStop(stop, [mailbox] { mailbox->Wake(); });
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized))
    {
        VolumeSnapshot snapshot;
        snapshot.error = AudioError(L"无法初始化 Core Audio", initialized);
        publish(std::move(snapshot));
        return;
    }
    // 此作用域内的 COM 对象先全部析构，然后才 CoUninitialize。
    try
    {
        DeviceMonitor monitor;
        Endpoint endpoint;
        bool retry = true;
        while (!stop.stop_requested())
        {
            bool refresh = false;
            HRESULT failure = S_OK;
            if (mailbox->rebind.exchange(false, std::memory_order_acq_rel) || retry)
            {
                failure = BindEndpoint(monitor, endpoint, mailbox);
                refresh = true;
            }
            const float requested = mailbox->requestedLevel.exchange(
                std::numeric_limits<float>::quiet_NaN(), std::memory_order_acq_rel);
            const unsigned int toggles = mailbox->muteRequests.exchange(0, std::memory_order_acq_rel);
            if (endpoint.volume && std::isfinite(requested))
            {
                failure = endpoint.volume->SetMasterVolumeLevelScalar(requested, nullptr);
                refresh = true;
            }
            if (endpoint.volume && SUCCEEDED(failure) && (toggles & 1u))
            {
                BOOL muted = FALSE;
                failure = endpoint.volume->GetMute(&muted);
                if (SUCCEEDED(failure)) failure = endpoint.volume->SetMute(!muted, nullptr);
                refresh = true;
            }
            if (endpoint.signal && endpoint.signal->sequence.load(std::memory_order_acquire) != endpoint.sequence)
                refresh = true;
            if (refresh)
            {
                VolumeSnapshot snapshot;
                if (endpoint.volume && SUCCEEDED(failure)) failure = ReadEndpoint(endpoint, snapshot);
                if (FAILED(failure))
                {
                    endpoint.Close();
                    // 正常的“没有设备”保留通知注册；服务/代理故障重新创建枚举器。
                    if (failure != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) monitor.Close();
                    snapshot = {};
                    snapshot.error = AudioError(L"音频输出暂不可用", failure);
                }
                publish(std::move(snapshot));
            }
            retry = !endpoint.volume;
            // 正常运行没有轮询；服务停止/设备变化/音量变化/控制命令会唤醒。
            // 无设备时每两秒重试，覆盖音频服务重启等没有设备通知的情况。
            const DWORD waited = WaitForSingleObject(mailbox->wake, retry ? 2000 : INFINITE);
            if (waited == WAIT_FAILED) break;
        }
    }
    catch (...)
    {
        VolumeSnapshot snapshot;
        snapshot.error = L"Core Audio 工作线程异常";
        publish(std::move(snapshot));
        logger.Write("Core Audio worker exception");
    }
    CoUninitialize();
}
}

struct VolumeService::Impl
{
    explicit Impl(Logger& value) : logger(value) {}
    Logger& logger;
    std::shared_ptr<Mailbox> mailbox;
    std::jthread worker;
};

VolumeService::VolumeService(Logger& logger) : impl_(std::make_unique<Impl>(logger)) {}
VolumeService::~VolumeService() { Stop(); }

void VolumeService::Start(std::function<void(VolumeSnapshot)> sink)
{
    Stop();
    auto mailbox = std::make_shared<Mailbox>();
    impl_->mailbox = mailbox;
    Logger* logger = &impl_->logger;
    impl_->worker = std::jthread([logger, mailbox, sink = std::move(sink)](std::stop_token stop) mutable
        { RunVolumeWorker(*logger, std::move(mailbox), std::move(sink), stop); });
}

void VolumeService::Stop() noexcept
{
    if (impl_->mailbox) impl_->mailbox->accepting.store(false, std::memory_order_release);
    if (impl_->worker.joinable())
    {
        impl_->worker.request_stop();
        if (impl_->mailbox) impl_->mailbox->Wake();
        impl_->worker.join();
    }
    impl_->mailbox.reset();
}

void VolumeService::SetLevel(float level)
{
    if (!std::isfinite(level)) return;
    const auto mailbox = impl_->mailbox;
    if (!mailbox || !mailbox->accepting.load(std::memory_order_acquire)) return;
    mailbox->requestedLevel.store(std::clamp(level, 0.0f, 1.0f), std::memory_order_release);
    mailbox->Wake();
}

void VolumeService::ToggleMute()
{
    const auto mailbox = impl_->mailbox;
    if (!mailbox || !mailbox->accepting.load(std::memory_order_acquire)) return;
    mailbox->muteRequests.fetch_add(1, std::memory_order_release);
    mailbox->Wake();
}
}
