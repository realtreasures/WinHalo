#include "../Core/Platform.h"
#include "../Services/MediaService.h"
#include "MediaInteropTests.h"
#include "../Utils/Logger.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Storage.Streams.h>
#include <shobjidl_core.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <winrt/Windows.Data.Json.h>
#include <sstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using namespace winrt::Windows::Media;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;
using namespace winisland;

namespace
{
struct Apartment
{
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
};
void Expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Operation>
auto Await(const Operation& operation) -> decltype(operation.GetResults())
{
    if (operation.wait_for(3s) == winrt::Windows::Foundation::AsyncStatus::Started)
    {
        operation.Cancel();
        throw std::runtime_error("Fixture operation timed out");
    }
    return operation.GetResults();
}

void Put32(std::vector<unsigned char>& bytes, std::size_t at, std::uint32_t value)
{
    for (unsigned int i = 0; i < 4; ++i) bytes[at + i] = static_cast<unsigned char>(value >> (8 * i));
}

InMemoryRandomAccessStream Stream(const std::vector<unsigned char>& bytes)
{
    InMemoryRandomAccessStream stream;
    DataWriter writer(stream);
    writer.WriteBytes(bytes);
    Await(writer.StoreAsync());
    writer.DetachStream();
    stream.Seek(0);
    return stream;
}

std::vector<unsigned char> SilentWave()
{
    constexpr std::uint32_t length = 8000 * 60;
    std::vector<unsigned char> bytes(44 + length, 128);
    const unsigned char header[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,1,0,8,0,'d','a','t','a',0,0,0,0 };
    std::copy(std::begin(header), std::end(header), bytes.begin());
    Put32(bytes, 4, 36 + length);
    Put32(bytes, 24, 8000);
    Put32(bytes, 28, 8000);
    Put32(bytes, 40, length);
    return bytes;
}

std::vector<unsigned char> ArtworkBitmap(bool alternate = false)
{
    std::vector<unsigned char> bytes(54 + 16 * 16 * 4, 0);
    bytes[0] = 'B'; bytes[1] = 'M';
    Put32(bytes, 2, static_cast<std::uint32_t>(bytes.size()));
    Put32(bytes, 10, 54); Put32(bytes, 14, 40);
    Put32(bytes, 18, 16); Put32(bytes, 22, 16);
    bytes[26] = 1; bytes[28] = 32;
    Put32(bytes, 34, 16 * 16 * 4);
    for (std::size_t i = 54; i < bytes.size(); i += 4)
    {
        bytes[i] = alternate ? 90 : 220; bytes[i + 1] = alternate ? 190 : 80;
        bytes[i + 2] = alternate ? 55 : 140; bytes[i + 3] = 255;
    }
    return bytes;
}

struct Receiver
{
    std::mutex mutex;
    std::condition_variable changed;
    MediaSnapshot snapshot;
    void Receive(MediaSnapshot value)
    {
        { std::lock_guard lock(mutex); snapshot = std::move(value); }
        changed.notify_all();
    }
};

// A hidden owned window lets the UI exercise real app restoration without touching user apps.
struct FixtureWindow
{
    HWND window = nullptr;
    std::jthread thread;
    std::mutex mutex;
    std::condition_variable ready;
    bool created = false;
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM w, LPARAM l)
    {
        if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
        return DefWindowProcW(window, message, w, l);
    }
    explicit FixtureWindow(const std::wstring& source)
    {
        thread = std::jthread([this, source]
        {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            WNDCLASSW cls{}; cls.lpfnWndProc = Proc; cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpszClassName = L"WinHalo.OwnedMediaFixture";
            cls.hIcon = LoadIconW(nullptr, IDI_APPLICATION); cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            RegisterClassW(&cls);
            HWND target = CreateWindowExW(0, cls.lpszClassName, source.c_str(), WS_OVERLAPPEDWINDOW,
                40, 500, 320, 180, nullptr, nullptr, cls.hInstance, nullptr);
            if (target)
            {
                winrt::com_ptr<IPropertyStore> store;
                if (SUCCEEDED(SHGetPropertyStoreForWindow(target, IID_PPV_ARGS(store.put()))))
                {
                    PROPVARIANT value{}; InitPropVariantFromString(source.c_str(), &value);
                    store->SetValue(PKEY_AppUserModel_ID, value); store->Commit(); PropVariantClear(&value);
                }
            }
            { std::lock_guard lock(mutex); window = target; created = true; }
            ready.notify_one();
            if (target)
            {
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            UnregisterClassW(cls.lpszClassName, cls.hInstance);
            CoUninitialize();
        });
        std::unique_lock lock(mutex); ready.wait(lock, [this] { return created; });
        Expect(window != nullptr, "Owned fixture window creation failed");
    }
    ~FixtureWindow() { if (window) PostMessageW(window, WM_CLOSE, 0, 0); }
};

int ObserveService()
{
    using namespace winrt::Windows::Data::Json;
    Logger logger; auto receiver = std::make_shared<Receiver>(); MediaService service(logger);
    service.Start([receiver](MediaSnapshot value) { receiver->Receive(std::move(value)); });
    std::cout << "READY OBSERVER" << std::endl;
    std::string command;
    while (std::getline(std::cin, command) && command != "quit")
    {
        if (command == "state")
        {
            MediaSnapshot snapshot; { std::lock_guard lock(receiver->mutex); snapshot = receiver->snapshot; }
            JsonObject result;
            result.SetNamedValue(L"source", JsonValue::CreateStringValue(snapshot.source));
            result.SetNamedValue(L"id", JsonValue::CreateNumberValue(static_cast<double>(snapshot.sessionId)));
            result.SetNamedValue(L"playing", JsonValue::CreateBooleanValue(snapshot.playing));
            result.SetNamedValue(L"error", JsonValue::CreateStringValue(snapshot.error));
            result.SetNamedValue(L"iconBytes", JsonValue::CreateNumberValue(snapshot.app ? static_cast<double>(snapshot.app->iconBgra.size()) : 0));
            JsonArray choices;
            for (const auto& choice : snapshot.sessions)
            {
                JsonObject entry; entry.SetNamedValue(L"source", JsonValue::CreateStringValue(choice.source));
                entry.SetNamedValue(L"id", JsonValue::CreateNumberValue(static_cast<double>(choice.id)));
                entry.SetNamedValue(L"playing", JsonValue::CreateBooleanValue(choice.playing)); choices.Append(entry);
            }
            result.SetNamedValue(L"sessions", choices);
            std::cout << winrt::to_string(result.Stringify()) << std::endl;
        }
        else
        {
            std::istringstream input(command); std::string action, source; std::uint64_t id = 0;
            input >> action >> id >> source;
            if (action == "next") service.NextSession(id);
            else if (source.starts_with("WinHalo.MediaFixture."))
            {
                if (action == "toggle") service.Command(MediaCommand::TogglePlayPause, winrt::to_hstring(source).c_str(), id);
                else if (action == "previous") service.Command(MediaCommand::Previous, winrt::to_hstring(source).c_str(), id);
                else if (action == "next-track") service.Command(MediaCommand::Next, winrt::to_hstring(source).c_str(), id);
            }
            else { std::cout << "REJECT " << command << std::endl; continue; }
            std::cout << "ACK " << command << std::endl;
        }
    }
    service.Stop(); return 0;
}
}

int main(int argc, char** argv)
{
    try
    {
        Apartment apartment;
        if (argc > 1 && std::string(argv[1]) == "--observe") return ObserveService();
        const bool serve = argc > 1 && std::string(argv[1]).starts_with("--serve");
        const bool alternate = argc > 1 && std::string(argv[1]) == "--serve-alt";
        if (argc > 1 && std::string(argv[1]) == "--inspect")
        {
            const auto manager = Await(GlobalSystemMediaTransportControlsSessionManager::RequestAsync());
            const auto current = manager.GetCurrentSession();
            std::cout << "Current: " << (current ? winrt::to_string(current.SourceAppUserModelId()) : "none") << '\n';
            for (const auto& session : manager.GetSessions())
            {
                const auto playback = session.GetPlaybackInfo();
                std::cout << "Session: " << winrt::to_string(session.SourceAppUserModelId()) << " status="
                    << (playback ? static_cast<int>(playback.PlaybackStatus()) : -1) << '\n';
            }
            return 0;
        }
        const auto identity = L"WinHalo.MediaFixture." + std::to_wstring(GetCurrentProcessId());
        media_interop_tests::Run();
        if (!serve)
            std::cout << "PASS: disconnected synchronous read and closed async result produce HRESULT without first-chance C++ exceptions.\n";
        winrt::check_hresult(SetCurrentProcessExplicitAppUserModelID(identity.c_str()));
        std::unique_ptr<FixtureWindow> fixtureWindow;
        if (serve) fixtureWindow = std::make_unique<FixtureWindow>(identity);
        const auto data = std::filesystem::current_path() / L"artifacts" / L"media-fixture-data";
        std::filesystem::create_directories(data);
        Expect(SetEnvironmentVariableW(L"WINHALO_DATA_DIR", data.c_str()) != FALSE, "Fixture data directory failed");
        Logger logger;
        winrt::Windows::Media::Playback::MediaPlayer player;
        player.Volume(0);
        player.IsMuted(true);
        player.CommandManager().IsEnabled(false);
        auto audio = Stream(SilentWave());
        player.Source(winrt::Windows::Media::Core::MediaSource::CreateFromStream(audio, L"audio/wav"));
        auto controls = player.SystemMediaTransportControls();
        controls.IsEnabled(true);
        controls.IsPlayEnabled(true); controls.IsPauseEnabled(true);
        controls.IsPreviousEnabled(true); controls.IsNextEnabled(true);
        auto updater = controls.DisplayUpdater();
        updater.Type(MediaPlaybackType::Music);
        const auto title = L"WinHalo fixture " + std::to_wstring(GetCurrentProcessId());
        updater.MusicProperties().Title(title);
        updater.MusicProperties().Artist(L"WinHalo test artist");
        updater.MusicProperties().AlbumTitle(L"WinHalo test album");
        auto artwork = Stream(ArtworkBitmap(alternate));
        updater.Thumbnail(RandomAccessStreamReference::CreateFromStream(artwork));
        updater.Update();
        auto pressed = std::make_shared<std::atomic<unsigned int>>(0);
        auto buttons = controls.ButtonPressed(winrt::auto_revoke,
            [player, pressed](const SystemMediaTransportControls& sender, const SystemMediaTransportControlsButtonPressedEventArgs& args)
            {
                if (args.Button() == SystemMediaTransportControlsButton::Play)
                { player.Play(); sender.PlaybackStatus(MediaPlaybackStatus::Playing); }
                else if (args.Button() == SystemMediaTransportControlsButton::Pause)
                { player.Pause(); sender.PlaybackStatus(MediaPlaybackStatus::Paused); }
                else if (args.Button() == SystemMediaTransportControlsButton::Next || args.Button() == SystemMediaTransportControlsButton::Previous)
                    pressed->fetch_add(1);
            });
        player.Play();
        const auto openedBy = std::chrono::steady_clock::now() + 2s;
        while (player.PlaybackSession().PlaybackState() != winrt::Windows::Media::Playback::MediaPlaybackState::Playing &&
            std::chrono::steady_clock::now() < openedBy)
            std::this_thread::sleep_for(20ms);
        controls.IsEnabled(true);
        controls.IsPlayEnabled(true); controls.IsPauseEnabled(true);
        controls.IsPreviousEnabled(true); controls.IsNextEnabled(true);
        controls.PlaybackStatus(MediaPlaybackStatus::Playing);
        auto manager = Await(GlobalSystemMediaTransportControlsSessionManager::RequestAsync());
        GlobalSystemMediaTransportControlsSession own{ nullptr };
        const auto registeredBy = std::chrono::steady_clock::now() + 4s;
        while (!own && std::chrono::steady_clock::now() < registeredBy)
        {
            for (const auto& session : manager.GetSessions())
                if (session.SourceAppUserModelId() == identity) { own = session; break; }
            if (!own) std::this_thread::sleep_for(100ms);
        }
        if (!own)
        {
            player.Close();
            std::cout << "SKIP: own silent MediaPlayer did not register an identifiable SMTC session; no media commands sent.\n";
            return 0;
        }
        Expect(own.SourceAppUserModelId() == identity, "Fixture session identity differs");
        if (serve)
        {
            SystemMediaTransportControlsTimelineProperties timeline;
            timeline.StartTime(0s); timeline.EndTime(60s); timeline.MinSeekTime(0s); timeline.MaxSeekTime(60s);
            timeline.Position(10s); controls.UpdateTimelineProperties(timeline);
            std::cout << "READY " << GetCurrentProcessId() << std::endl;
            std::string command;
            while (std::getline(std::cin, command) && command != "quit")
            {
                if (command == "stats")
                {
                    std::cout << "{\"playing\":" << (controls.PlaybackStatus() == MediaPlaybackStatus::Playing ? "true" : "false")
                        << ",\"pressed\":" << pressed->load() << "}" << std::endl;
                    continue;
                }
                if (command == "pause") { player.Pause(); controls.PlaybackStatus(MediaPlaybackStatus::Paused); }
                else if (command == "play") { player.Play(); controls.PlaybackStatus(MediaPlaybackStatus::Playing); }
                else if (command == "missing-cover") { updater.Thumbnail(nullptr); updater.Update(); }
                else if (command == "seek") { timeline.Position(40s); controls.UpdateTimelineProperties(timeline); }
                std::cout << "ACK " << command << std::endl;
            }
            buttons.revoke(); player.Close();
            return 0;
        }
        // 扫描固定自有会话验证身份，实际控制走带期望身份的生产队列。
        const auto playback = own.GetPlaybackInfo();
        std::cout << "Fixture capabilities: toggle=" << playback.Controls().IsPlayPauseToggleEnabled()
            << " play=" << playback.Controls().IsPlayEnabled() << " pause=" << playback.Controls().IsPauseEnabled()
            << " previous=" << playback.Controls().IsPreviousEnabled() << " next=" << playback.Controls().IsNextEnabled() << '\n';
        Expect(playback.Controls().IsPauseEnabled(), "Playing fixture has no pause capability");
        const auto properties = Await(own.TryGetMediaPropertiesAsync());
        Expect(properties.Title() == title && properties.Artist() == L"WinHalo test artist" &&
            properties.AlbumTitle() == L"WinHalo test album", "Fixture metadata missing");
        Expect(properties.Thumbnail() != nullptr, "Fixture thumbnail missing");
        {
            auto receiver = std::make_shared<Receiver>();
            MediaService service(logger);
            service.Start([receiver](MediaSnapshot value) { receiver->Receive(std::move(value)); });
            {
                std::unique_lock lock(receiver->mutex);
                const bool integrated = receiver->changed.wait_for(lock, 3s, [&]
                    { return receiver->snapshot.title == title && !receiver->snapshot.artworkBgra.empty(); });
                if (!integrated && receiver->snapshot.source != identity)
                {
                    lock.unlock();
                    service.Stop();
                    buttons.revoke();
                    player.Close();
                    std::cout << "SKIP: service selected another session; no production controls were sent.\n";
                    return 0;
                }
                Expect(integrated, "MediaService did not decode own artwork");
                Expect(receiver->snapshot.available && receiver->snapshot.canToggle && receiver->snapshot.canNext && receiver->snapshot.canPrevious,
                    "MediaService did not publish own capabilities");
                Expect(receiver->snapshot.source == identity, "MediaService selected source differs from fixture");
                Expect(receiver->snapshot.album == L"WinHalo test album", "MediaService did not publish album metadata");
                Expect(receiver->snapshot.artworkWidth == 16 && receiver->snapshot.artworkHeight == 16,
                    "MediaService artwork dimensions differ");
                std::cout << "PASS: MediaService own-session metadata, capabilities and 16x16 premultiplied BGRA decode.\n";
            }
            service.Command(MediaCommand::Next, identity + L".wrong");
            {
                std::unique_lock lock(receiver->mutex);
                Expect(receiver->changed.wait_for(lock, 3s, [&]
                    { return receiver->snapshot.error == L"媒体会话已切换，请重新操作"; }),
                    "Mismatched expected source was not rejected");
                Expect(pressed->load() == 0, "Mismatched command reached the fixture");
            }
            Expect(own.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing,
                "Mismatched command changed own playback status");
            service.Command(MediaCommand::TogglePlayPause, identity);
            {
                std::unique_lock lock(receiver->mutex);
                Expect(receiver->changed.wait_for(lock, 3s, [&]
                    { return receiver->snapshot.source == identity && !receiver->snapshot.playing && receiver->snapshot.error.empty(); }),
                    "Production toggle did not pause own fixture");
            }
            Expect(own.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused,
                "Production toggle did not change own playback status to paused");
            service.Command(MediaCommand::TogglePlayPause, identity);
            {
                std::unique_lock lock(receiver->mutex);
                Expect(receiver->changed.wait_for(lock, 3s, [&]
                    { return receiver->snapshot.source == identity && receiver->snapshot.playing && receiver->snapshot.error.empty(); }),
                    "Production toggle did not play own fixture");
            }
            Expect(own.GetPlaybackInfo().PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing,
                "Production toggle did not change own playback status to playing");
            service.Command(MediaCommand::Next, identity);
            service.Command(MediaCommand::Previous, identity);
            const auto callbacksBy = std::chrono::steady_clock::now() + 3s;
            while (pressed->load() < 2 && std::chrono::steady_clock::now() < callbacksBy)
                std::this_thread::sleep_for(20ms);
            Expect(pressed->load() == 2, "Production next/previous queue did not reach own fixture");
            buttons.revoke();
            controls.IsEnabled(false);
            player.Close();
            {
                std::unique_lock lock(receiver->mutex);
                Expect(receiver->changed.wait_for(lock, 4s, [&]
                    { return receiver->snapshot.source != identity; }),
                    "Player exit did not release the media session");
                Expect(receiver->snapshot.available || (!receiver->snapshot.playing && receiver->snapshot.artworkBgra.empty() &&
                    !receiver->snapshot.canToggle && !receiver->snapshot.canNext && !receiver->snapshot.canPrevious),
                    "Exited session retained playback controls or cover");
            }
            service.Stop();
        }
        buttons.revoke();
        std::cout << "PASS: production MediaService queue source-mismatch rejection, own-session play/pause/previous/next and player exit with worker alive; no user session controlled.\n";
        return 0;
    }
    catch (const winrt::hresult_error& error)
    {
        std::wcerr << L"Fixture unavailable: " << error.message().c_str() << L'\n';
        return 2;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
