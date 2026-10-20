// ==WindhawkMod==
// @id              taskbar-media-island
// @name            Taskbar Media Island
// @description     A simple, minimalistic media control (track, artist, previous / play-pause / next) on the taskbar of the monitors of your choice, secondary monitors by default
// @version         0.3.2
// @author          flavienkohler
// @github          https://github.com/flavienkohler
// @license         GPL-3.0
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lwindowsapp -lgdiplus -ld2d1 -ldwrite -lgdi32 -luser32 -luuid -lshcore -ladvapi32 -lshell32
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.

// ==WindhawkModReadme==
/*
# Taskbar Media Island

A simple, minimalistic media control on the taskbar: the album art, the title and artist of the current track, and previous / play-pause / next buttons. It is a deliberately basic first version: it does what is needed to control the music without leaving a game or an application, nothing more. It shows the media session Windows considers current, so it works with any player that integrates with the system media controls (browsers, Spotify, music players, ...).

By default it appears on the taskbars of the **secondary** monitors only, which is useful next to a mod that already shows a media control on the main taskbar. It can also be shown on the main taskbar or on all of them.

The island is hidden when nothing is playing, while the taskbar is hidden (auto-hide), and while a full-screen application is running.

It is drawn as a separate window above the taskbar, not inside it, so it does not take part in the taskbar layout: choose a position and a margin that leave it clear of the taskbar buttons.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- monitors: secondary
  $name: Monitors
  $description: The taskbars on which the island is shown.
  $options:
  - secondary: Secondary monitors only
  - primary: Primary monitor only
  - all: All monitors
- position: left
  $name: Position
  $description: Side of the taskbar on which the island is placed.
  $options:
  - left: Left
  - right: Right
- style: pill
  $name: Style
  $description: A rounded background, or no background so that the controls blend into the taskbar.
  $options:
  - pill: Rounded background
  - transparent: No background
- margin: 12
  $name: Margin
  $description: Distance between the island and the edge of the taskbar, in pixels at 100% scaling. On the right side, leave room for the clock.
- width: 300
  $name: Width
  $description: Width of the island, in pixels at 100% scaling.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <d2d1.h>
#include <dwrite.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

using namespace winrt::Windows::Media::Control;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncInfo;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Storage::Streams::IRandomAccessStream;
using winrt::Windows::Storage::Streams::IRandomAccessStreamReference;
using winrt::Windows::Storage::Streams::IRandomAccessStreamWithContentType;

// Declared in shcore.h, which is not always available.
extern "C" HRESULT WINAPI CreateStreamOverRandomAccessStream(IUnknown* stream,
                                                             REFIID riid,
                                                             void** ppv);

enum class Monitors {
    secondary,
    primary,
    all,
};

enum class Position {
    left,
    right,
};

enum class Style {
    pill,
    transparent,
};

struct {
    Monitors monitors;
    Position position;
    Style style;
    int margin;
    int width;
} g_settings;

std::mutex g_settingsMutex;

constexpr UINT kMsgMediaChanged = WM_APP + 1;
constexpr UINT kMsgReconcile = WM_APP + 2;
constexpr UINT kMsgQuit = WM_APP + 3;
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerIntervalMs = 1000;
constexpr WCHAR kIslandClassName[] = L"Windhawk_TaskbarMediaIsland";

// Buttons of the island, from left to right.
enum Button {
    kButtonPrevious,
    kButtonPlayPause,
    kButtonNext,
    kButtonCount,
    kButtonNone = -1,
};

////////////////////////////////////////////////////////////////////////////////
// Media state, written by the media thread, read by the UI thread.

struct MediaState {
    bool hasSession = false;
    bool playing = false;
    std::wstring title;
    std::wstring artist;
    std::wstring trackKey;  // Identifies the track the art belongs to.
    std::unique_ptr<Gdiplus::Bitmap> art;
};

std::mutex g_mediaMutex;
MediaState g_media;
GlobalSystemMediaTransportControlsSession g_session{nullptr};

////////////////////////////////////////////////////////////////////////////////
// Threads.

HANDLE g_uiThread;
HANDLE g_mediaThread;
HANDLE g_stopEvent;
HANDLE g_refreshEvent;
HANDLE g_uiReadyEvent;
std::atomic<HWND> g_controlWnd;
ULONG_PTR g_gdiplusToken;
std::atomic<bool> g_fullscreenAppRunning;

// After a command, the playback state is polled quickly until this time
// (GetTickCount64), to show its effect as soon as possible.
std::atomic<ULONGLONG> g_fastPollUntil;

// State shown after a play/pause click, kept until the player reports it or
// the time is over, so that a poll made too early does not undo it.
std::atomic<ULONGLONG> g_expectedUntil;
std::atomic<bool> g_expectedPlaying;

// Wakes the media thread, to take the fast polling into account.
HANDLE g_wakeEvent;

////////////////////////////////////////////////////////////////////////////////
// Media thread.

std::unique_ptr<Gdiplus::Bitmap> DecodeThumbnail(
    IRandomAccessStream const& stream) {
    winrt::com_ptr<IStream> istream;
    if (FAILED(CreateStreamOverRandomAccessStream(
            winrt::get_unknown(stream), IID_IStream, istream.put_void()))) {
        return nullptr;
    }

    Gdiplus::Bitmap source(istream.get());
    if (source.GetLastStatus() != Gdiplus::Ok || source.GetWidth() == 0 ||
        source.GetHeight() == 0) {
        return nullptr;
    }

    // Keep a small square copy, the stream cannot be used after this function.
    constexpr int kSize = 256;
    auto art = std::make_unique<Gdiplus::Bitmap>(kSize, kSize,
                                                 PixelFormat32bppARGB);
    Gdiplus::Graphics graphics(art.get());
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

    // Center crop.
    UINT side = std::min(source.GetWidth(), source.GetHeight());
    graphics.DrawImage(
        &source, Gdiplus::Rect(0, 0, kSize, kSize),
        static_cast<INT>((source.GetWidth() - side) / 2),
        static_cast<INT>((source.GetHeight() - side) / 2),
        static_cast<INT>(side), static_cast<INT>(side), Gdiplus::UnitPixel);

    return art;
}

struct MediaSubscription {
    GlobalSystemMediaTransportControlsSession session{nullptr};
    winrt::event_token propertiesToken;
    winrt::event_token playbackToken;
};

void Unsubscribe(MediaSubscription& subscription) {
    if (!subscription.session) {
        return;
    }

    try {
        subscription.session.MediaPropertiesChanged(
            subscription.propertiesToken);
        subscription.session.PlaybackInfoChanged(subscription.playbackToken);
    } catch (...) {
    }

    subscription.session = nullptr;
}

void Subscribe(MediaSubscription& subscription,
               GlobalSystemMediaTransportControlsSession const& session) {
    subscription.session = session;
    subscription.propertiesToken =
        session.MediaPropertiesChanged([](auto&&, auto&&) {
            SetEvent(g_refreshEvent);
        });
    subscription.playbackToken = session.PlaybackInfoChanged([](auto&&, auto&&) {
        SetEvent(g_refreshEvent);
    });
}

void NotifyMediaChanged() {
    if (HWND controlWnd = g_controlWnd) {
        PostMessage(controlWnd, kMsgMediaChanged, 0, 0);
    }
}

// The media properties and the album art can take a long time to arrive, up to
// seconds depending on the player. They are requested asynchronously, so that
// a slow one never delays the handling of the next event (a new track, the
// next button, ...). The requests in progress are counted, to wait for them
// when the mod is unloaded.
using Clock = std::chrono::steady_clock;

std::atomic<uint64_t> g_refreshGeneration;
std::atomic<int> g_pendingOperations;
std::mutex g_operationsMutex;
IAsyncInfo g_propertiesOperation{nullptr};
IAsyncInfo g_artOperation{nullptr};

long long ElapsedMs(Clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now() - since)
        .count();
}

void CancelOperation(IAsyncInfo& operation) {
    if (operation) {
        try {
            operation.Cancel();
        } catch (...) {
        }
        operation = nullptr;
    }
}

void StartArtLoad(IRandomAccessStreamReference const& reference,
                  std::wstring key) {
    if (!reference) {
        return;
    }

    auto started = Clock::now();
    g_pendingOperations++;

    try {
        auto operation = reference.OpenReadAsync();

        {
            std::lock_guard<std::mutex> guard(g_operationsMutex);
            CancelOperation(g_artOperation);
            g_artOperation = operation.as<IAsyncInfo>();
        }

        operation.Completed([key, started](
                                IAsyncOperation<IRandomAccessStreamWithContentType> const&
                                    operation,
                                AsyncStatus status) {
            if (status == AsyncStatus::Completed) {
                try {
                    auto art = DecodeThumbnail(operation.GetResults());

                    std::lock_guard<std::mutex> guard(g_mediaMutex);
                    if (art && g_media.trackKey == key) {
                        g_media.art = std::move(art);
                        Wh_Log(L"Album art ready after %lld ms",
                               ElapsedMs(started));
                        NotifyMediaChanged();
                    }
                } catch (...) {
                    Wh_Log(L"Could not load the album art");
                }
            }

            g_pendingOperations--;
        });
    } catch (...) {
        Wh_Log(L"Could not request the album art");
        g_pendingOperations--;
    }
}

void OnPropertiesReady(
    IAsyncOperation<GlobalSystemMediaTransportControlsSessionMediaProperties> const&
        operation,
    AsyncStatus status,
    uint64_t generation,
    Clock::time_point started) {
    // A newer request replaces this one.
    if (status != AsyncStatus::Completed ||
        generation != g_refreshGeneration) {
        return;
    }

    auto properties = operation.GetResults();
    std::wstring title = properties.Title().c_str();
    std::wstring artist = properties.Artist().c_str();
    std::wstring key = title + L"\n" + artist;

    bool needArt;
    {
        std::lock_guard<std::mutex> guard(g_mediaMutex);
        g_media.title = title;
        g_media.artist = artist;
        if (key != g_media.trackKey) {
            g_media.trackKey = key;
            g_media.art.reset();
        }

        // Players often provide the art after the title: it is requested
        // again on the next event until it is loaded.
        needArt = !g_media.art;
    }

    Wh_Log(L"Media properties ready after %lld ms", ElapsedMs(started));
    NotifyMediaChanged();

    if (needArt) {
        StartArtLoad(properties.Thumbnail(), key);
    }
}

void StartPropertiesLoad(
    GlobalSystemMediaTransportControlsSession const& session) {
    uint64_t generation = ++g_refreshGeneration;
    auto started = Clock::now();
    g_pendingOperations++;

    try {
        auto operation = session.TryGetMediaPropertiesAsync();

        {
            std::lock_guard<std::mutex> guard(g_operationsMutex);
            CancelOperation(g_propertiesOperation);
            g_propertiesOperation = operation.as<IAsyncInfo>();
        }

        operation.Completed(
            [generation, started](
                IAsyncOperation<GlobalSystemMediaTransportControlsSessionMediaProperties> const&
                    operation,
                AsyncStatus status) {
                try {
                    OnPropertiesReady(operation, status, generation, started);
                } catch (...) {
                    Wh_Log(L"Could not read the media properties");
                }

                g_pendingOperations--;
            });
    } catch (...) {
        Wh_Log(L"Could not request the media properties");
        g_pendingOperations--;
    }
}

// Reads whether the session is playing. Returns whether the state changed.
bool UpdatePlaybackState(
    GlobalSystemMediaTransportControlsSession const& session) {
    bool playing =
        session.GetPlaybackInfo().PlaybackStatus() ==
        GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

    if (GetTickCount64() < g_expectedUntil) {
        if (playing == g_expectedPlaying) {
            g_expectedUntil = 0;
        } else {
            playing = g_expectedPlaying;
        }
    }

    std::lock_guard<std::mutex> guard(g_mediaMutex);
    bool changed = !g_media.hasSession || g_media.playing != playing;
    g_session = session;
    g_media.hasSession = true;
    g_media.playing = playing;
    return changed;
}

// The playback change events are not reliable with every player, they can be
// missing, or come before the state is updated. The state is also polled.
void PollPlaybackState(
    GlobalSystemMediaTransportControlsSessionManager const& manager) {
    try {
        auto session = manager.GetCurrentSession();
        if (session && UpdatePlaybackState(session)) {
            NotifyMediaChanged();
        }
    } catch (...) {
    }
}

// Reads the current session. What is quick to get (is there a session, is it
// playing) is published at once, the title, artist and album art follow when
// they arrive.
void RefreshMedia(GlobalSystemMediaTransportControlsSessionManager const&
                      manager,
                  MediaSubscription& subscription) {
    auto session = manager.GetCurrentSession();

    if (session != subscription.session) {
        Unsubscribe(subscription);
        if (session) {
            Subscribe(subscription, session);
        }
    }

    if (!session) {
        g_refreshGeneration++;
        {
            std::lock_guard<std::mutex> guard(g_mediaMutex);
            g_session = nullptr;
            g_media = MediaState{};
        }
        NotifyMediaChanged();
        return;
    }

    try {
        UpdatePlaybackState(session);
    } catch (...) {
        Wh_Log(L"Could not read the playback state");
    }
    NotifyMediaChanged();

    StartPropertiesLoad(session);
}

DWORD WINAPI MediaThread(LPVOID) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        return 1;
    }

    {
        GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
        MediaSubscription subscription;
        winrt::event_token sessionChangedToken;

        try {
            manager =
                GlobalSystemMediaTransportControlsSessionManager::RequestAsync()
                    .get();
            sessionChangedToken =
                manager.CurrentSessionChanged([](auto&&, auto&&) {
                    SetEvent(g_refreshEvent);
                });
        } catch (...) {
            Wh_Log(L"Could not get the media session manager");
        }

        if (manager) {
            HANDLE events[] = {g_stopEvent, g_refreshEvent, g_wakeEvent};
            bool refresh = true;

            for (;;) {
                if (refresh) {
                    try {
                        RefreshMedia(manager, subscription);
                    } catch (...) {
                        Wh_Log(L"Could not read the media session");
                    }
                }

                bool fast = GetTickCount64() < g_fastPollUntil;
                DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events,
                                                    FALSE, fast ? 150 : 1000);
                if (wait == WAIT_TIMEOUT || wait == WAIT_OBJECT_0 + 2) {
                    PollPlaybackState(manager);
                    refresh = false;
                    continue;
                }

                if (wait != WAIT_OBJECT_0 + 1) {
                    break;
                }

                // Several events usually come together.
                Sleep(60);
                refresh = true;
            }

            try {
                manager.CurrentSessionChanged(sessionChangedToken);
            } catch (...) {
            }
        }

        Unsubscribe(subscription);

        // Stop the requests in progress, and wait for their handlers: they run
        // code of this mod, which is about to be unloaded.
        g_refreshGeneration++;
        {
            std::lock_guard<std::mutex> guard(g_operationsMutex);
            CancelOperation(g_propertiesOperation);
            CancelOperation(g_artOperation);
        }
        for (int i = 0; i < 1000 && g_pendingOperations > 0; i++) {
            Sleep(10);
        }
        if (g_pendingOperations > 0) {
            Wh_Log(L"%d media requests are still pending", g_pendingOperations.load());
        }

        std::lock_guard<std::mutex> guard(g_mediaMutex);
        g_session = nullptr;
        g_media = MediaState{};
    }

    winrt::uninit_apartment();
    return 0;
}

void SendCommand(Button button) {
    GlobalSystemMediaTransportControlsSession session{nullptr};
    {
        std::lock_guard<std::mutex> guard(g_mediaMutex);
        session = g_session;
    }

    if (!session) {
        return;
    }

    // Poll quickly for a while, to show the effect of the command. For
    // play/pause the expected state is shown at once.
    ULONGLONG now = GetTickCount64();
    g_fastPollUntil = now + 3000;
    if (button == kButtonPlayPause) {
        {
            std::lock_guard<std::mutex> guard(g_mediaMutex);
            g_expectedPlaying = !g_media.playing;
            g_expectedUntil = now + 1500;
            g_media.playing = g_expectedPlaying;
        }
        NotifyMediaChanged();
    }
    SetEvent(g_wakeEvent);

    try {
        switch (button) {
            case kButtonPrevious:
                (void)session.TrySkipPreviousAsync();
                break;
            case kButtonPlayPause:
                (void)session.TryTogglePlayPauseAsync();
                break;
            case kButtonNext:
                (void)session.TrySkipNextAsync();
                break;
            default:
                break;
        }
    } catch (...) {
    }
}

////////////////////////////////////////////////////////////////////////////////
// Islands, one for each managed taskbar. Used by the UI thread only.

struct Island {
    HWND window = nullptr;
    HWND taskbar = nullptr;
    int width = 0;
    int height = 0;
    bool dirty = true;
    bool trackingMouse = false;
    Button hover = kButtonNone;
    RECT buttons[kButtonCount]{};
};

std::unordered_map<HWND, std::unique_ptr<Island>> g_islands;

bool IsTaskbarClass(HWND hWnd) {
    WCHAR className[32];
    if (!GetClassName(hWnd, className, ARRAYSIZE(className))) {
        return false;
    }

    return _wcsicmp(className, L"Shell_TrayWnd") == 0 ||
           _wcsicmp(className, L"Shell_SecondaryTrayWnd") == 0;
}

std::vector<HWND> FindTaskbars() {
    std::vector<HWND> taskbars;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD processId = 0;
            GetWindowThreadProcessId(hWnd, &processId);
            if (processId == GetCurrentProcessId() && IsTaskbarClass(hWnd)) {
                reinterpret_cast<std::vector<HWND>*>(lParam)->push_back(hWnd);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&taskbars));

    return taskbars;
}

bool IsMonitorManaged(HMONITOR monitor) {
    std::lock_guard<std::mutex> guard(g_settingsMutex);

    if (g_settings.monitors == Monitors::all) {
        return true;
    }

    bool isPrimary =
        monitor == MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return (g_settings.monitors == Monitors::primary) == isPrimary;
}

bool UsesLightTheme() {
    DWORD value = 0;
    DWORD size = sizeof(value);
    if (RegGetValue(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                    L"Personalize",
                    L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
                    &size) != ERROR_SUCCESS) {
        return false;
    }

    return value != 0;
}

void AddRoundedRect(Gdiplus::GraphicsPath& path,
                    Gdiplus::RectF const& rect,
                    float radius) {
    float d = std::min(radius * 2, std::min(rect.Width, rect.Height));
    path.AddArc(rect.X, rect.Y, d, d, 180, 90);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270, 90);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0, 90);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

// Text and glyphs are drawn with DirectWrite (grayscale anti-aliasing, the
// right mode on a transparent window), the shapes and the album art with GDI+.
winrt::com_ptr<ID2D1Factory> g_d2dFactory;
winrt::com_ptr<IDWriteFactory> g_dwriteFactory;

bool HasFontFamily(PCWSTR name) {
    winrt::com_ptr<IDWriteFontCollection> collection;
    if (FAILED(g_dwriteFactory->GetSystemFontCollection(collection.put(),
                                                        FALSE))) {
        return false;
    }

    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) &&
           exists;
}

PCWSTR PickFontFamily(std::initializer_list<PCWSTR> names) {
    for (PCWSTR name : names) {
        if (HasFontFamily(name)) {
            return name;
        }
    }

    return L"Segoe UI";
}

winrt::com_ptr<IDWriteTextFormat> MakeTextFormat(
    PCWSTR family,
    float size,
    DWRITE_FONT_WEIGHT weight,
    DWRITE_TEXT_ALIGNMENT alignment) {
    winrt::com_ptr<IDWriteTextFormat> format;
    if (FAILED(g_dwriteFactory->CreateTextFormat(
            family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"", format.put()))) {
        return nullptr;
    }

    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    winrt::com_ptr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(g_dwriteFactory->CreateEllipsisTrimmingSign(format.get(),
                                                              ellipsis.put()))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        format->SetTrimming(&trimming, ellipsis.get());
    }

    return format;
}

struct IslandText {
    std::wstring title;
    std::wstring artist;
    bool playing;
    D2D1_COLOR_F foreground;
    D2D1_COLOR_F dimmed;
    D2D1_RECT_F titleRect;
    D2D1_RECT_F artistRect;
    D2D1_RECT_F buttonRects[kButtonCount];
    float titleSize;
    float artistSize;
    float glyphSize;
};

// Draws the title, the artist and the buttons on the bitmap selected in dc.
void DrawIslandText(HDC dc, int w, int h, IslandText const& text) {
    if (!g_d2dFactory || !g_dwriteFactory) {
        return;
    }

    D2D1_RENDER_TARGET_PROPERTIES properties{};
    properties.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
    properties.pixelFormat = {DXGI_FORMAT_B8G8R8A8_UNORM,
                              D2D1_ALPHA_MODE_PREMULTIPLIED};
    properties.dpiX = 96;
    properties.dpiY = 96;

    winrt::com_ptr<ID2D1DCRenderTarget> target;
    if (FAILED(g_d2dFactory->CreateDCRenderTarget(&properties, target.put()))) {
        return;
    }

    RECT bounds{0, 0, w, h};
    if (FAILED(target->BindDC(dc, &bounds))) {
        return;
    }

    winrt::com_ptr<ID2D1SolidColorBrush> foreground;
    winrt::com_ptr<ID2D1SolidColorBrush> dimmed;
    target->CreateSolidColorBrush(text.foreground, foreground.put());
    target->CreateSolidColorBrush(text.dimmed, dimmed.put());

    PCWSTR textFamily = PickFontFamily({L"Segoe UI Variable Text", L"Segoe UI"});
    PCWSTR glyphFamily =
        PickFontFamily({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});

    auto titleFormat =
        MakeTextFormat(textFamily, text.titleSize, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                       DWRITE_TEXT_ALIGNMENT_LEADING);
    auto artistFormat =
        MakeTextFormat(textFamily, text.artistSize, DWRITE_FONT_WEIGHT_NORMAL,
                       DWRITE_TEXT_ALIGNMENT_LEADING);
    auto glyphFormat =
        MakeTextFormat(glyphFamily, text.glyphSize, DWRITE_FONT_WEIGHT_NORMAL,
                       DWRITE_TEXT_ALIGNMENT_CENTER);

    target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target->BeginDraw();

    if (titleFormat && foreground) {
        target->DrawTextW(text.title.c_str(), (UINT32)text.title.size(),
                         titleFormat.get(), text.titleRect, foreground.get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (artistFormat && dimmed) {
        target->DrawTextW(text.artist.c_str(), (UINT32)text.artist.size(),
                         artistFormat.get(), text.artistRect, dimmed.get(),
                         D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (glyphFormat && foreground) {
        // The solid media glyphs: previous, play or pause, next.
        const WCHAR* glyphs[kButtonCount] = {
            L"\uE100",
            text.playing ? L"\uE103" : L"\uE102",
            L"\uE101",
        };

        for (int i = 0; i < kButtonCount; i++) {
            target->DrawTextW(glyphs[i], 1, glyphFormat.get(),
                             text.buttonRects[i], foreground.get(),
                             D2D1_DRAW_TEXT_OPTIONS_NONE);
        }
    }

    target->EndDraw();
}

// Draws the island and gives it to the window.
void RenderIsland(Island& island, double scale) {
    const int w = island.width;
    const int h = island.height;

    Style style;
    {
        std::lock_guard<std::mutex> guard(g_settingsMutex);
        style = g_settings.style;
    }

    const bool light = UsesLightTheme();
    const Gdiplus::Color background =
        light ? Gdiplus::Color(238, 243, 243, 243)
              : Gdiplus::Color(238, 36, 36, 36);
    const Gdiplus::Color hover = light ? Gdiplus::Color(40, 0, 0, 0)
                                       : Gdiplus::Color(48, 255, 255, 255);

    const float pad = static_cast<float>(std::max(2.0, 4 * scale));
    const float radius = static_cast<float>(std::max(4.0, 8 * scale));
    const float artSize = h - 2 * pad;
    const float buttonSize = h - 2 * pad;
    const float buttonsLeft = w - pad - buttonSize * (float)kButtonCount;
    const float textLeft = pad + artSize + pad * 2;
    const float textWidth = std::max(0.0f, buttonsLeft - pad - textLeft);

    IslandText text{};
    text.foreground = light ? D2D1_COLOR_F{0.08f, 0.08f, 0.08f, 1.0f}
                            : D2D1_COLOR_F{1.0f, 1.0f, 1.0f, 1.0f};
    text.dimmed = light ? D2D1_COLOR_F{0.30f, 0.30f, 0.30f, 1.0f}
                        : D2D1_COLOR_F{0.78f, 0.78f, 0.78f, 1.0f};
    text.titleSize = std::max(10.0f, h * 0.31f);
    text.artistSize = std::max(9.0f, h * 0.25f);
    text.glyphSize = buttonSize * 0.46f;

    const float titleHeight = text.titleSize * 1.35f;
    const float artistHeight = text.artistSize * 1.35f;
    const float top = (h - titleHeight - artistHeight) / 2;
    text.titleRect = {textLeft, top, textLeft + textWidth, top + titleHeight};
    text.artistRect = {textLeft, top + titleHeight, textLeft + textWidth,
                       top + titleHeight + artistHeight};

    for (int i = 0; i < kButtonCount; i++) {
        island.buttons[i] = {
            static_cast<LONG>(buttonsLeft + buttonSize * i),
            static_cast<LONG>(pad),
            static_cast<LONG>(buttonsLeft + buttonSize * (i + 1)),
            static_cast<LONG>(pad + buttonSize),
        };
        text.buttonRects[i] = {(float)island.buttons[i].left,
                               (float)island.buttons[i].top,
                               (float)island.buttons[i].right,
                               (float)island.buttons[i].bottom};
    }

    Gdiplus::Bitmap bitmap(w, h, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bitmap);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        if (style == Style::transparent) {
            // Almost invisible, but not click-through.
            Gdiplus::SolidBrush hitBrush(Gdiplus::Color(2, 0, 0, 0));
            g.FillRectangle(&hitBrush, 0, 0, w, h);
        }

        if (style == Style::pill) {
            Gdiplus::GraphicsPath path;
            AddRoundedRect(path, Gdiplus::RectF(0, 0, (float)w, (float)h),
                           radius);
            Gdiplus::SolidBrush brush(background);
            g.FillPath(&brush, &path);
        }

        // Album art, and the text to draw.
        {
            std::lock_guard<std::mutex> guard(g_mediaMutex);

            Gdiplus::RectF artRect(pad, pad, artSize, artSize);
            Gdiplus::GraphicsPath path;
            AddRoundedRect(path, artRect, radius * 0.6f);

            if (g_media.art) {
                Gdiplus::GraphicsState state = g.Save();
                g.SetClip(&path);
                g.DrawImage(g_media.art.get(), artRect);
                g.Restore(state);
            } else {
                Gdiplus::SolidBrush brush(hover);
                g.FillPath(&brush, &path);
            }

            text.title = g_media.title.empty() ? L"Unknown title" : g_media.title;
            text.artist = g_media.artist;
            text.playing = g_media.playing;
        }

        // Highlight of the hovered button.
        if (island.hover != kButtonNone) {
            Gdiplus::RectF rect((float)island.buttons[island.hover].left,
                                (float)island.buttons[island.hover].top,
                                buttonSize, buttonSize);
            Gdiplus::GraphicsPath path;
            AddRoundedRect(path, rect, radius * 0.8f);
            Gdiplus::SolidBrush hoverBrush(hover);
            g.FillPath(&hoverBrush, &path);
        }
    }

    HBITMAP hBitmap = nullptr;
    if (bitmap.GetHBITMAP(Gdiplus::Color(0, 0, 0, 0), &hBitmap) !=
        Gdiplus::Ok) {
        return;
    }

    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBitmap = SelectObject(memoryDc, hBitmap);

    DrawIslandText(memoryDc, w, h, text);

    SIZE size{w, h};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(island.window, screenDc, nullptr, &size, memoryDc,
                        &source, 0, &blend, ULW_ALPHA);

    SelectObject(memoryDc, oldBitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
    DeleteObject(hBitmap);
}

// Places the island on its taskbar, or hides it.
void LayoutIsland(Island& island) {
    auto hide = [&] {
        if (IsWindowVisible(island.window)) {
            ShowWindow(island.window, SW_HIDE);
        }
    };

    RECT taskbarRect;
    if (!IsWindow(island.taskbar) ||
        !GetWindowRect(island.taskbar, &taskbarRect)) {
        hide();
        return;
    }

    bool hasSession;
    {
        std::lock_guard<std::mutex> guard(g_mediaMutex);
        hasSession = g_media.hasSession;
    }

    // The taskbar is moved away when it auto-hides, a few pixels stay visible.
    HMONITOR monitor =
        MonitorFromWindow(island.taskbar, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{.cbSize = sizeof(monitorInfo)};
    RECT visible{};
    int taskbarHeight = taskbarRect.bottom - taskbarRect.top;
    bool taskbarShown =
        GetMonitorInfo(monitor, &monitorInfo) &&
        IntersectRect(&visible, &taskbarRect, &monitorInfo.rcMonitor) &&
        (visible.bottom - visible.top) * 2 >= taskbarHeight &&
        (visible.right - visible.left) * 2 >=
            (taskbarRect.right - taskbarRect.left);

    if (!hasSession || !taskbarShown || g_fullscreenAppRunning ||
        !IsWindowVisible(island.taskbar) || !IsMonitorManaged(monitor)) {
        hide();
        return;
    }

    Position position;
    int margin;
    int width;
    {
        std::lock_guard<std::mutex> guard(g_settingsMutex);
        position = g_settings.position;
        margin = g_settings.margin;
        width = g_settings.width;
    }

    double scale = GetDpiForWindow(island.taskbar) / 96.0;
    int pad = static_cast<int>(6 * scale);
    int height = taskbarHeight - 2 * pad;
    if (height < 16) {
        height = taskbarHeight;
        pad = 0;
    }
    width = std::max(static_cast<int>(width * scale), height * 3);
    margin = static_cast<int>(margin * scale);

    int x = position == Position::left ? taskbarRect.left + margin
                                       : taskbarRect.right - margin - width;
    int y = taskbarRect.top + (taskbarHeight - height) / 2;

    if (island.width != width || island.height != height || island.dirty) {
        island.width = width;
        island.height = height;
        island.dirty = false;
        RenderIsland(island, scale);
    }

    // Stay right above the taskbar in the z-order.
    HWND previous = GetWindow(island.taskbar, GW_HWNDPREV);
    UINT flags = SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW;
    HWND insertAfter = previous ? previous : HWND_TOPMOST;
    if (previous == island.window) {
        flags |= SWP_NOZORDER;
    }
    SetWindowPos(island.window, insertAfter, x, y, 0, 0, flags);
}

void LayoutAllIslands() {
    for (auto& [taskbar, island] : g_islands) {
        LayoutIsland(*island);
    }
}

Button HitTest(Island const& island, int x, int y) {
    POINT point{x, y};
    for (int i = 0; i < kButtonCount; i++) {
        if (PtInRect(&island.buttons[i], point)) {
            return static_cast<Button>(i);
        }
    }

    return kButtonNone;
}

LRESULT CALLBACK IslandWndProc(HWND hWnd,
                               UINT message,
                               WPARAM wParam,
                               LPARAM lParam) {
    Island* island =
        reinterpret_cast<Island*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE:
            SetWindowLongPtr(
                hWnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(
                    reinterpret_cast<CREATESTRUCT*>(lParam)->lpCreateParams));
            break;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_MOUSEMOVE:
            if (island) {
                if (!island->trackingMouse) {
                    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hWnd, 0};
                    island->trackingMouse = TrackMouseEvent(&track);
                }

                Button hover = HitTest(*island, (short)LOWORD(lParam),
                                       (short)HIWORD(lParam));
                if (hover != island->hover) {
                    island->hover = hover;
                    island->dirty = true;
                    LayoutIsland(*island);
                }
            }
            return 0;

        case WM_MOUSELEAVE:
            if (island) {
                island->trackingMouse = false;
                if (island->hover != kButtonNone) {
                    island->hover = kButtonNone;
                    island->dirty = true;
                    LayoutIsland(*island);
                }
            }
            return 0;

        case WM_LBUTTONUP:
            if (island) {
                Button button = HitTest(*island, (short)LOWORD(lParam),
                                        (short)HIWORD(lParam));
                if (button != kButtonNone) {
                    SendCommand(button);
                }
            }
            return 0;
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

// Creates the islands of the managed taskbars, and removes the others.
void ReconcileIslands(bool redraw) {
    std::vector<HWND> taskbars = FindTaskbars();

    for (auto it = g_islands.begin(); it != g_islands.end();) {
        bool found = std::find(taskbars.begin(), taskbars.end(), it->first) !=
                     taskbars.end();
        bool managed =
            found && IsMonitorManaged(MonitorFromWindow(
                         it->first, MONITOR_DEFAULTTONEAREST));
        if (!managed) {
            DestroyWindow(it->second->window);
            it = g_islands.erase(it);
        } else {
            ++it;
        }
    }

    for (HWND taskbar : taskbars) {
        if (g_islands.contains(taskbar) ||
            !IsMonitorManaged(
                MonitorFromWindow(taskbar, MONITOR_DEFAULTTONEAREST))) {
            continue;
        }

        auto island = std::make_unique<Island>();
        island->taskbar = taskbar;
        island->window = CreateWindowEx(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kIslandClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
            GetModuleHandle(nullptr), island.get());
        if (!island->window) {
            Wh_Log(L"Could not create the island window, error %u",
                   GetLastError());
            continue;
        }

        Wh_Log(L"Island created for taskbar %08X",
               (DWORD)(DWORD_PTR)taskbar);
        g_islands[taskbar] = std::move(island);
    }

    if (redraw) {
        for (auto& [taskbar, island] : g_islands) {
            island->dirty = true;
        }
    }

    LayoutAllIslands();
}

void CALLBACK WinEventProc(HWINEVENTHOOK,
                           DWORD event,
                           HWND hWnd,
                           LONG idObject,
                           LONG,
                           DWORD,
                           DWORD) {
    if (idObject != OBJID_WINDOW || !hWnd) {
        return;
    }

    auto it = g_islands.find(hWnd);
    if (it != g_islands.end()) {
        LayoutIsland(*it->second);
        return;
    }

    if ((event == EVENT_OBJECT_CREATE || event == EVENT_OBJECT_DESTROY) &&
        IsTaskbarClass(hWnd)) {
        if (HWND controlWnd = g_controlWnd) {
            PostMessage(controlWnd, kMsgReconcile, 0, 0);
        }
    }
}

void UpdateFullscreenState() {
    QUERY_USER_NOTIFICATION_STATE state;
    g_fullscreenAppRunning =
        SUCCEEDED(SHQueryUserNotificationState(&state)) &&
        (state == QUNS_BUSY || state == QUNS_RUNNING_D3D_FULL_SCREEN ||
         state == QUNS_PRESENTATION_MODE);
}

LRESULT CALLBACK ControlWndProc(HWND hWnd,
                                UINT message,
                                WPARAM wParam,
                                LPARAM lParam) {
    switch (message) {
        case kMsgMediaChanged:
            for (auto& [taskbar, island] : g_islands) {
                island->dirty = true;
            }
            LayoutAllIslands();
            return 0;

        case kMsgReconcile:
            ReconcileIslands(/*redraw=*/true);
            return 0;

        case kMsgQuit:
            PostQuitMessage(0);
            return 0;

        case WM_TIMER: {
            UpdateFullscreenState();

            // Redraw when the theme changes.
            static bool s_lightTheme = UsesLightTheme();
            bool lightTheme = UsesLightTheme();
            ReconcileIslands(/*redraw=*/lightTheme != s_lightTheme);
            s_lightTheme = lightTheme;
            return 0;
        }
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

DWORD WINAPI UiThread(LPVOID) {
    // The island is drawn at the real pixels of the monitor. Without this the
    // thread may inherit a DPI-unaware context, and the window would be
    // stretched, hence blurry, on monitors with a scaling above 100%.
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        return 1;
    }

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                      __uuidof(ID2D1Factory), nullptr, g_d2dFactory.put_void());
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(g_dwriteFactory.put_void()));
    if (!g_d2dFactory || !g_dwriteFactory) {
        Wh_Log(L"Direct2D or DirectWrite is not available, no text");
    }

    HINSTANCE instance = GetModuleHandle(nullptr);

    WNDCLASS islandClass{};
    islandClass.lpfnWndProc = IslandWndProc;
    islandClass.hInstance = instance;
    islandClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    islandClass.lpszClassName = kIslandClassName;
    RegisterClass(&islandClass);

    constexpr WCHAR kControlClassName[] = L"Windhawk_TaskbarMediaIsland_Control";
    WNDCLASS controlClass{};
    controlClass.lpfnWndProc = ControlWndProc;
    controlClass.hInstance = instance;
    controlClass.lpszClassName = kControlClassName;
    RegisterClass(&controlClass);

    HWND controlWnd = CreateWindowEx(0, kControlClassName, L"", 0, 0, 0, 0, 0,
                                     HWND_MESSAGE, nullptr, instance, nullptr);
    g_controlWnd = controlWnd;
    if (!controlWnd) {
        Wh_Log(L"Could not create the control window, error %u", GetLastError());
    }

    HWINEVENTHOOK hook = nullptr;
    HWINEVENTHOOK locationHook = nullptr;
    if (controlWnd) {
        DWORD processId = GetCurrentProcessId();
        hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER,
                               nullptr, WinEventProc, processId, 0,
                               WINEVENT_OUTOFCONTEXT);
        locationHook = SetWinEventHook(
            EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
            WinEventProc, processId, 0, WINEVENT_OUTOFCONTEXT);

        SetTimer(controlWnd, kTimerId, kTimerIntervalMs, nullptr);
        UpdateFullscreenState();
        ReconcileIslands(/*redraw=*/true);
    }

    SetEvent(g_uiReadyEvent);

    MSG msg;
    while (controlWnd && GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (hook) {
        UnhookWinEvent(hook);
    }
    if (locationHook) {
        UnhookWinEvent(locationHook);
    }

    g_controlWnd = nullptr;
    for (auto& [taskbar, island] : g_islands) {
        DestroyWindow(island->window);
    }
    g_islands.clear();

    if (controlWnd) {
        DestroyWindow(controlWnd);
    }

    UnregisterClass(kIslandClassName, instance);
    UnregisterClass(kControlClassName, instance);

    g_d2dFactory = nullptr;
    g_dwriteFactory = nullptr;

    winrt::uninit_apartment();
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Mod.

void LoadSettings() {
    std::lock_guard<std::mutex> guard(g_settingsMutex);

    PCWSTR monitors = Wh_GetStringSetting(L"monitors");
    g_settings.monitors = Monitors::secondary;
    if (wcscmp(monitors, L"primary") == 0) {
        g_settings.monitors = Monitors::primary;
    } else if (wcscmp(monitors, L"all") == 0) {
        g_settings.monitors = Monitors::all;
    }
    Wh_FreeStringSetting(monitors);

    PCWSTR position = Wh_GetStringSetting(L"position");
    g_settings.position =
        wcscmp(position, L"right") == 0 ? Position::right : Position::left;
    Wh_FreeStringSetting(position);

    PCWSTR style = Wh_GetStringSetting(L"style");
    g_settings.style =
        wcscmp(style, L"transparent") == 0 ? Style::transparent : Style::pill;
    Wh_FreeStringSetting(style);

    g_settings.margin = std::clamp(Wh_GetIntSetting(L"margin"), 0, 2000);
    g_settings.width = std::clamp(Wh_GetIntSetting(L"width"), 120, 1200);
}

void Wh_ModUninit();

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    Gdiplus::GdiplusStartupInput startupInput;
    if (Gdiplus::GdiplusStartup(&g_gdiplusToken, &startupInput, nullptr) !=
        Gdiplus::Ok) {
        Wh_Log(L"GdiplusStartup failed");
        return FALSE;
    }

    g_stopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_refreshEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_wakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    g_uiReadyEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_mediaThread = CreateThread(nullptr, 0, MediaThread, nullptr, 0, nullptr);
    g_uiThread = CreateThread(nullptr, 0, UiThread, nullptr, 0, nullptr);

    if (!g_stopEvent || !g_refreshEvent || !g_wakeEvent || !g_uiReadyEvent ||
        !g_mediaThread || !g_uiThread) {
        Wh_Log(L"Could not start the threads");
        Wh_ModUninit();
        return FALSE;
    }

    return TRUE;
}

void Wh_ModUninit() {
    Wh_Log(L">");

    if (g_stopEvent) {
        SetEvent(g_stopEvent);
    }

    if (g_uiThread) {
        // The message loop exists once the thread has signaled it is ready.
        if (g_uiReadyEvent) {
            WaitForSingleObject(g_uiReadyEvent, 10000);
        }
        if (HWND controlWnd = g_controlWnd) {
            PostMessage(controlWnd, kMsgQuit, 0, 0);
        }
        WaitForSingleObject(g_uiThread, INFINITE);
        CloseHandle(g_uiThread);
        g_uiThread = nullptr;
    }

    if (g_mediaThread) {
        WaitForSingleObject(g_mediaThread, INFINITE);
        CloseHandle(g_mediaThread);
        g_mediaThread = nullptr;
    }

    if (g_stopEvent) {
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
    if (g_refreshEvent) {
        CloseHandle(g_refreshEvent);
        g_refreshEvent = nullptr;
    }
    if (g_wakeEvent) {
        CloseHandle(g_wakeEvent);
        g_wakeEvent = nullptr;
    }
    if (g_uiReadyEvent) {
        CloseHandle(g_uiReadyEvent);
        g_uiReadyEvent = nullptr;
    }

    if (g_gdiplusToken) {
        Gdiplus::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

BOOL Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();

    if (HWND controlWnd = g_controlWnd) {
        PostMessage(controlWnd, kMsgReconcile, 0, 0);
    }

    return TRUE;
}
