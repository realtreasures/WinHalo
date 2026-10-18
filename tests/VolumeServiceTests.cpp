#include "../Services/VolumeService.h"
#include "../Utils/Logger.h"
#include "../Core/Platform.h"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace winisland;

namespace
{
void Expect(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

struct Receiver
{
    std::mutex mutex;
    std::condition_variable changed;
    VolumeSnapshot snapshot;
    unsigned int received = 0;
    bool firstNotify = false;
    std::thread::id thread;
    void Receive(VolumeSnapshot value)
    {
        std::lock_guard lock(mutex);
        if (!received) firstNotify = value.notify;
        snapshot = std::move(value);
        thread = std::this_thread::get_id();
        ++received;
        changed.notify_all();
    }
    void Wait()
    {
        std::unique_lock lock(mutex);
        Expect(changed.wait_for(lock, std::chrono::seconds(6), [this] { return received != 0; }),
            "Initial audio snapshot timed out");
    }
};

void ReadAndStop(VolumeService& service, bool print)
{
    Receiver receiver;
    service.Start([&receiver](VolumeSnapshot value) { receiver.Receive(std::move(value)); });
    receiver.Wait();
    service.Stop();
    unsigned int stoppedCount;
    {
        std::lock_guard lock(receiver.mutex);
        Expect(!receiver.firstNotify, "Initial snapshot requested a notification");
        Expect(receiver.thread != std::this_thread::get_id(), "Audio snapshot ran on the caller thread");
        Expect(receiver.snapshot.available || !receiver.snapshot.error.empty(), "Unavailable audio lacked an explanation");
        Expect(receiver.snapshot.level >= 0 && receiver.snapshot.level <= 1, "Invalid audio level");
        if (receiver.snapshot.available) Expect(!receiver.snapshot.deviceName.empty(), "Output device name was empty");
        stoppedCount = receiver.received;
        if (print) std::cout << "Read-only default output: available=" << receiver.snapshot.available
            << ", level=" << receiver.snapshot.level << ", muted=" << receiver.snapshot.muted << '\n';
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
    {
        std::lock_guard lock(receiver.mutex);
        Expect(receiver.received == stoppedCount, "A snapshot arrived after Stop returned");
    }
}
}

int main()
{
    try
    {
        const auto data = std::filesystem::current_path() / L"artifacts" / L"volume-test-data";
        std::filesystem::create_directories(data);
        Expect(SetEnvironmentVariableW(L"WINHALO_DATA_DIR", data.c_str()) != FALSE, "Test data directory failed");
        Logger logger;
        VolumeService service(logger);
        ReadAndStop(service, true);
        DWORD before = 0;
        Expect(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "Initial handle count failed");
        for (int cycle = 0; cycle < 20; ++cycle) ReadAndStop(service, false);
        for (int cycle = 0; cycle < 20; ++cycle)
        {
            service.Start([](VolumeSnapshot) {});
            service.Stop();
        }
        service.Stop();
        DWORD after = 0;
        Expect(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "Final handle count failed");
        Expect(after <= before + 4, "Start/Stop leaked handles");
        std::cout << "Volume lifecycle passed: handles " << before << " -> " << after << '\n';
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
