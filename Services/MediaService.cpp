#include "MediaService.h"
#include "MediaInterop.h"
#include "MediaAppResolver.h"
#include "../Core/Platform.h"
#include "../Utils/Logger.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cwchar>
#include <deque>
#include <mutex>
#include <type_traits>
#include <utility>
#include <unordered_map>

namespace winisland
{
namespace
{
using namespace std::chrono_literals;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus;
using MediaSession = GlobalSystemMediaTransportControlsSession;
using MediaManager = GlobalSystemMediaTransportControlsSessionManager;
using Clock = std::chrono::steady_clock;
enum Dirty : unsigned int { SessionDirty = 1, MetadataDirty = 2, PlaybackDirty = 4, TimelineDirty = 8 };
constexpr unsigned int AllDirty = SessionDirty | MetadataDirty | PlaybackDirty | TimelineDirty;
constexpr std::uint64_t MaximumThumbnailBytes = 5 * 1024 * 1024;
constexpr unsigned int MaximumArtworkSize = 256;

struct QueuedCommand
{
    MediaCommand command;
    std::wstring expectedSource;
    std::uint64_t expectedSession = 0;
    enum class Kind { Control, NextSession, OpenApp } kind = Kind::Control;
};

struct AsyncCompletion
{
    std::mutex mutex;
    std::condition_variable_any ready;
    bool completed = false;
};

// 完成回调只持有独立等待状态；超时取消后仍然安全，不会访问服务对象。
template<class Operation>
auto Await(const Operation& operation, std::stop_token stop, Clock::time_point deadline)
    -> decltype(operation.GetResults())
{
    if (!operation)
    {
        using Result = decltype(operation.GetResults());
        if constexpr (std::is_constructible_v<Result, std::nullptr_t>) return Result{ nullptr };
        else return Result{};
    }
    auto completion = std::make_shared<AsyncCompletion>();
    operation.Completed([completion](const auto&, AsyncStatus) noexcept
    {
        {
            std::lock_guard lock(completion->mutex);
            completion->completed = true;
        }
        completion->ready.notify_all();
    });
    std::unique_lock lock(completion->mutex);
    const bool finished = completion->ready.wait_until(lock, stop, deadline,
        [&completion] { return completion->completed; });
    lock.unlock();
    if (!finished || stop.stop_requested())
    {
        try { operation.Cancel(); } catch (...) {}
        if (stop.stop_requested()) throw winrt::hresult_canceled();
        throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"媒体服务请求超时");
    }
    return operation.GetResults();
}

std::wstring LimitedText(const winrt::hstring& text, std::size_t maximum = 256)
{
    return std::wstring(text.c_str(), std::min<std::size_t>(text.size(), maximum));
}

std::wstring CurrentError()
{
    try { throw; }
    catch (const winrt::hresult_error& error) { return LimitedText(error.message(), 384); }
    catch (...) { return L"媒体服务暂时不可用"; }
}

struct Apartment
{
    Apartment() { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    ~Apartment() { winrt::uninit_apartment(); }
};
}

struct MediaServiceState
{
    std::mutex mutex;
    std::condition_variable_any changed;
    unsigned int dirty = AllDirty;
    bool stopping = false;
    std::deque<QueuedCommand> commands;
    std::function<void(MediaSnapshot)> sink;
};

namespace
{
void Signal(const std::weak_ptr<MediaServiceState>& weak, unsigned int dirty) noexcept
{
    if (const auto state = weak.lock())
    {
        {
            std::lock_guard lock(state->mutex);
            if (state->stopping) return;
            state->dirty |= dirty;
        }
        state->changed.notify_all();
    }
}

void Publish(const std::shared_ptr<MediaServiceState>& state, const MediaSnapshot& snapshot) noexcept
{
    try
    {
        std::function<void(MediaSnapshot)> sink;
        {
            std::lock_guard lock(state->mutex);
            if (state->stopping) return;
            sink = state->sink;
        }
        if (sink) sink(snapshot);
    }
    catch (...) {} // UI 订阅者的异常不能终止后台线程。
}

std::vector<unsigned char> ReadThumbnail(
    const winrt::Windows::Storage::Streams::IRandomAccessStreamReference& thumbnail,
    std::stop_token stop)
{
    using namespace winrt::Windows::Storage::Streams;
    if (!thumbnail) return {};
    const auto deadline = Clock::now() + 2500ms;
    winrt::Windows::Foundation::IAsyncOperation<IRandomAccessStreamWithContentType> opening{ nullptr };
    if (FAILED(media_interop::Read(thumbnail,
        &media_interop::Abi<IRandomAccessStreamReference>::OpenReadAsync, opening))) return {};
    const auto opened = media_interop::Await(opening, stop, deadline);
    if (FAILED(opened.code)) return {};
    const auto& stream = opened.value;
    if (!stream) return {};
    const auto random = stream.try_as<IRandomAccessStream>();
    std::uint64_t size = 0;
    if (FAILED(media_interop::Read(random, &media_interop::Abi<IRandomAccessStream>::get_Size, size))) return {};
    if (size == 0) { media_interop::Close(stream); return {}; }
    if (size > MaximumThumbnailBytes)
    {
        media_interop::Close(stream);
        throw winrt::hresult_error(E_INVALIDARG, L"媒体封面超过 5 MB 限制");
    }
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
    IInputStream input{ nullptr };
    media_interop::Read(random, &media_interop::Abi<IRandomAccessStream>::GetInputStreamAt, input, std::uint64_t{0});
    if (!input) { media_interop::Close(stream); return {}; }
    std::size_t offset = 0;
    try
    {
        while (offset < bytes.size())
        {
            const auto count = static_cast<std::uint32_t>(std::min<std::size_t>(65536, bytes.size() - offset));
            winrt::Windows::Foundation::IAsyncOperationWithProgress<IBuffer, std::uint32_t> reading{ nullptr };
            const Buffer destination(count);
            const HRESULT started = static_cast<media_interop::Abi<IInputStream>*>(winrt::get_abi(input))->ReadAsync(
                winrt::get_abi(destination), count, static_cast<std::uint32_t>(InputStreamOptions::None), winrt::put_abi(reading));
            if (FAILED(started)) return {};
            const auto result = media_interop::Await(reading, stop, deadline);
            if (FAILED(result.code)) return {};
            const auto& buffer = result.value;
            if (!buffer) { media_interop::Close(input); media_interop::Close(stream); return {}; }
            const auto read = buffer.Length();
            if (read == 0 || read > count)
                throw winrt::hresult_error(E_FAIL, L"媒体封面读取不完整");
            const auto reader = DataReader::FromBuffer(buffer);
            if (!reader) { media_interop::Close(input); media_interop::Close(stream); return {}; }
            reader.ReadBytes(winrt::array_view<unsigned char>(bytes.data() + offset, bytes.data() + offset + read));
            offset += read;
        }
        media_interop::Close(input);
        media_interop::Close(stream);
    }
    catch (...)
    {
        media_interop::Close(input); media_interop::Close(stream);
        throw;
    }
    return bytes;
}

void DecodeArtwork(IWICImagingFactory* factory, const std::vector<unsigned char>& encoded, MediaSnapshot& snapshot)
{
    winrt::com_ptr<IWICStream> stream;
    winrt::check_hresult(factory->CreateStream(stream.put()));
    winrt::check_hresult(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()), static_cast<DWORD>(encoded.size())));
    winrt::com_ptr<IWICBitmapDecoder> decoder;
    winrt::check_hresult(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()));
    winrt::com_ptr<IWICBitmapFrameDecode> frame;
    winrt::check_hresult(decoder->GetFrame(0, frame.put()));
    UINT width = 0;
    UINT height = 0;
    winrt::check_hresult(frame->GetSize(&width, &height));
    if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
        static_cast<std::uint64_t>(width) * height > 16 * 1024 * 1024)
        throw winrt::hresult_error(E_INVALIDARG, L"媒体封面尺寸超出限制");
    const double scale = std::min(1.0, static_cast<double>(MaximumArtworkSize) / std::max(width, height));
    const auto targetWidth = std::max(1u, static_cast<unsigned int>(std::lround(width * scale)));
    const auto targetHeight = std::max(1u, static_cast<unsigned int>(std::lround(height * scale)));
    winrt::com_ptr<IWICBitmapScaler> scaler;
    winrt::check_hresult(factory->CreateBitmapScaler(scaler.put()));
    winrt::check_hresult(scaler->Initialize(frame.get(), targetWidth, targetHeight, WICBitmapInterpolationModeFant));
    winrt::com_ptr<IWICFormatConverter> converter;
    winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
    winrt::check_hresult(converter->Initialize(scaler.get(), GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom));
    std::vector<unsigned char> pixels(static_cast<std::size_t>(targetWidth) * targetHeight * 4);
    winrt::check_hresult(converter->CopyPixels(nullptr, targetWidth * 4,
        static_cast<UINT>(pixels.size()), pixels.data()));
    snapshot.artworkBgra = std::move(pixels);
    snapshot.artworkWidth = targetWidth;
    snapshot.artworkHeight = targetHeight;
}

struct WorkerContext
{
    struct Entry
    {
        MediaSession session{nullptr};
        std::uint64_t id = 0;
        std::wstring source;
        bool playing = false;
        MediaSession::PlaybackInfoChanged_revoker playbackChanged;
    };
    std::vector<Entry> entries;
    std::uint64_t nextId = 0, manualId = 0;
    std::unordered_map<std::wstring, std::shared_ptr<const MediaAppInfo>> apps;
    MediaManager manager{ nullptr };
    MediaManager::CurrentSessionChanged_revoker currentSessionChanged;
    MediaManager::SessionsChanged_revoker sessionsChanged;
    MediaSession session{ nullptr };
    MediaSession::MediaPropertiesChanged_revoker mediaPropertiesChanged;
    MediaSession::PlaybackInfoChanged_revoker playbackInfoChanged;
    MediaSession::TimelinePropertiesChanged_revoker timelinePropertiesChanged;
    winrt::com_ptr<IWICImagingFactory> imagingFactory;
    MediaSnapshot snapshot;
    std::vector<unsigned char> encodedArtwork;
    std::uint64_t artworkRevision = 0;
    HRESULT requestError = S_OK;
    const wchar_t* requestOperation = L"";

    template<class Result, class Object, class Method>
    Result Read(const Object& object, Method method, const wchar_t* operation)
    {
        Result result = media_interop::Empty<Result>();
        const HRESULT code = media_interop::Read(object, method, result);
        if (FAILED(code) && SUCCEEDED(requestError))
        { requestError = code; requestOperation = operation; }
        return result;
    }

    void ClearSession() noexcept
    {
        mediaPropertiesChanged.revoke();
        playbackInfoChanged.revoke();
        timelinePropertiesChanged.revoke();
        session = nullptr;
        if (!snapshot.artworkBgra.empty()) ++artworkRevision;
        encodedArtwork.clear();
        auto choices = std::move(snapshot.sessions);
        snapshot = {};
        snapshot.sessions = std::move(choices);
        snapshot.artworkRevision = artworkRevision;
        snapshot.capturedAt = Clock::now();
    }

    void AttachManager(const std::shared_ptr<MediaServiceState>& state, std::stop_token stop)
    {
        manager = Await(MediaManager::RequestAsync(), stop, Clock::now() + 2500ms);
        if (!manager) throw winrt::hresult_error(E_FAIL, L"媒体会话管理器暂时不可用");
        const std::weak_ptr<MediaServiceState> weak = state;
        currentSessionChanged = manager.CurrentSessionChanged(winrt::auto_revoke,
            [weak](const auto&, const auto&) noexcept { Signal(weak, AllDirty); });
        sessionsChanged = manager.SessionsChanged(winrt::auto_revoke,
            [weak](const auto&, const auto&) noexcept { Signal(weak, AllDirty); });
    }

    void RefreshSessions(const std::shared_ptr<MediaServiceState>& state)
    {
        using Sessions = winrt::Windows::Foundation::Collections::IVectorView<MediaSession>;
        const auto sessions = Read<Sessions>(manager, &media_interop::Abi<MediaManager>::GetSessions, L"GetSessions");
        std::uint32_t count = 0;
        if (FAILED(media_interop::Read(sessions, &media_interop::Abi<Sessions>::get_Size, count))) return;
        std::vector<Entry> discovered;
        for (std::uint32_t i = 0; i < count; ++i)
        {
            MediaSession candidate{nullptr};
            if (FAILED(media_interop::Read(sessions, &media_interop::Abi<Sessions>::GetAt, candidate, i)) || !candidate) continue;
            winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackInfo playback{ nullptr };
            media_interop::Read(candidate, &media_interop::Abi<MediaSession>::GetPlaybackInfo, playback);
            GlobalSystemMediaTransportControlsSessionPlaybackStatus status{};
            if (!playback || FAILED(media_interop::Read(playback,
                &media_interop::Abi<decltype(playback)>::get_PlaybackStatus, status)) ||
                status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed) continue;
            const auto previous = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.session == candidate; });
            Entry entry;
            if (previous != entries.end()) entry = std::move(*previous);
            else
            {
                entry.session = candidate; entry.id = ++nextId;
                winrt::hstring source;
                if (FAILED(media_interop::Read(candidate, &media_interop::Abi<MediaSession>::get_SourceAppUserModelId, source))) continue;
                entry.source.assign(source.c_str(), source.size());
                const std::weak_ptr<MediaServiceState> weak = state;
                using namespace winrt::Windows::Media::Control;
                const winrt::Windows::Foundation::TypedEventHandler<MediaSession, PlaybackInfoChangedEventArgs> callback{
                    [weak](const auto&, const auto&) noexcept { Signal(weak, SessionDirty); }};
                media_interop::Subscribe(candidate, &media_interop::Abi<MediaSession>::add_PlaybackInfoChanged, callback, entry.playbackChanged);
            }
            entry.playing = status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
            discovered.push_back(std::move(entry));
        }
        // IDs survive enumeration ordering changes and distinguish sessions from the same app.
        std::sort(discovered.begin(), discovered.end(), [](const Entry& a, const Entry& b) { return a.id < b.id; });
        entries = std::move(discovered);
        snapshot.sessions.clear();
        for (const auto& entry : entries)
        {
            const auto app = apps.find(entry.source);
            snapshot.sessions.push_back({entry.id, entry.source, app == apps.end() ? entry.source : app->second->name, entry.playing});
        }
    }

    bool SelectSession(const std::shared_ptr<MediaServiceState>& state)
    {
        const Entry* selectedEntry = nullptr;
        if (manualId)
        {
            const auto pinned = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.id == manualId; });
            if (pinned != entries.end()) selectedEntry = &*pinned;
            else manualId = 0;
        }
        if (!selectedEntry)
        {
            MediaSession current{nullptr};
            media_interop::Read(manager, &media_interop::Abi<MediaManager>::GetCurrentSession, current);
            const auto system = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.session == current; });
            if (system != entries.end()) selectedEntry = &*system;
            else
            {
                const auto playing = std::find_if(entries.begin(), entries.end(), [](const Entry& entry) { return entry.playing; });
                if (playing != entries.end()) selectedEntry = &*playing;
                else if (!entries.empty()) selectedEntry = &entries.front();
            }
        }
        auto selected = selectedEntry ? selectedEntry->session : MediaSession{nullptr};
        if (selected == session) return false;
        ClearSession();
        session = std::move(selected);
        if (!session) return true;
        const std::weak_ptr<MediaServiceState> weak = state;
        using namespace winrt::Windows::Media::Control;
        const winrt::Windows::Foundation::TypedEventHandler<MediaSession, MediaPropertiesChangedEventArgs> metadataCallback{
            [weak](const auto&, const auto&) noexcept { Signal(weak, MetadataDirty | PlaybackDirty | TimelineDirty); }};
        const winrt::Windows::Foundation::TypedEventHandler<MediaSession, PlaybackInfoChangedEventArgs> playbackCallback{
            [weak](const auto&, const auto&) noexcept { Signal(weak, PlaybackDirty | TimelineDirty); }};
        const winrt::Windows::Foundation::TypedEventHandler<MediaSession, TimelinePropertiesChangedEventArgs> timelineCallback{
            [weak](const auto&, const auto&) noexcept { Signal(weak, TimelineDirty); }};
        requestOperation = L"Subscribe media session events";
        requestError = media_interop::Subscribe(session, &media_interop::Abi<MediaSession>::add_MediaPropertiesChanged,
            metadataCallback, mediaPropertiesChanged);
        if (SUCCEEDED(requestError)) requestError = media_interop::Subscribe(session,
            &media_interop::Abi<MediaSession>::add_PlaybackInfoChanged, playbackCallback, playbackInfoChanged);
        if (SUCCEEDED(requestError)) requestError = media_interop::Subscribe(session,
            &media_interop::Abi<MediaSession>::add_TimelinePropertiesChanged, timelineCallback, timelinePropertiesChanged);
        if (FAILED(requestError)) return true;
        snapshot.available = true;
        const auto source = Read<winrt::hstring>(session,
            &media_interop::Abi<MediaSession>::get_SourceAppUserModelId, L"SourceAppUserModelId");
        snapshot.source.assign(source.c_str(), source.size()); // 身份用于匹配，不能截断。
        snapshot.sessionId = selectedEntry->id;
        auto found = apps.find(snapshot.source);
        if (found == apps.end())
        {
            if (apps.size() >= 64) apps.clear();
            found = apps.emplace(snapshot.source, std::make_shared<const MediaAppInfo>(ResolveMediaApp(snapshot.source))).first;
        }
        snapshot.app = found->second;
        for (auto& choice : snapshot.sessions) if (choice.source == snapshot.source) choice.name = snapshot.app->name;
        return true;
    }

    void RefreshPlayback()
    {
        const auto playback = Read<winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackInfo>(
            session, &media_interop::Abi<MediaSession>::GetPlaybackInfo, L"GetPlaybackInfo");
        const auto status = playback ? Read<GlobalSystemMediaTransportControlsSessionPlaybackStatus>(playback,
            &media_interop::Abi<decltype(playback)>::get_PlaybackStatus, L"PlaybackStatus") :
            GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed;
        if (!playback || status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed)
        {
            ClearSession();
            return;
        }
        const auto controls = Read<winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackControls>(
            playback, &media_interop::Abi<decltype(playback)>::get_Controls, L"Controls");
        snapshot.playing = status == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        snapshot.canToggle = controls && (controls.IsPlayPauseToggleEnabled() ||
            (snapshot.playing ? controls.IsPauseEnabled() : controls.IsPlayEnabled()));
        snapshot.canPrevious = controls && controls.IsPreviousEnabled();
        snapshot.canNext = controls && controls.IsNextEnabled();
        snapshot.playbackRate = 1;
        if (const auto rate = playback.PlaybackRate())
        {
            const double value = rate.Value();
            if (std::isfinite(value) && value > 0 && value <= 16) snapshot.playbackRate = value;
        }
    }

    void RefreshTimeline()
    {
        const auto timeline = Read<winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionTimelineProperties>(
            session, &media_interop::Abi<MediaSession>::GetTimelineProperties, L"GetTimelineProperties");
        if (!timeline)
        {
            snapshot.positionSeconds = snapshot.durationSeconds = 0;
            snapshot.capturedAt = Clock::now();
            return;
        }
        const double start = std::chrono::duration<double>(Read<winrt::Windows::Foundation::TimeSpan>(timeline,
            &media_interop::Abi<decltype(timeline)>::get_StartTime, L"StartTime")).count();
        const double end = std::chrono::duration<double>(Read<winrt::Windows::Foundation::TimeSpan>(timeline,
            &media_interop::Abi<decltype(timeline)>::get_EndTime, L"EndTime")).count();
        snapshot.durationSeconds = std::max(0.0, end - start);
        double position = std::chrono::duration<double>(Read<winrt::Windows::Foundation::TimeSpan>(timeline,
            &media_interop::Abi<decltype(timeline)>::get_Position, L"Position")).count() - start;
        // Position 对应 LastUpdatedTime，而非查询时刻；补上播放器两次上报之间的时间。
        const double age = std::chrono::duration<double>(winrt::clock::now() - Read<winrt::Windows::Foundation::DateTime>(timeline,
            &media_interop::Abi<decltype(timeline)>::get_LastUpdatedTime, L"LastUpdatedTime")).count();
        if (snapshot.playing && std::isfinite(age) && age >= 0 && age <= 24 * 60 * 60)
            position += age * snapshot.playbackRate;
        snapshot.positionSeconds = std::max(0.0, position);
        if (snapshot.durationSeconds > 0)
            snapshot.positionSeconds = std::min(snapshot.positionSeconds, snapshot.durationSeconds);
        snapshot.capturedAt = Clock::now();
    }

    void RefreshMetadata(std::stop_token stop)
    {
        using Properties = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionMediaProperties;
        const auto operation = Read<winrt::Windows::Foundation::IAsyncOperation<Properties>>(
            session, &media_interop::Abi<MediaSession>::TryGetMediaPropertiesAsync, L"TryGetMediaPropertiesAsync");
        if (FAILED(requestError)) return;
        const auto result = media_interop::Await(operation, stop, Clock::now() + 2500ms);
        if (FAILED(result.code))
        { requestError = result.code; requestOperation = L"TryGetMediaPropertiesAsync.GetResults"; return; }
        const auto& properties = result.value;
        if (!properties) { ClearSession(); return; }
        snapshot.title = LimitedText(properties.Title());
        snapshot.artist = LimitedText(properties.Artist());
        snapshot.album = LimitedText(properties.AlbumTitle());
        try
        {
            auto encoded = ReadThumbnail(properties.Thumbnail(), stop);
            // 同一封面不重复解码，也不改变版本号；播放进度更新只复用像素缓存。
            if (encoded == encodedArtwork) return;
            if (encoded.empty())
            {
                snapshot.artworkBgra.clear();
                snapshot.artworkWidth = snapshot.artworkHeight = 0;
            }
            else
            {
                if (!imagingFactory)
                    winrt::check_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                        IID_PPV_ARGS(imagingFactory.put())));
                DecodeArtwork(imagingFactory.get(), encoded, snapshot);
            }
            encodedArtwork = std::move(encoded);
            snapshot.artworkRevision = ++artworkRevision;
        }
        catch (...)
        {
            if (stop.stop_requested()) throw;
            if (!snapshot.artworkBgra.empty()) snapshot.artworkRevision = ++artworkRevision;
            snapshot.artworkBgra.clear();
            snapshot.artworkWidth = snapshot.artworkHeight = 0;
            encodedArtwork.clear();
            snapshot.error = L"封面暂时不可用：" + CurrentError();
        }
    }

    void Execute(const QueuedCommand& queued, std::stop_token stop)
    {
        if (!session) { snapshot.error = L"当前没有可控制的媒体会话"; return; }
        if (queued.expectedSession && queued.expectedSession != snapshot.sessionId)
        { snapshot.error = L"媒体会话已切换，请重新操作"; return; }
        if (!queued.expectedSource.empty() &&
            (queued.expectedSource != snapshot.source || Read<winrt::hstring>(session,
                &media_interop::Abi<MediaSession>::get_SourceAppUserModelId, L"SourceAppUserModelId") != queued.expectedSource))
        {
            snapshot.error = L"媒体会话已切换，请重新操作";
            return;
        }
        const auto playback = Read<winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackInfo>(
            session, &media_interop::Abi<MediaSession>::GetPlaybackInfo, L"GetPlaybackInfo");
        if (!playback || playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed)
        { ClearSession(); return; }
        const auto controls = playback.Controls();
        if (!controls) { snapshot.error = L"当前播放器不支持此操作"; return; }
        winrt::Windows::Foundation::IAsyncOperation<bool> operation{ nullptr };
        switch (queued.command)
        {
        case MediaCommand::TogglePlayPause:
            if (controls.IsPlayPauseToggleEnabled()) operation = Read<decltype(operation)>(session,
                &media_interop::Abi<MediaSession>::TryTogglePlayPauseAsync, L"TryTogglePlayPauseAsync");
            else if (playback.PlaybackStatus() == GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing && controls.IsPauseEnabled())
                operation = Read<decltype(operation)>(session, &media_interop::Abi<MediaSession>::TryPauseAsync, L"TryPauseAsync");
            else if (playback.PlaybackStatus() != GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing && controls.IsPlayEnabled())
                operation = Read<decltype(operation)>(session, &media_interop::Abi<MediaSession>::TryPlayAsync, L"TryPlayAsync");
            break;
        case MediaCommand::Previous:
            if (controls.IsPreviousEnabled()) operation = Read<decltype(operation)>(session,
                &media_interop::Abi<MediaSession>::TrySkipPreviousAsync, L"TrySkipPreviousAsync");
            break;
        case MediaCommand::Next:
            if (controls.IsNextEnabled()) operation = Read<decltype(operation)>(session,
                &media_interop::Abi<MediaSession>::TrySkipNextAsync, L"TrySkipNextAsync");
            break;
        }
        if (!operation) { snapshot.error = L"当前播放器不支持此操作"; return; }
        const auto completed = media_interop::Await(operation, stop, Clock::now() + 2000ms);
        if (FAILED(completed.code)) { requestError = completed.code; requestOperation = L"Media control.GetResults"; }
        else if (!completed.value)
            snapshot.error = L"播放器未执行请求，请在播放器内重试";
    }
};

void RunWorker(const std::shared_ptr<MediaServiceState>& state, std::stop_token stop) noexcept
{
    try
    {
        Apartment apartment;
        WorkerContext context;
        bool retrySession = false;
        while (!stop.stop_requested())
        {
            unsigned int dirty = 0;
            std::deque<QueuedCommand> commands;
            {
                std::unique_lock lock(state->mutex);
                const auto ready = [&state]
                    { return state->stopping || state->dirty != 0 || !state->commands.empty(); };
                if (retrySession)
                {
                    state->changed.wait_for(lock, stop, 1s, ready);
                    state->dirty |= AllDirty;
                }
                else state->changed.wait(lock, stop, ready);
                if (state->stopping || stop.stop_requested()) break;
                dirty = std::exchange(state->dirty, 0u);
                commands.swap(state->commands);
            }
            context.snapshot.error.clear();
            context.requestError = S_OK;
            retrySession = false;
            try
            {
                const bool hadSession = static_cast<bool>(context.session);
                if (!context.manager) { context.AttachManager(state, stop); dirty |= AllDirty; }
                if (!commands.empty()) dirty |= SessionDirty;
                // A direct user action also keeps the displayed player selected when pausing
                // causes Windows to nominate another currently playing app.
                if (!commands.empty() && commands.front().kind != QueuedCommand::Kind::NextSession &&
                    ((commands.front().expectedSession && commands.front().expectedSession == context.snapshot.sessionId) ||
                        (!commands.front().expectedSession && !commands.front().expectedSource.empty() &&
                            commands.front().expectedSource == context.snapshot.source)))
                    context.manualId = context.snapshot.sessionId;
                // A playback event can report Closed before SessionsChanged arrives.
                if ((dirty & (SessionDirty | PlaybackDirty)) != 0)
                {
                    context.RefreshSessions(state);
                    if (context.SelectSession(state)) dirty |= AllDirty;
                }
                for (const auto& command : commands)
                {
                    if (stop.stop_requested()) break;
                    if (command.kind == QueuedCommand::Kind::NextSession)
                    {
                        if (command.expectedSession == context.snapshot.sessionId && context.entries.size() > 1)
                        {
                            const auto current = std::find_if(context.entries.begin(), context.entries.end(),
                                [&](const WorkerContext::Entry& entry) { return entry.id == command.expectedSession; });
                            if (current != context.entries.end())
                            {
                                const auto index = static_cast<size_t>(current - context.entries.begin());
                                context.manualId = context.entries[(index + 1) % context.entries.size()].id;
                                if (context.SelectSession(state)) dirty |= AllDirty;
                            }
                        }
                    }
                    else if (command.kind == QueuedCommand::Kind::OpenApp)
                    {
                        if (context.snapshot.sessionId == command.expectedSession && context.snapshot.app)
                        {
                            context.snapshot.appError = OpenMediaApp(*context.snapshot.app) ? L"" : L"暂时无法打开此播放器窗口";
                            ++context.snapshot.appRequest;
                        }
                    }
                    else context.Execute(command, stop);
                    dirty |= PlaybackDirty | TimelineDirty;
                }
                if (context.session)
                {
                    if ((dirty & PlaybackDirty) != 0) context.RefreshPlayback();
                    if (context.session && (dirty & TimelineDirty) != 0) context.RefreshTimeline();
                    if (context.session && (dirty & MetadataDirty) != 0) context.RefreshMetadata(stop);
                }
                if (hadSession && !context.session) retrySession = true;
                if (FAILED(context.requestError))
                {
                    context.ClearSession();
                    wchar_t diagnostic[256]{};
                    swprintf_s(diagnostic, L"媒体调用 %s 失败 (HRESULT 0x%08X)",
                        context.requestOperation, static_cast<unsigned int>(context.requestError));
                    context.snapshot.error = diagnostic;
                    OutputDebugStringW(diagnostic);
                    OutputDebugStringW(L"\n");
                    retrySession = true;
                }
            }
            catch (...)
            {
                if (stop.stop_requested()) break;
                const auto error = CurrentError();
                // Calls racing player shutdown must not leave stale artwork, controls
                // or continuous playback frames. Retry selection after a bounded wait.
                context.ClearSession();
                context.snapshot.error = error;
                retrySession = true;
                // 初始化失败允许稍后恢复，同时保留无媒体的正常降级状态。
                if (!context.manager)
                {
                    Publish(state, context.snapshot);
                    std::unique_lock lock(state->mutex);
                    state->changed.wait_for(lock, stop, 5s, [&state] { return state->stopping; });
                    if (state->stopping || stop.stop_requested()) break;
                    state->dirty |= AllDirty;
                    continue;
                }
            }
            if (!stop.stop_requested()) Publish(state, context.snapshot);
        }
        context.ClearSession();
        context.entries.clear();
        // revoker 先于 MTA 撤销，不让事件在析构后访问应用或 COM 对象。
        context.currentSessionChanged.revoke();
        context.sessionsChanged.revoke();
    }
    catch (...)
    {
        if (!stop.stop_requested())
        {
            MediaSnapshot failure;
            failure.error = CurrentError();
            failure.capturedAt = Clock::now();
            Publish(state, failure);
        }
    }
}
}

MediaService::MediaService(Logger& logger) : logger_(logger) {}
MediaService::~MediaService() { Stop(); }

void MediaService::Start(std::function<void(MediaSnapshot)> sink)
{
    Stop();
    auto state = std::make_shared<MediaServiceState>();
    state->sink = std::move(sink);
    worker_ = std::jthread([state](std::stop_token stop) { RunWorker(state, stop); });
    state_ = std::move(state);
    logger_.Write("Media service worker started");
}

void MediaService::Stop() noexcept
{
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->commands.clear();
    }
    worker_.request_stop();
    state_->changed.notify_all();
    if (worker_.joinable())
    {
        if (worker_.get_id() == std::this_thread::get_id()) worker_.detach();
        else worker_.join();
    }
    state_.reset();
    logger_.Write("Media service worker stopped");
}

void MediaService::Command(MediaCommand command, std::wstring expectedSource, std::uint64_t expectedSession)
{
    const auto state = state_;
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopping || state->commands.size() >= 16) return;
        state->commands.push_back({ command, std::move(expectedSource), expectedSession });
    }
    state->changed.notify_all();
}

void MediaService::NextSession(std::uint64_t expectedSession)
{
    const auto state = state_;
    if (!state || !expectedSession) return;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopping || state->commands.size() >= 16) return;
        state->commands.push_back({MediaCommand::TogglePlayPause, {}, expectedSession, QueuedCommand::Kind::NextSession});
    }
    state->changed.notify_all();
}

void MediaService::OpenApp(std::uint64_t expectedSession)
{
    const auto state = state_;
    if (!state || !expectedSession) return;
    {
        std::lock_guard lock(state->mutex);
        if (state->stopping || state->commands.size() >= 16) return;
        state->commands.push_back({MediaCommand::TogglePlayPause, {}, expectedSession, QueuedCommand::Kind::OpenApp});
    }
    state->changed.notify_all();
}
}
