#include "../Core/Platform.h"
#include "../Services/MediaService.h"
#include "../Services/VolumeService.h"
#include "../Utils/Logger.h"
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace winisland;
using namespace std::chrono_literals;

namespace
{
void Expect(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Snapshot>
struct Probe
{
    void Receive(Snapshot value)
    {
        {
            std::lock_guard lock(mutex);
            if (received == 0) latest = std::move(value);
            ++received;
        }
        ready.notify_all();
    }

    Snapshot First()
    {
        std::unique_lock lock(mutex);
        Expect(ready.wait_for(lock, 8s, [this] { return received != 0; }), "Service did not publish an initial snapshot");
        return latest;
    }

    unsigned int Count()
    {
        std::lock_guard lock(mutex);
        return received;
    }

    std::mutex mutex;
    std::condition_variable ready;
    Snapshot latest;
    unsigned int received = 0;
};

void Validate(const MediaSnapshot& media)
{
    Expect(std::isfinite(media.positionSeconds) && media.positionSeconds >= 0,
        "Invalid media position");
    Expect(std::isfinite(media.durationSeconds) && media.durationSeconds >= 0,
        "Invalid media duration");
    Expect(std::isfinite(media.playbackRate) && media.playbackRate > 0, "Invalid media playback rate");
    Expect(media.artworkWidth <= 256 && media.artworkHeight <= 256, "Artwork dimensions exceed limits");
    Expect(media.artworkBgra.size() == static_cast<std::size_t>(media.artworkWidth) * media.artworkHeight * 4,
        "Artwork byte count differs from BGRA dimensions");
    if (!media.available)
    {
        Expect(!media.playing && !media.canToggle && !media.canPrevious && !media.canNext,
            "Unavailable media exposed enabled playback controls");
        Expect(media.artworkBgra.empty(), "Unavailable media retained album artwork");
    }
}

void Validate(const VolumeSnapshot& volume)
{
    Expect(std::isfinite(volume.level) && volume.level >= 0 && volume.level <= 1,
        "Invalid master volume level");
    Expect(!volume.notify, "Initial volume snapshot unexpectedly requested a popup");
    if (!volume.available) Expect(!volume.error.empty(), "Unavailable audio endpoint did not report an error");
}
}

int main()
{
    try
    {
        const auto data = std::filesystem::current_path() / L"artifacts" / L"services-test-data";
        std::filesystem::create_directories(data);
        Expect(SetEnvironmentVariableW(L"WINHALO_DATA_DIR", data.c_str()) != FALSE, "Test data directory failed");
        Logger logger;
        MediaService media(logger);
        VolumeService volume(logger);
        media.Stop();
        volume.Stop();
        bool observedNoMedia = false;
        for (int cycle = 0; cycle < 6; ++cycle)
        {
            auto mediaProbe = std::make_shared<Probe<MediaSnapshot>>();
            auto volumeProbe = std::make_shared<Probe<VolumeSnapshot>>();
            media.Start([mediaProbe](MediaSnapshot value) { mediaProbe->Receive(std::move(value)); });
            volume.Start([volumeProbe](VolumeSnapshot value) { volumeProbe->Receive(std::move(value)); });
            const auto mediaSnapshot = mediaProbe->First();
            const auto volumeSnapshot = volumeProbe->First();
            Validate(mediaSnapshot);
            Validate(volumeSnapshot);
            observedNoMedia |= !mediaSnapshot.available && mediaSnapshot.error.empty();
            const auto stopStarted = std::chrono::steady_clock::now();
            media.Stop();
            volume.Stop();
            Expect(std::chrono::steady_clock::now() - stopStarted < 3s, "Service shutdown exceeded 3 seconds");
            media.Stop();
            volume.Stop();
            const auto mediaCount = mediaProbe->Count();
            const auto volumeCount = volumeProbe->Count();
            std::this_thread::sleep_for(50ms);
            Expect(mediaProbe->Count() == mediaCount && volumeProbe->Count() == volumeCount,
                "Service invoked its receiver after Stop returned");
        }
        for (int cycle = 0; cycle < 16; ++cycle)
        {
            media.Start([](MediaSnapshot) {});
            media.Stop();
        }
        std::cout << "PASS: concurrent media/audio initial snapshots; 6 restart cycles; bounded idempotent shutdown; "
            "no callbacks after Stop; 16 immediate media cancellations; no playback or volume commands sent.\n";
        std::cout << (observedNoMedia ? "No-player degradation observed and validated.\n" :
            "No-player path was not observed (active session or system service error).\n");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
