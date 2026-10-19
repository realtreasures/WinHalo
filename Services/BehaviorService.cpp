#include "BehaviorService.h"
#include "../Core/Platform.h"
#include "../Utils/Logger.h"
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <wbemidl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <vector>

namespace winisland
{
namespace
{
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

struct Mailbox
{
    void Wake() noexcept { version.fetch_add(1, std::memory_order_release); changed.notify_one(); }
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<unsigned long long> version{0};
    std::atomic<bool> microphoneDirty{false};
    std::wstring device;
    unsigned long long configuration = 0;
    int adjustment = 0;
    bool brightness = false, privacy = false, stopping = true;
};

bool Equal(const BehaviorSnapshot& a, const BehaviorSnapshot& b) noexcept
{
    return a.brightnessAvailable == b.brightnessAvailable && a.brightness == b.brightness &&
        a.cameraAvailable == b.cameraAvailable && a.microphoneAvailable == b.microphoneAvailable &&
        a.cameraActive == b.cameraActive && a.microphoneActive == b.microphoneActive &&
        a.brightnessStatus == b.brightnessStatus && a.privacyStatus == b.privacyStatus;
}

std::wstring Error(const wchar_t* message, HRESULT result)
{
    wchar_t code[16]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned int>(result));
    return std::wstring(message) + L"（" + code + L"）";
}

HRESULT LastFailure() noexcept
{
    const DWORD error = GetLastError();
    return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE);
}

struct Bstr
{
    explicit Bstr(const wchar_t* text) : value(SysAllocString(text)) { if (!value) throw std::bad_alloc(); }
    ~Bstr() { SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR value;
};
struct Variant
{
    Variant() { VariantInit(&value); }
    ~Variant() { VariantClear(&value); }
    Variant(const Variant&) = delete;
    Variant& operator=(const Variant&) = delete;
    VARIANT value;
};

std::wstring StringProperty(IWbemClassObject* object, const wchar_t* property)
{
    Variant value;
    if (FAILED(object->Get(property, 0, &value.value, nullptr, nullptr)) ||
        value.value.vt != VT_BSTR || !value.value.bstrVal) return {};
    return value.value.bstrVal;
}

int IntegerProperty(IWbemClassObject* object, const wchar_t* property, int fallback = -1)
{
    Variant value;
    if (FAILED(object->Get(property, 0, &value.value, nullptr, nullptr))) return fallback;
    switch (value.value.vt)
    {
    case VT_UI1: return value.value.bVal;
    case VT_UI2: return value.value.uiVal;
    case VT_I2: return value.value.iVal;
    case VT_I4: return value.value.lVal;
    case VT_UI4: return value.value.ulVal <= 0x7FFFFFFFu ? static_cast<int>(value.value.ulVal) : fallback;
    case VT_BOOL: return value.value.boolVal != VARIANT_FALSE;
    default: return fallback;
    }
}

std::wstring Upper(std::wstring text)
{
    for (auto& character : text) character = static_cast<wchar_t>(std::towupper(character));
    return text;
}

struct SelectedMonitor
{
    std::wstring requested, device, instance;
    HMONITOR monitor = nullptr;
    bool ambiguous = false;
};

BOOL CALLBACK FindMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) noexcept
{
    auto& selected = *reinterpret_cast<SelectedMonitor*>(context);
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info))) return TRUE;
    if ((!selected.requested.empty() && _wcsicmp(selected.requested.c_str(), info.szDevice) == 0) ||
        (selected.requested.empty() && (info.dwFlags & MONITORINFOF_PRIMARY)))
    {
        selected.monitor = monitor;
        // A fixed-size display name avoids allocations escaping this Win32 callback.
        try { selected.device = info.szDevice; } catch (...) { selected.monitor = nullptr; }
        return FALSE;
    }
    return TRUE;
}

SelectedMonitor ResolveMonitor(const std::wstring& requested)
{
    SelectedMonitor selected; selected.requested = requested;
    EnumDisplayMonitors(nullptr, nullptr, FindMonitor, reinterpret_cast<LPARAM>(&selected));
    if (!selected.monitor) return selected;
    // Device-interface IDs map to WMI InstanceName, unlike the adapter's friendly
    // name. Match exactly except for WMI's trailing _N; never use the first laptop
    // panel merely because an unrelated external display is selected.
    for (DWORD index = 0; index < 32; ++index)
    {
        DISPLAY_DEVICEW device{}; device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(selected.device.c_str(), index, &device, EDD_GET_DEVICE_INTERFACE_NAME)) break;
        if (!(device.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
        auto path = Upper(device.DeviceID);
        if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
        const auto suffix = path.find(L"#{");
        if (suffix != std::wstring::npos) path.resize(suffix);
        std::replace(path.begin(), path.end(), L'#', L'\\');
        if (path.rfind(L"DISPLAY\\", 0) != 0) continue;
        if (!selected.instance.empty() && selected.instance != path) selected.ambiguous = true;
        selected.instance = std::move(path);
    }
    if (selected.ambiguous) selected.instance.clear();
    return selected;
}

class Brightness
{
public:
    ~Brightness() { Close(); }
    void Close() noexcept
    {
        if (!physical_.empty()) DestroyPhysicalMonitors(static_cast<DWORD>(physical_.size()), physical_.data());
        physical_.clear(); services_.Reset(); methodInputs_.Reset();
        path_.clear(); methodPath_.clear(); levels_.clear(); device_.clear(); monitor_ = nullptr; ddc_ = false;
    }
    bool Bound() const noexcept { return ddc_ || !path_.empty(); }

    HRESULT Bind(const std::wstring& device, std::wstring& status)
    {
        Close();
        const auto selected = ResolveMonitor(device);
        if (!selected.monitor) { status = L"未找到选定显示器"; return HRESULT_FROM_WIN32(ERROR_NOT_FOUND); }
        device_ = selected.device; monitor_ = selected.monitor;
        if (selected.ambiguous) { status = L"镜像显示无法唯一识别亮度目标"; return E_NOTIMPL; }
        const HRESULT wmi = BindWmi(selected.instance);
        if (SUCCEEDED(wmi)) { status = L"系统 WMI 亮度"; return S_OK; }
        DWORD count = 0;
        if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor_, &count) || count != 1)
        {
            status = count > 1 ? L"多个物理屏幕映射到同一显示器，无法安全调节" : L"所选显示器不支持亮度控制";
            return E_NOTIMPL;
        }
        physical_.resize(count);
        if (!GetPhysicalMonitorsFromHMONITOR(monitor_, count, physical_.data()))
        {
            physical_.clear(); status = L"无法访问显示器 DDC/CI"; return LastFailure();
        }
        DWORD capabilities = 0, temperatures = 0;
        if (!GetMonitorCapabilities(physical_[0].hPhysicalMonitor, &capabilities, &temperatures) ||
            !(capabilities & MC_CAPS_BRIGHTNESS))
        {
            status = L"显示器不支持亮度控制，请检查 DDC/CI"; return E_NOTIMPL;
        }
        ddc_ = true; status = L"显示器 DDC/CI 亮度";
        return S_OK;
    }

    HRESULT Read(int& brightness)
    {
        const auto selected = ResolveMonitor(device_);
        if (selected.monitor != monitor_ || selected.device != device_) return HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
        if (ddc_)
        {
            DWORD current = 0;
            if (!GetMonitorBrightness(physical_[0].hPhysicalMonitor, &minimum_, &current, &maximum_))
                return LastFailure();
            if (maximum_ <= minimum_ || current < minimum_ || current > maximum_) return E_UNEXPECTED;
            brightness = static_cast<int>((static_cast<unsigned long long>(current - minimum_) * 100 +
                (maximum_ - minimum_) / 2) / (maximum_ - minimum_));
            return S_OK;
        }
        if (!services_ || path_.empty()) return E_NOINTERFACE;
        ComPtr<IWbemClassObject> object;
        const Bstr path(path_.c_str());
        const HRESULT result = services_->GetObject(path.value, 0, nullptr, &object, nullptr);
        if (FAILED(result)) return result;
        brightness = IntegerProperty(object.Get(), L"CurrentBrightness");
        return brightness >= 0 && brightness <= 100 ? S_OK : E_UNEXPECTED;
    }

    HRESULT Adjust(int delta, int& brightness)
    {
        HRESULT result = Read(brightness);
        if (FAILED(result)) return result;
        int target = std::clamp(brightness + delta, 0, 100);
        if (ddc_)
        {
            const auto raw = minimum_ + static_cast<DWORD>((static_cast<unsigned long long>(maximum_ - minimum_) *
                static_cast<unsigned int>(target) + 50) / 100);
            if (!SetMonitorBrightness(physical_[0].hPhysicalMonitor, raw)) return LastFailure();
        }
        else
        {
            if (!levels_.empty())
                target = *std::min_element(levels_.begin(), levels_.end(), [target](int a, int b)
                    { return std::abs(a - target) < std::abs(b - target); });
            ComPtr<IWbemClassObject> inputs;
            result = methodInputs_->SpawnInstance(0, &inputs);
            if (FAILED(result)) return result;
            Variant timeout, level;
            timeout.value.vt = VT_I4; timeout.value.lVal = 0;
            level.value.vt = VT_UI1; level.value.bVal = static_cast<BYTE>(target);
            result = inputs->Put(L"Timeout", 0, &timeout.value, CIM_UINT32);
            if (SUCCEEDED(result)) result = inputs->Put(L"Brightness", 0, &level.value, CIM_UINT8);
            if (FAILED(result)) return result;
            const Bstr path(methodPath_.c_str()), method(L"WmiSetBrightness");
            ComPtr<IWbemCallResult> call;
            result = services_->ExecMethod(path.value, method.value, WBEM_FLAG_RETURN_IMMEDIATELY,
                nullptr, inputs.Get(), nullptr, &call);
            if (FAILED(result)) return result;
            LONG completion = E_PENDING;
            result = call->GetCallStatus(2000, &completion);
            if (result == WBEM_S_TIMEDOUT) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            if (FAILED(result) || FAILED(completion)) return FAILED(result) ? result : completion;
            ComPtr<IWbemClassObject> output;
            result = call->GetResultObject(0, &output);
            if (FAILED(result)) return result;
            if (!output || IntegerProperty(output.Get(), L"ReturnValue") != 0) return E_FAIL;
        }
        brightness = target;
        return S_OK;
    }

private:
    HRESULT BindWmi(const std::wstring& expected)
    {
        if (expected.empty()) return E_NOTIMPL;
        ComPtr<IWbemLocator> locator;
        HRESULT result = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator));
        if (FAILED(result)) return result;
        const Bstr nameSpace(L"ROOT\\WMI");
        result = locator->ConnectServer(nameSpace.value, nullptr, nullptr, nullptr,
            WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &services_);
        if (FAILED(result)) return result;
        result = CoSetProxyBlanket(services_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
            RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
        if (FAILED(result)) return result;
        const Bstr language(L"WQL"), query(L"SELECT * FROM WmiMonitorBrightness WHERE Active=TRUE");
        ComPtr<IEnumWbemClassObject> enumeration;
        result = services_->ExecQuery(language.value, query.value, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr, &enumeration);
        if (FAILED(result)) return result;
        for (int index = 0; index < 128; ++index)
        {
            ComPtr<IWbemClassObject> object; ULONG returned = 0;
            result = enumeration->Next(500, 1, &object, &returned);
            if (FAILED(result)) return result;
            if (!returned) break;
            const auto actual = Upper(StringProperty(object.Get(), L"InstanceName"));
            if (actual != expected && !(actual.size() > expected.size() && actual.compare(0, expected.size(), expected) == 0 &&
                actual[expected.size()] == L'_')) continue;
            path_ = StringProperty(object.Get(), L"__PATH");
            if (path_.empty()) return E_UNEXPECTED;
            // The two WMI classes share the exact InstanceName key.
            methodPath_ = path_;
            const auto className = Upper(methodPath_).find(L"WMIMONITORBRIGHTNESS.");
            if (className == std::wstring::npos) return E_UNEXPECTED;
            methodPath_.replace(className, std::wcslen(L"WmiMonitorBrightness"), L"WmiMonitorBrightnessMethods");
            ComPtr<IWbemClassObject> methodClass;
            const Bstr methodName(L"WmiMonitorBrightnessMethods");
            result = services_->GetObject(methodName.value, 0, nullptr, &methodClass, nullptr);
            if (SUCCEEDED(result)) result = methodClass->GetMethod(L"WmiSetBrightness", 0, &methodInputs_, nullptr);
            if (FAILED(result)) { path_.clear(); return result; }
            Variant levels;
            if (SUCCEEDED(object->Get(L"Level", 0, &levels.value, nullptr, nullptr)) &&
                levels.value.vt == (VT_ARRAY | VT_UI1) && levels.value.parray && SafeArrayGetDim(levels.value.parray) == 1)
            {
                LONG lower = 0, upper = -1;
                if (SUCCEEDED(SafeArrayGetLBound(levels.value.parray, 1, &lower)) &&
                    SUCCEEDED(SafeArrayGetUBound(levels.value.parray, 1, &upper)) && upper - lower < 256)
                    for (LONG element = lower; element <= upper; ++element)
                    {
                        BYTE level = 0;
                        if (SUCCEEDED(SafeArrayGetElement(levels.value.parray, &element, &level)) && level <= 100)
                            levels_.push_back(level);
                    }
            }
            return S_OK;
        }
        services_.Reset(); return E_NOTIMPL;
    }
    std::vector<PHYSICAL_MONITOR> physical_;
    ComPtr<IWbemServices> services_;
    ComPtr<IWbemClassObject> methodInputs_;
    std::wstring device_, path_, methodPath_;
    std::vector<int> levels_;
    HMONITOR monitor_ = nullptr;
    DWORD minimum_ = 0, maximum_ = 100;
    bool ddc_ = false;
};

struct CameraSignal
{
    std::atomic<bool> enabled{true}, known{false}, active{false}, valid{true};
    std::mutex mutex;
    std::map<std::wstring, bool> devices;
};

class CameraCallback final : public IMFSensorActivitiesReportCallback
{
public:
    CameraCallback(std::shared_ptr<Mailbox> mailbox, std::shared_ptr<CameraSignal> signal)
        : mailbox_(std::move(mailbox)), signal_(std::move(signal)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMFSensorActivitiesReportCallback)) return E_NOINTERFACE;
        *object = static_cast<IMFSensorActivitiesReportCallback*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_; if (!remaining) delete this; return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnActivitiesReport(IMFSensorActivitiesReport* report) override
    {
        if (!report) return E_POINTER;
        if (!signal_->enabled.load()) return S_OK;
        try
        {
            ULONG count = 0;
            HRESULT result = report->GetCount(&count);
            if (FAILED(result) || count > 1024) { signal_->valid = false; mailbox_->Wake(); return S_OK; }
            if (!count) return S_OK; // Reports can be partial or empty; do not clear another camera's state.
            std::vector<std::pair<std::wstring, bool>> changes;
            bool valid = true;
            for (ULONG index = 0; index < count; ++index)
            {
                ComPtr<IMFSensorActivityReport> sensor;
                if (FAILED(report->GetActivityReport(index, &sensor))) { valid = false; continue; }
                wchar_t link[2048]{}; ULONG written = 0, processes = 0;
                if (FAILED(sensor->GetSymbolicLink(link, 2048, &written)) ||
                    FAILED(sensor->GetProcessCount(&processes)) || processes > 4096) { valid = false; continue; }
                bool active = false;
                for (ULONG process = 0; process < processes; ++process)
                {
                    ComPtr<IMFSensorProcessActivity> activity; BOOL streaming = FALSE;
                    if (FAILED(sensor->GetProcessActivity(process, &activity)) ||
                        FAILED(activity->GetStreamingState(&streaming))) { valid = false; continue; }
                    active = active || streaming != FALSE;
                }
                changes.emplace_back(link, active);
            }
            {
                std::lock_guard lock(signal_->mutex);
                for (auto& change : changes) signal_->devices[std::move(change.first)] = change.second;
                signal_->active = std::any_of(signal_->devices.begin(), signal_->devices.end(),
                    [](const auto& entry) { return entry.second; });
            }
            signal_->valid = valid;
            signal_->known = !changes.empty(); mailbox_->Wake();
            return S_OK;
        }
        catch (...) { signal_->valid = false; mailbox_->Wake(); return S_OK; }
    }
private:
    std::atomic<ULONG> references_{1};
    std::shared_ptr<Mailbox> mailbox_;
    std::shared_ptr<CameraSignal> signal_;
};

class CameraMonitor
{
public:
    ~CameraMonitor() { Close(); }
    void Close() noexcept
    {
        if (signal) signal->enabled = false;
        if (monitor_) { monitor_->Stop(); ComPtr<IMFShutdown> shutdown; if (SUCCEEDED(monitor_.As(&shutdown))) shutdown->Shutdown(); }
        monitor_.Reset(); callback_.Reset(); signal.reset();
        if (startedMf_) MFShutdown(); startedMf_ = false;
        if (library_) FreeLibrary(library_); library_ = nullptr;
    }
    HRESULT Open(const std::shared_ptr<Mailbox>& mailbox)
    {
        Close();
        library_ = LoadLibraryExW(L"MFSensorGroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_) return LastFailure();
        using Create = HRESULT(WINAPI*)(IMFSensorActivitiesReportCallback*, IMFSensorActivityMonitor**);
        const auto create = reinterpret_cast<Create>(GetProcAddress(library_, "MFCreateSensorActivityMonitor"));
        if (!create) return E_NOTIMPL;
        HRESULT result = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(result)) return result;
        startedMf_ = true;
        signal = std::make_shared<CameraSignal>();
        callback_.Attach(new CameraCallback(mailbox, signal));
        result = create(callback_.Get(), &monitor_);
        if (SUCCEEDED(result)) result = monitor_->Start();
        if (FAILED(result)) Close();
        return result;
    }
    bool Opened() const noexcept { return monitor_ != nullptr; }
    std::shared_ptr<CameraSignal> signal;
private:
    HMODULE library_ = nullptr;
    bool startedMf_ = false;
    ComPtr<IMFSensorActivityMonitor> monitor_;
    ComPtr<CameraCallback> callback_;
};

struct MicrophoneSignal
{
    std::atomic<bool> enabled{true}, valid{true};
    std::mutex mutex;
    std::vector<ComPtr<IAudioSessionControl>> sessions;
};

class MicrophoneCallback final : public IAudioSessionNotification
{
public:
    MicrophoneCallback(std::shared_ptr<Mailbox> mailbox, std::shared_ptr<MicrophoneSignal> signal)
        : mailbox_(std::move(mailbox)), signal_(std::move(signal)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
    {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IAudioSessionNotification)) return E_NOINTERFACE;
        *object = static_cast<IAudioSessionNotification*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG remaining = --references_; if (!remaining) delete this; return remaining;
    }
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* session) override
    {
        if (!session) return E_POINTER;
        if (!signal_->enabled.load()) return S_OK;
        try
        {
            {
                std::lock_guard lock(signal_->mutex);
                if (!signal_->enabled.load()) return S_OK;
                if (signal_->sessions.size() >= 4096) signal_->valid = false;
                else signal_->sessions.emplace_back(session);
            }
            mailbox_->microphoneDirty = true; mailbox_->Wake();
            return S_OK;
        }
        catch (...) { signal_->valid = false; mailbox_->Wake(); return S_OK; }
    }
private:
    std::atomic<ULONG> references_{1};
    std::shared_ptr<Mailbox> mailbox_;
    std::shared_ptr<MicrophoneSignal> signal_;
};

struct MicrophoneEndpoint
{
    ~MicrophoneEndpoint()
    {
        if (signal) signal->enabled = false;
        if (registered) manager->UnregisterSessionNotification(callback.Get());
    }
    HRESULT Bind(IMMDevice* device, const std::shared_ptr<Mailbox>& mailbox)
    {
        HRESULT result = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
            reinterpret_cast<void**>(manager.GetAddressOf()));
        if (FAILED(result)) return result;
        signal = std::make_shared<MicrophoneSignal>();
        callback.Attach(new MicrophoneCallback(mailbox, signal));
        // Register before taking the initial list, then retain the controls from
        // both sources: the session enumerator alone can miss newly created sessions.
        result = manager->RegisterSessionNotification(callback.Get());
        if (FAILED(result)) return result;
        registered = true;
        ComPtr<IAudioSessionEnumerator> sessions;
        result = manager->GetSessionEnumerator(&sessions);
        int count = 0;
        if (SUCCEEDED(result)) result = sessions->GetCount(&count);
        if (FAILED(result)) return result;
        if (count < 0 || count > 4096) return E_UNEXPECTED;
        for (int index = 0; index < count; ++index)
        {
            ComPtr<IAudioSessionControl> control;
            result = sessions->GetSession(index, &control);
            if (FAILED(result)) return result;
            // Seed directly; synthesizing create notifications here can produce
            // a wake/rebind loop when a driver returns a broken session.
            std::lock_guard lock(signal->mutex);
            signal->sessions.push_back(std::move(control));
        }
        return S_OK;
    }
    bool Read(bool& active)
    {
        std::vector<ComPtr<IAudioSessionControl>> controls;
        { std::lock_guard lock(signal->mutex); controls = signal->sessions; }
        bool complete = signal->valid.load();
        std::set<IAudioSessionControl*> expired;
        for (const auto& control : controls)
        {
            AudioSessionState state = AudioSessionStateInactive;
            const HRESULT result = control->GetState(&state);
            if (FAILED(result)) complete = false;
            else if (state == AudioSessionStateActive) active = true;
            else if (state == AudioSessionStateExpired) expired.insert(control.Get());
        }
        if (!expired.empty())
        {
            std::lock_guard lock(signal->mutex);
            std::erase_if(signal->sessions, [&](const auto& control) { return expired.contains(control.Get()); });
        }
        return complete;
    }
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<MicrophoneCallback> callback;
    std::shared_ptr<MicrophoneSignal> signal;
    bool registered = false;
};

// Only metadata is requested: no IAudioClient, capture buffer or camera source.
class MicrophoneMonitor
{
public:
    void Close() noexcept { endpoints_.clear(); }
    void Read(const std::shared_ptr<Mailbox>& mailbox, bool& available, bool& active)
    {
        available = active = false;
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) { Close(); return; }
        ComPtr<IMMDeviceCollection> devices;
        result = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices);
        UINT count = 0;
        if (FAILED(result) || FAILED(devices->GetCount(&count))) { Close(); return; }
        bool complete = true;
        std::set<std::wstring> present;
        for (UINT index = 0; index < count; ++index)
        {
            ComPtr<IMMDevice> device;
            LPWSTR rawId = nullptr;
            result = devices->Item(index, &device);
            if (SUCCEEDED(result)) result = device->GetId(&rawId);
            const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> release(rawId, CoTaskMemFree);
            if (FAILED(result) || !rawId) { complete = false; continue; }
            const std::wstring id(rawId); present.insert(id);
            auto& endpoint = endpoints_[id];
            if (!endpoint)
            {
                auto created = std::make_unique<MicrophoneEndpoint>();
                if (FAILED(created->Bind(device.Get(), mailbox))) { complete = false; continue; }
                endpoint = std::move(created);
            }
            if (!endpoint->Read(active)) { complete = false; endpoint.reset(); }
        }
        std::erase_if(endpoints_, [&](const auto& entry) { return !present.contains(entry.first); });
        available = count > 0 && (complete || active);
    }
private:
    std::map<std::wstring, std::unique_ptr<MicrophoneEndpoint>> endpoints_;
};
}

struct BehaviorService::Impl
{
    explicit Impl(Logger& target) : logger(target), mailbox(std::make_shared<Mailbox>()) {}
    Logger& logger;
    std::shared_ptr<Mailbox> mailbox;
    std::thread worker;

    void Run(std::function<void(BehaviorSnapshot)> sink) noexcept
    {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        struct Apartment { HRESULT result; ~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); } } apartment{initialized};
        BehaviorSnapshot previous;
        bool published = false;
        try
        {
            Brightness brightness;
            CameraMonitor camera;
            MicrophoneMonitor microphone;
            BehaviorSnapshot snapshot;
            auto generation = ~0ull;
            auto nextBrightness = Clock::time_point::min(), nextPrivacy = Clock::time_point::min();
            auto retryCamera = Clock::time_point::min();
            HRESULT cameraResult = E_PENDING;
            for (;;)
            {
                std::wstring device; bool brightnessEnabled = false, privacyEnabled = false;
                unsigned long long current = 0, version = 0; int delta = 0;
                {
                    std::lock_guard lock(mailbox->mutex);
                    if (mailbox->stopping) break;
                    device = mailbox->device; brightnessEnabled = mailbox->brightness; privacyEnabled = mailbox->privacy;
                    current = mailbox->configuration; delta = mailbox->adjustment; mailbox->adjustment = 0;
                    version = mailbox->version.load(std::memory_order_acquire);
                }
                const auto now = Clock::now();
                if (current != generation)
                {
                    brightness.Close(); camera.Close(); microphone.Close(); snapshot = {}; generation = current;
                    nextBrightness = nextPrivacy = retryCamera = Clock::time_point::min();
                }
                if (!brightnessEnabled) snapshot.brightnessStatus = L"亮度快捷调节已关闭";
                else if (FAILED(initialized)) snapshot.brightnessStatus = Error(L"亮度服务不可用", initialized);
                else if (now >= nextBrightness || delta)
                {
                    HRESULT result = S_OK;
                    if (!brightness.Bound()) result = brightness.Bind(device, snapshot.brightnessStatus);
                    if (SUCCEEDED(result))
                    {
                        // A queued wheel delta belongs to the configuration that accepted it.
                        std::lock_guard lock(mailbox->mutex);
                        if (mailbox->configuration != generation || mailbox->stopping) continue;
                    }
                    if (SUCCEEDED(result)) result = delta ? brightness.Adjust(delta, snapshot.brightness) : brightness.Read(snapshot.brightness);
                    snapshot.brightnessAvailable = SUCCEEDED(result);
                    if (FAILED(result))
                    {
                        if (brightness.Bound()) snapshot.brightnessStatus = Error(L"亮度读取或调节失败", result);
                        brightness.Close();
                    }
                    nextBrightness = now + (snapshot.brightnessAvailable ? 5s : 30s);
                }
                if (!privacyEnabled) snapshot.privacyStatus = L"设备占用提示已关闭";
                else if (FAILED(initialized)) snapshot.privacyStatus = Error(L"设备状态服务不可用", initialized);
                else
                {
                    if (!camera.Opened() && now >= retryCamera)
                    {
                        cameraResult = camera.Open(mailbox);
                        retryCamera = now + 30s;
                    }
                    if (now >= nextPrivacy || mailbox->microphoneDirty.exchange(false))
                    {
                        microphone.Read(mailbox, snapshot.microphoneAvailable, snapshot.microphoneActive);
                        nextPrivacy = now + 2s;
                    }
                    snapshot.cameraAvailable = camera.Opened() && camera.signal && camera.signal->known && camera.signal->valid;
                    snapshot.cameraActive = snapshot.cameraAvailable && camera.signal->active;
                    snapshot.privacyStatus = snapshot.cameraAvailable ? L"摄像头：系统流状态" :
                        (camera.Opened() ? L"摄像头：等待系统活动报告" : Error(L"摄像头状态不可用", cameraResult));
                    snapshot.privacyStatus += snapshot.microphoneAvailable ? L"；麦克风：系统会话状态" : L"；麦克风状态不可用";
                }
                bool currentConfiguration = false;
                {
                    std::lock_guard lock(mailbox->mutex);
                    currentConfiguration = mailbox->configuration == generation && !mailbox->stopping;
                }
                if (currentConfiguration && (!published || !Equal(previous, snapshot)))
                {
                    previous = snapshot; published = true;
                    try { if (sink) sink(snapshot); } catch (...) { logger.Write("Behavior snapshot sink failed"); }
                }
                auto deadline = Clock::time_point::max();
                if (brightnessEnabled && SUCCEEDED(initialized)) deadline = std::min(deadline, nextBrightness);
                if (privacyEnabled && SUCCEEDED(initialized)) deadline = std::min(deadline, nextPrivacy);
                std::unique_lock lock(mailbox->mutex);
                mailbox->changed.wait_until(lock, deadline, [&]
                    { return mailbox->stopping || mailbox->version.load(std::memory_order_acquire) != version; });
            }
        }
        catch (...)
        {
            logger.Write("Behavior worker stopped after an unexpected failure");
            try
            {
                BehaviorSnapshot failed;
                failed.brightnessStatus = failed.privacyStatus = L"状态服务暂时不可用";
                if (sink) sink(std::move(failed));
            }
            catch (...) { logger.Write("Behavior failure snapshot could not be delivered"); }
        }
    }
};

BehaviorService::BehaviorService(Logger& logger) : impl_(std::make_unique<Impl>(logger)) {}
BehaviorService::~BehaviorService() { Stop(); }

void BehaviorService::Start(std::function<void(BehaviorSnapshot)> sink)
{
    Stop();
    { std::lock_guard lock(impl_->mailbox->mutex); impl_->mailbox->stopping = false; }
    try { impl_->worker = std::thread([state = impl_.get(), sink = std::move(sink)]() mutable { state->Run(std::move(sink)); }); }
    catch (...) { std::lock_guard lock(impl_->mailbox->mutex); impl_->mailbox->stopping = true; impl_->logger.Write("Behavior worker could not start"); }
}

void BehaviorService::Configure(std::wstring monitorDevice, bool brightnessEnabled, bool privacyEnabled)
{
    {
        std::lock_guard lock(impl_->mailbox->mutex);
        if (impl_->mailbox->device == monitorDevice && impl_->mailbox->brightness == brightnessEnabled &&
            impl_->mailbox->privacy == privacyEnabled) return;
        impl_->mailbox->device = std::move(monitorDevice);
        impl_->mailbox->brightness = brightnessEnabled; impl_->mailbox->privacy = privacyEnabled;
        impl_->mailbox->adjustment = 0; ++impl_->mailbox->configuration;
    }
    impl_->mailbox->Wake();
}

void BehaviorService::AdjustBrightness(int delta)
{
    {
        std::lock_guard lock(impl_->mailbox->mutex);
        if (impl_->mailbox->stopping || !impl_->mailbox->brightness || !delta) return;
        impl_->mailbox->adjustment = std::clamp(impl_->mailbox->adjustment + std::clamp(delta, -100, 100), -100, 100);
    }
    impl_->mailbox->Wake();
}

void BehaviorService::Stop() noexcept
{
    {
        std::lock_guard lock(impl_->mailbox->mutex);
        impl_->mailbox->stopping = true; impl_->mailbox->adjustment = 0;
    }
    impl_->mailbox->Wake();
    if (impl_->worker.joinable()) impl_->worker.join();
}
}
