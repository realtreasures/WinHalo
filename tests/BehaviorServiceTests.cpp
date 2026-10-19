#include "../Core/Platform.h"
#include "../Services/BehaviorService.h"
#include "../Utils/Logger.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace winisland;
using namespace std::chrono_literals;

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

std::string Utf8(const std::wstring& text)
{
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.c_str(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return "<invalid status text>";
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.c_str(), static_cast<int>(text.size()),
        result.data(), count, nullptr, nullptr);
    return result;
}

bool Disabled(const BehaviorSnapshot& snapshot)
{
    return !snapshot.brightnessAvailable && !snapshot.cameraAvailable && !snapshot.microphoneAvailable &&
        !snapshot.cameraActive && !snapshot.microphoneActive &&
        snapshot.brightnessStatus.find(L"关闭") != std::wstring::npos &&
        snapshot.privacyStatus.find(L"关闭") != std::wstring::npos;
}

void Validate(const BehaviorSnapshot& snapshot)
{
    Expect(!snapshot.brightnessAvailable || (snapshot.brightness >= 0 && snapshot.brightness <= 100),
        "Available brightness was outside its percentage range");
    Expect(!snapshot.cameraActive || snapshot.cameraAvailable, "Unavailable camera was reported active");
    Expect(!snapshot.microphoneActive || snapshot.microphoneAvailable, "Unavailable microphone was reported active");
    Expect(!snapshot.brightnessStatus.empty() && !snapshot.privacyStatus.empty(),
        "Behavior service omitted its capability/status explanation");
}

struct Probe
{
    void Receive(BehaviorSnapshot snapshot)
    {
        {
            std::lock_guard lock(mutex);
            if (stopped) ++lateCallbacks;
            latest = std::move(snapshot);
            ++received;
        }
        changed.notify_all();
    }

    unsigned int Count()
    {
        std::lock_guard lock(mutex);
        return received;
    }

    BehaviorSnapshot Wait(unsigned int after, bool disabled)
    {
        std::unique_lock lock(mutex);
        const bool ready = changed.wait_for(lock, 10s, [&]
            { return received > after && Disabled(latest) == disabled; });
        Expect(ready, disabled ? "No disabled snapshot arrived within ten seconds" :
            "No read-only capability snapshot arrived within ten seconds");
        auto result = latest;
        lock.unlock();
        Validate(result);
        return result;
    }

    BehaviorSnapshot Observe()
    {
        // Allow initial asynchronous camera reports and one two-second microphone
        // poll to settle. No input, audio stream or camera source is created.
        std::unique_lock lock(mutex);
        const auto deadline = std::chrono::steady_clock::now() + 2200ms;
        changed.wait_until(lock, deadline, [&] { return stopped; });
        auto result = latest;
        lock.unlock();
        Validate(result);
        return result;
    }

    void ExpectQuiet(bool afterStop)
    {
        std::unique_lock lock(mutex);
        if (afterStop) stopped = true;
        const auto baseline = received;
        const bool extra = changed.wait_for(lock, 350ms, [&] { return received != baseline || lateCallbacks != 0; });
        Expect(!extra && lateCallbacks == 0, afterStop ? "Behavior callback arrived after Stop returned" :
            "Repeated disabled configuration produced duplicate snapshots");
    }

    std::mutex mutex;
    std::condition_variable changed;
    BehaviorSnapshot latest;
    unsigned int received = 0, lateCallbacks = 0;
    bool stopped = false;
};

DWORD Handles()
{
    DWORD count = 0;
    Expect(GetProcessHandleCount(GetCurrentProcess(), &count) != FALSE, "Could not inspect process handles");
    return count;
}

struct IsolatedData
{
    IsolatedData()
    {
        const DWORD required = GetEnvironmentVariableW(L"WINHALO_DATA_DIR", nullptr, 0);
        if (required)
        {
            std::vector<wchar_t> value(required);
            const DWORD copied = GetEnvironmentVariableW(L"WINHALO_DATA_DIR", value.data(), required);
            Expect(copied > 0 && copied < required, "Could not preserve existing data-directory environment");
            previous = value.data(); hadPrevious = true;
        }
        const auto directory = std::filesystem::current_path() / L"artifacts" / L"behavior-test-data";
        std::filesystem::create_directories(directory);
        Expect(SetEnvironmentVariableW(L"WINHALO_DATA_DIR", directory.c_str()) != FALSE,
            "Could not isolate behavior-service test logs");
    }
    ~IsolatedData() { SetEnvironmentVariableW(L"WINHALO_DATA_DIR", hadPrevious ? previous.c_str() : nullptr); }
    std::wstring previous;
    bool hadPrevious = false;
};
}

int main()
{
    try
    {
        IsolatedData isolated;
        Logger logger;
        const DWORD initialHandles = Handles();
        DWORD warmedHandles = initialHandles, finalHandles = initialHandles;
        BehaviorService service(logger);
        std::vector<std::shared_ptr<Probe>> probes;
        for (unsigned int cycle = 0; cycle < 2; ++cycle)
        {
            auto probe = std::make_shared<Probe>(); probes.push_back(probe);
            service.Configure(L"", false, false);
            service.Start([probe](BehaviorSnapshot value) { probe->Receive(std::move(value)); });
            probe->Wait(0, true);

            auto before = probe->Count();
            service.Configure(L"", true, true);
            probe->Wait(before, false);
            const auto capability = probe->Observe();
            std::cout << "Cycle " << cycle + 1 << ": brightness_available=" << capability.brightnessAvailable
                << ", brightness=" << capability.brightness << ", camera_available=" << capability.cameraAvailable
                << ", camera_active=" << capability.cameraActive << ", microphone_available="
                << capability.microphoneAvailable << ", microphone_active=" << capability.microphoneActive << '\n'
                << "  brightness_status=" << Utf8(capability.brightnessStatus) << '\n'
                << "  privacy_status=" << Utf8(capability.privacyStatus) << '\n';

            before = probe->Count();
            service.Configure(L"", false, false);
            const auto disabled = probe->Wait(before, true);
            Expect(!disabled.brightnessAvailable && !disabled.cameraAvailable && !disabled.microphoneAvailable &&
                !disabled.cameraActive && !disabled.microphoneActive, "Disabling behavior retained an available/active flag");
            service.Configure(L"", false, false);
            probe->ExpectQuiet(false);
            service.Stop();
            service.Stop(); // Idempotence is part of repeated service teardown.
            probe->ExpectQuiet(true);
            finalHandles = Handles();
            if (cycle == 0) warmedHandles = finalHandles;
        }
        // Keep the first probe alive across the second start: an old asynchronous
        // callback must not escape after a later lifecycle has begun.
        for (const auto& probe : probes) probe->ExpectQuiet(true);
        Expect(finalHandles <= warmedHandles + 16, "Repeated behavior lifecycle retained excessive process handles");
        std::cout << "PASS: two read-only Start/Configure/Stop cycles; disabled snapshots; capability reporting; "
            "disable clears active/available flags; duplicate suppression; no callbacks after Stop; "
            "handles initial=" << initialHandles << ", warmed=" << warmedHandles << ", final=" << finalHandles
            << "; brightness adjustments=0, capture streams=0\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
