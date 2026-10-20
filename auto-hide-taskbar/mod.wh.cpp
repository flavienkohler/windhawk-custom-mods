// ==WindhawkMod==
// @id              auto-hide-taskbar
// @name            Auto-Hide Taskbar (per monitor)
// @description     Hides each taskbar only when a window on its own monitor is maximized or overlaps it
// @version         1.3.0
// @author          flavienkohler
// @github          https://github.com/flavienkohler
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -ldwmapi -lole32 -loleaut32 -luuid -lversion
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.
//
// This is a modified version of "Taskbar auto-hide when maximized" 1.2.6 by
// m417z, published under the same license:
// https://github.com/ramensoftware/windhawk-mods/blob/main/mods/taskbar-auto-hide-when-maximized.wh.cpp
// Most of the taskbar hooking, the Alt+Tab / Win+Tab handling and the program
// exclusion are his work. Modified by flavienkohler on 2026-10-20: the decision
// is taken for each monitor on its own, monitors can be left alone, the options
// and the support of other Windows versions were removed, the gap of
// Chromium-based apps is removed, and the space of a taskbar that stays visible
// is kept out of the work area.

// ==WindhawkModReadme==
/*
# Auto-Hide Taskbar (per monitor)

The taskbar stays visible, and slides away only when a window on **the same monitor** is maximized, fullscreen, or overlaps the taskbar area. Windows on another monitor never affect it. Hovering the screen edge reveals the taskbar as usual.

Taskbars on monitors that are not selected in the settings are always visible.

When the taskbar stays visible because of an excluded program, or because its monitor is not managed, maximized windows stop above it instead of going under it.

Chromium-based apps (Chrome, VSCodium, Discord, ...) normally leave a 2 px gap at the bottom of a maximized window when the taskbar auto-hides. By default this mod hides the auto-hide state from applications on the managed monitors, so they maximize to the true screen edge.

Derived from [Taskbar auto-hide when maximized](https://windhawk.net/mods/taskbar-auto-hide-when-maximized) by m417z (GPL-3.0). Written after that mod did not behave as expected on a two-monitor setup.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- monitors: all
  $name: Monitors
  $description: Taskbars on the other monitors are always visible.
  $options:
  - all: All monitors
  - primary: Primary monitor only
  - secondary: Secondary monitors only
- maximizeToEdge: true
  $name: Maximize windows to the screen edge
  $description: Chromium-based apps (Chrome, VSCodium, Discord, ...) leave a 2 px gap at the screen edge when maximized with an auto-hide taskbar. Enable this to make them use the whole screen on the monitors managed by the mod. It applies to windows maximized afterwards, and the taskbar is still revealed by moving the pointer to the screen edge.
- reserveSpace: true
  $name: Keep the space of a visible taskbar
  $description: When the taskbar stays visible because of an excluded program, or because its monitor is not managed by the mod, maximized windows stop above the taskbar instead of going under it.
- excludedPrograms: [""]
  $name: Excluded programs
  $description: >-
    Windows of these programs never hide the taskbar, even when maximized or overlapping it. Entries can be process names, paths or application IDs, for example: mspaint.exe, C:\Windows\System32\notepad.exe, Microsoft.WindowsCalculator_8wekyb3d8bbwe!App
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <dwmapi.h>
#include <uiautomationclient.h>

#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

enum class Monitors {
    all,
    primary,
    secondary,
};

struct {
    Monitors monitors;
    bool maximizeToEdge;
    bool reserveSpace;
    std::unordered_set<std::wstring> excludedPrograms;
} g_settings;

std::atomic<bool> g_taskbarViewDllLoaded;

bool g_wasAutoHideProcessed;
bool g_wasAutoHideDisabled;
std::mutex g_winEventHookThreadMutex;
std::atomic<HANDLE> g_winEventHookThread;
std::unordered_map<void*, HWND> g_taskbarsKeptShown;
std::unordered_map<HWND, void*> g_taskbarToViewCoordinator;
UINT_PTR g_pendingEventsTimer;
std::atomic<HWND> g_multitaskingViewHwnd;
std::atomic<HWND> g_altTabViewHwnd;

// TrayUI::_HandleTrayPrivateSettingMessage
constexpr UINT kHandleTrayPrivateSettingMessage = WM_USER + 0x1CA;

enum {
    kTrayPrivateSettingAutoHideGet = 3,
    kTrayPrivateSettingAutoHideSet = 4,
};

constexpr WCHAR kUpdateTaskbarStatePendingTickCount[] =
    L"Windhawk_UpdateTaskbarStatePendingTickCount_" WH_MOD_ID;

static const UINT g_getTaskbarRectRegisteredMsg =
    RegisterWindowMessage(L"Windhawk_GetTaskbarRect_" WH_MOD_ID);

static const UINT g_updateTaskbarStateRegisteredMsg =
    RegisterWindowMessage(L"Windhawk_UpdateTaskbarState_" WH_MOD_ID);

static const UINT g_releaseWorkAreaRegisteredMsg =
    RegisterWindowMessage(L"Windhawk_ReleaseWorkArea_" WH_MOD_ID);

enum {
    kTrayUITimerHide = 2,
};

#if __cplusplus < 202302L
// Missing in older MinGW headers.
DECLARE_HANDLE(CO_MTA_USAGE_COOKIE);
WINOLEAPI CoIncrementMTAUsage(CO_MTA_USAGE_COOKIE* pCookie);
WINOLEAPI CoDecrementMTAUsage(CO_MTA_USAGE_COOKIE Cookie);
#endif

// Missing in older MinGW headers.
#ifndef EVENT_OBJECT_CLOAKED
#define EVENT_OBJECT_CLOAKED 0x8017
#endif
#ifndef EVENT_OBJECT_UNCLOAKED
#define EVENT_OBJECT_UNCLOAKED 0x8018
#endif

// Private API for window band (z-order band).
// https://blog.adeltax.com/window-z-order-in-windows-10/
using GetWindowBand_t = BOOL(WINAPI*)(HWND hWnd, PDWORD pdwBand);
GetWindowBand_t pGetWindowBand;

// https://devblogs.microsoft.com/oldnewthing/20200302-00/?p=103507
bool IsWindowCloaked(HWND hwnd) {
    BOOL isCloaked = FALSE;
    return SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &isCloaked,
                                           sizeof(isCloaked))) &&
           isCloaked;
}

enum class MultitaskingViewType {
    None,
    WinTab,
    AltTab,
};

// Detects Alt+Tab or Win+Tab and returns the type.
MultitaskingViewType GetMultitaskingViewType(HWND hWnd) {
    // Must be in the current process (explorer.exe).
    DWORD dwProcessId = 0;
    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, &dwProcessId);
    if (!dwThreadId || dwProcessId != GetCurrentProcessId()) {
        return MultitaskingViewType::None;
    }

    WCHAR className[64];
    if (!GetClassName(hWnd, className, ARRAYSIZE(className)) ||
        _wcsicmp(className, L"XamlExplorerHostIslandWindow") != 0) {
        return MultitaskingViewType::None;
    }

    // The Win+Tab window uses band ZBID_IMMERSIVE_APPCHROME.
    constexpr DWORD ZBID_IMMERSIVE_APPCHROME = 5;

    // The Alt+Tab window uses band ZBID_SYSTEM_TOOLS. The virtual desktop
    // switcher (which we don't want) uses band ZBID_IMMERSIVE_EDGY.
    constexpr DWORD ZBID_SYSTEM_TOOLS = 16;

    DWORD band = 0;
    if (!pGetWindowBand || !pGetWindowBand(hWnd, &band)) {
        return MultitaskingViewType::None;
    }

    MultitaskingViewType type;
    if (band == ZBID_IMMERSIVE_APPCHROME) {
        type = MultitaskingViewType::WinTab;
    } else if (band == ZBID_SYSTEM_TOOLS) {
        type = MultitaskingViewType::AltTab;
    } else {
        return MultitaskingViewType::None;
    }

    // Check thread description for "MultitaskingView".
    HANDLE hThread =
        OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, dwThreadId);
    if (!hThread) {
        return MultitaskingViewType::None;
    }

    bool isMultitaskingView = false;
    PWSTR description = nullptr;
    if (SUCCEEDED(GetThreadDescription(hThread, &description)) && description) {
        isMultitaskingView = wcscmp(description, L"MultitaskingView") == 0;
        LocalFree(description);
    }

    CloseHandle(hThread);
    return isMultitaskingView ? type : MultitaskingViewType::None;
}

HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            DWORD dwProcessId;
            WCHAR className[32];
            if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
                dwProcessId == GetCurrentProcessId() &&
                GetClassName(hWnd, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(lParam) = hWnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&hTaskbarWnd));

    return hTaskbarWnd;
}

bool IsTaskbarWindow(HWND hWnd) {
    WCHAR szClassName[32];
    if (!GetClassName(hWnd, szClassName, ARRAYSIZE(szClassName))) {
        return false;
    }

    return _wcsicmp(szClassName, L"Shell_TrayWnd") == 0 ||
           _wcsicmp(szClassName, L"Shell_SecondaryTrayWnd") == 0;
}

HWND FindTaskbarWindows(std::unordered_set<HWND>* secondaryTaskbarWindows) {
    secondaryTaskbarWindows->clear();

    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) {
        return nullptr;
    }

    DWORD taskbarThreadId = GetWindowThreadProcessId(hTaskbarWnd, nullptr);
    if (!taskbarThreadId) {
        return nullptr;
    }

    auto enumWindowsProc = [&secondaryTaskbarWindows](HWND hWnd) -> BOOL {
        WCHAR szClassName[32];
        if (GetClassName(hWnd, szClassName, ARRAYSIZE(szClassName)) == 0) {
            return TRUE;
        }

        if (_wcsicmp(szClassName, L"Shell_SecondaryTrayWnd") == 0) {
            secondaryTaskbarWindows->insert(hWnd);
        }

        return TRUE;
    };

    EnumThreadWindows(
        taskbarThreadId,
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto& proc = *reinterpret_cast<decltype(enumWindowsProc)*>(lParam);
            return proc(hWnd);
        },
        reinterpret_cast<LPARAM>(&enumWindowsProc));

    return hTaskbarWnd;
}

bool GetTaskbarRectForMonitor(HMONITOR monitor, RECT* rect) {
    SetRectEmpty(rect);

    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (!hTaskbarWnd) {
        return false;
    }

    SendMessage(hTaskbarWnd, g_getTaskbarRectRegisteredMsg, (WPARAM)monitor,
                (LPARAM)rect);
    return true;
}

// https://gist.github.com/m417z/451dfc2dad88d7ba88ed1814779a26b4
std::wstring GetWindowAppId(HWND hWnd) {
    // {c8900b66-a973-584b-8cae-355b7f55341b}
    constexpr winrt::guid CLSID_StartMenuCacheAndAppResolver{
        0x660b90c8,
        0x73a9,
        0x4b58,
        {0x8c, 0xae, 0x35, 0x5b, 0x7f, 0x55, 0x34, 0x1b}};

    // {de25675a-72de-44b4-9373-05170450c140}
    constexpr winrt::guid IID_IAppResolver_8{
        0xde25675a,
        0x72de,
        0x44b4,
        {0x93, 0x73, 0x05, 0x17, 0x04, 0x50, 0xc1, 0x40}};

    struct IAppResolver_8 : public IUnknown {
       public:
        virtual HRESULT STDMETHODCALLTYPE GetAppIDForShortcut() = 0;
        virtual HRESULT STDMETHODCALLTYPE GetAppIDForShortcutObject() = 0;
        virtual HRESULT STDMETHODCALLTYPE
        GetAppIDForWindow(HWND hWnd,
                          WCHAR** pszAppId,
                          void* pUnknown1,
                          void* pUnknown2,
                          void* pUnknown3) = 0;
        virtual HRESULT STDMETHODCALLTYPE
        GetAppIDForProcess(DWORD dwProcessId,
                           WCHAR** pszAppId,
                           void* pUnknown1,
                           void* pUnknown2,
                           void* pUnknown3) = 0;
    };

    HRESULT hr;
    std::wstring result;

    CO_MTA_USAGE_COOKIE cookie;
    bool mtaUsageIncreased = SUCCEEDED(CoIncrementMTAUsage(&cookie));

    winrt::com_ptr<IAppResolver_8> appResolver;
    hr = CoCreateInstance(CLSID_StartMenuCacheAndAppResolver, nullptr,
                          CLSCTX_INPROC_SERVER | CLSCTX_INPROC_HANDLER,
                          IID_IAppResolver_8, appResolver.put_void());
    if (SUCCEEDED(hr)) {
        WCHAR* pszAppId;
        hr = appResolver->GetAppIDForWindow(hWnd, &pszAppId, nullptr, nullptr,
                                            nullptr);
        if (SUCCEEDED(hr)) {
            result = pszAppId;
            CoTaskMemFree(pszAppId);
        }
    }

    appResolver = nullptr;

    if (mtaUsageIncreased) {
        CoDecrementMTAUsage(cookie);
    }

    return result;
}

std::wstring GetProcessFileName(DWORD dwProcessId) {
    HANDLE hProcess =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwProcessId);
    if (!hProcess) {
        return std::wstring{};
    }

    WCHAR processPath[MAX_PATH];

    DWORD dwSize = ARRAYSIZE(processPath);
    if (!QueryFullProcessImageName(hProcess, 0, processPath, &dwSize)) {
        CloseHandle(hProcess);
        return std::wstring{};
    }

    CloseHandle(hProcess);

    PCWSTR processFileName = wcsrchr(processPath, L'\\');
    if (!processFileName) {
        return std::wstring{};
    }

    processFileName++;
    return processFileName;
}

std::wstring GetWindowLogInfo(HWND hWnd) {
    DWORD dwProcessId = 0;
    GetWindowThreadProcessId(hWnd, &dwProcessId);
    std::wstring processName = GetProcessFileName(dwProcessId);

    WCHAR className[256];
    if (!GetClassName(hWnd, className, ARRAYSIZE(className))) {
        wcscpy_s(className, L"<unknown>");
    }

    WCHAR windowName[256];
    if (!GetWindowText(hWnd, windowName, ARRAYSIZE(windowName))) {
        windowName[0] = L'\0';
    }

    LONG style = GetWindowLong(hWnd, GWL_STYLE);
    LONG exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);

    RECT rect{};
    GetWindowRect(hWnd, &rect);

    WCHAR buffer[1024];
    swprintf_s(buffer,
               L"window %08X: PID=%u, process=%s, class=%s, name=%s, "
               L"style=0x%08X, exStyle=0x%08X, rect={%d,%d,%d,%d}",
               (DWORD)(DWORD_PTR)hWnd, dwProcessId, processName.c_str(),
               className, windowName, style, exStyle, rect.left, rect.top,
               rect.right, rect.bottom);
    return buffer;
}

bool IsWindowExcluded(HWND hWnd) {
    if (g_settings.excludedPrograms.empty()) {
        return false;
    }

    DWORD resolvedWindowProcessPathLen = 0;
    WCHAR resolvedWindowProcessPath[MAX_PATH];
    WCHAR resolvedWindowProcessPathUpper[MAX_PATH];

    DWORD dwProcessId = 0;
    if (GetWindowThreadProcessId(hWnd, &dwProcessId)) {
        HANDLE hProcess =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, dwProcessId);
        if (hProcess) {
            DWORD dwSize = ARRAYSIZE(resolvedWindowProcessPath);
            if (QueryFullProcessImageName(hProcess, 0,
                                          resolvedWindowProcessPath, &dwSize)) {
                resolvedWindowProcessPathLen = dwSize;
            }

            CloseHandle(hProcess);
        }
    }

    if (resolvedWindowProcessPathLen > 0) {
        LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE,
                      resolvedWindowProcessPath,
                      resolvedWindowProcessPathLen + 1,
                      resolvedWindowProcessPathUpper,
                      resolvedWindowProcessPathLen + 1, nullptr, nullptr, 0);
    } else {
        *resolvedWindowProcessPath = L'\0';
        *resolvedWindowProcessPathUpper = L'\0';
    }

    if (resolvedWindowProcessPathLen > 0 &&
        g_settings.excludedPrograms.contains(resolvedWindowProcessPathUpper)) {
        return true;
    }

    if (PCWSTR programFileNameUpper =
            wcsrchr(resolvedWindowProcessPathUpper, L'\\')) {
        programFileNameUpper++;
        if (*programFileNameUpper &&
            g_settings.excludedPrograms.contains(programFileNameUpper)) {
            return true;
        }
    }

    std::wstring appId = GetWindowAppId(hWnd);
    LCMapStringEx(LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, appId.data(),
                  appId.length(), appId.data(), appId.length(), nullptr,
                  nullptr, 0);
    if (g_settings.excludedPrograms.contains(appId.c_str())) {
        return true;
    }

    return false;
}

// Whether the mod manages the taskbar of this monitor. Taskbars of the other
// monitors are always kept shown.
bool IsMonitorManaged(HMONITOR monitor) {
    if (g_settings.monitors == Monitors::all) {
        return true;
    }

    bool isPrimary =
        monitor == MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    return (g_settings.monitors == Monitors::primary) == isPrimary;
}

enum class Cover {
    none,
    nextTo,  // Does not cover the taskbar, but is just above it.
    covers,
};

// How the window, being on the given monitor, covers its taskbar. A window that
// covers it hides the taskbar, unless its program is excluded. nearRect is the
// taskbar rectangle with a margin, see ShouldKeepTaskbarShown. The rectangle of
// the window is returned if it covers the taskbar or is next to it.
Cover GetWindowCover(HWND hWnd,
                     HMONITOR monitor,
                     const RECT* taskbarRect,
                     const RECT* nearRect,
                     RECT* windowRect) {
    if (!IsWindowVisible(hWnd) || IsWindowCloaked(hWnd) || IsIconic(hWnd) ||
        (GetWindowLong(hWnd, GWL_EXSTYLE) &
         (WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW))) {
        return Cover::none;
    }

    // Ignore the Alt+Tab overlay so it doesn't affect taskbar state.
    if (hWnd == g_altTabViewHwnd) {
        return Cover::none;
    }

    if (hWnd == GetShellWindow() || GetProp(hWnd, L"DesktopWindow")) {
        return Cover::none;
    }

    // Exclude menus (#32768).
    if (GetClassWord(hWnd, GCW_ATOM) == 32768) {
        return Cover::none;
    }

    if (FAILED(DwmGetWindowAttribute(hWnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     windowRect, sizeof(*windowRect)))) {
        GetWindowRect(hWnd, windowRect);
    }

    // A maximized window counts for the monitor it is on, and only for it.
    WINDOWPLACEMENT wp{
        .length = sizeof(WINDOWPLACEMENT),
    };

    if (GetWindowPlacement(hWnd, &wp) && wp.showCmd == SW_SHOWMAXIMIZED) {
        return MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST) == monitor
                   ? Cover::covers
                   : Cover::none;
    }

    // Any other window (including fullscreen and snapped ones) counts when it
    // overlaps the taskbar of this monitor.
    RECT intersectRect;
    if (IntersectRect(&intersectRect, windowRect, taskbarRect)) {
        return Cover::covers;
    }

    if (IntersectRect(&intersectRect, windowRect, nearRect)) {
        return Cover::nextTo;
    }

    return Cover::none;
}

// The taskbar is always set to auto-hide, so Windows does not keep its space
// out of the work area, and maximized windows go under it when it is shown.
// That is wanted while the taskbar waits for a window to cover it, but not
// when it stays shown for a lasting reason (excluded program, monitor not
// managed by the mod). In that case the space of the taskbar is subtracted from
// the work area, and Windows resizes the maximized windows.
std::atomic<bool> g_unloading;

struct ReservedSpace {
    RECT rect;  // Kept out of the work area, in screen pixels.
    HWND taskbarWnd;
};

std::mutex g_reservedSpaceMutex;
std::unordered_map<HMONITOR, ReservedSpace> g_reservedSpace;

bool g_workAreaSupported;  // The symbols are found, set at init.

void* g_cTray;        // The CTray of explorer, seen by RecomputeWorkArea.
void* g_cTrayStatic;  // c_trayStatic, which contains the CTray.
void* g_trayUIHostVftable;

using CTray_RecomputeWorkArea_t = int(WINAPI*)(void* pThis,
                                               HMONITOR monitor,
                                               RECT* workArea);
CTray_RecomputeWorkArea_t CTray_RecomputeWorkArea_Original;

using CTray_WorkAreaMayHaveChanged_t = void(WINAPI*)(void* pThis,
                                                     HMONITOR monitor,
                                                     HWND hWnd);
CTray_WorkAreaMayHaveChanged_t CTray_WorkAreaMayHaveChanged_Original;

////////////////////////////////////////////////////////////////////////////////
// The taskbar window can be taller than the bar that is seen, for example when
// the taskbar is styled to float with margins. The space kept out of the work
// area starts at the top of the visible bar, so that maximized windows end at
// the bar, without a strip of wallpaper in between. The distance between the
// top of the taskbar window and the top of the bar is measured with UI
// Automation, which must not be called from the thread of the taskbar (it would
// wait for itself), hence a worker thread and a cache.

std::mutex g_barMutex;
std::unordered_map<HWND, LONG> g_barInsets;  // Taskbar window -> inset.
std::unordered_set<HWND> g_barRequests;
HANDLE g_barThread;
HANDLE g_barStopEvent;
HANDLE g_barWakeEvent;

bool GetBarInset(HWND taskbarWnd, LONG* inset) {
    std::lock_guard<std::mutex> guard(g_barMutex);
    auto it = g_barInsets.find(taskbarWnd);
    if (it == g_barInsets.end()) {
        return false;
    }

    *inset = it->second;
    return true;
}

void RequestBarMeasure(HWND taskbarWnd) {
    {
        std::lock_guard<std::mutex> guard(g_barMutex);
        g_barRequests.insert(taskbarWnd);
    }

    if (g_barWakeEvent) {
        SetEvent(g_barWakeEvent);
    }
}

bool MeasureBarInset(IUIAutomation* automation,
                     IUIAutomationCondition* condition,
                     HWND taskbarWnd,
                     LONG* inset) {
    RECT windowRect;
    if (!GetWindowRect(taskbarWnd, &windowRect)) {
        return false;
    }

    winrt::com_ptr<IUIAutomationElement> root;
    winrt::com_ptr<IUIAutomationElement> frame;
    RECT frameRect;
    if (FAILED(automation->ElementFromHandle((UIA_HWND)taskbarWnd,
                                             root.put())) ||
        !root ||
        FAILED(root->FindFirst(TreeScope_Descendants, condition,
                               frame.put())) ||
        !frame || FAILED(frame->get_CurrentBoundingRectangle(&frameRect))) {
        return false;
    }

    LONG value = frameRect.top - windowRect.top;
    if (IsRectEmpty(&frameRect) || value < 0 ||
        value > (windowRect.bottom - windowRect.top) / 2) {
        return false;
    }

    *inset = value;
    return true;
}

DWORD WINAPI BarThread(LPVOID) {
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        return 1;
    }

    {
        winrt::com_ptr<IUIAutomation> automation;
        winrt::com_ptr<IUIAutomationCondition> condition;

        VARIANT className;
        VariantInit(&className);
        className.vt = VT_BSTR;
        className.bstrVal = SysAllocString(L"Taskbar.TaskbarFrameAutomationPeer");

        if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                       CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                       automation.put_void()))) {
            automation->CreatePropertyCondition(UIA_ClassNamePropertyId,
                                                className, condition.put());
        }
        VariantClear(&className);

        if (!automation || !condition) {
            Wh_Log(L"UI Automation is not available, the space of the whole "
                   L"taskbar window is kept");
        }

        HANDLE events[] = {g_barStopEvent, g_barWakeEvent};
        for (;;) {
            // Also measure again from time to time, the style can change.
            DWORD wait = WaitForMultipleObjects(ARRAYSIZE(events), events,
                                                FALSE, 5000);
            if (wait == WAIT_OBJECT_0) {
                break;
            }

            std::unordered_set<HWND> toMeasure;
            {
                std::lock_guard<std::mutex> guard(g_barMutex);
                toMeasure.swap(g_barRequests);
            }

            if (wait == WAIT_TIMEOUT) {
                std::lock_guard<std::mutex> guard(g_reservedSpaceMutex);
                for (const auto& [monitor, reserved] : g_reservedSpace) {
                    toMeasure.insert(reserved.taskbarWnd);
                }
            }

            for (HWND taskbarWnd : toMeasure) {
                LONG inset = 0;
                bool measured =
                    automation && condition &&
                    MeasureBarInset(automation.get(), condition.get(),
                                    taskbarWnd, &inset);

                bool changed;
                {
                    std::lock_guard<std::mutex> guard(g_barMutex);
                    auto it = g_barInsets.find(taskbarWnd);

                    // When it cannot be measured, the whole window is kept.
                    changed = it == g_barInsets.end() ||
                              (measured && it->second != inset);
                    if (changed) {
                        g_barInsets[taskbarWnd] = inset;
                    }
                }

                if (changed) {
                    Wh_Log(L"Visible bar of taskbar %08X: inset %ld px%s",
                           (DWORD)(DWORD_PTR)taskbarWnd, inset,
                           measured ? L"" : L" (not measured)");
                    PostMessage(taskbarWnd, g_updateTaskbarStateRegisteredMsg, 0,
                                0);
                }
            }
        }
    }

    CoUninitialize();
    return 0;
}

void StartBarThread() {
    g_barStopEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    g_barWakeEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (g_barStopEvent && g_barWakeEvent) {
        g_barThread = CreateThread(nullptr, 0, BarThread, nullptr, 0, nullptr);
    }
}

void StopBarThread() {
    if (g_barThread) {
        SetEvent(g_barStopEvent);
        WaitForSingleObject(g_barThread, 10000);
        CloseHandle(g_barThread);
        g_barThread = nullptr;
    }

    for (HANDLE* handle : {&g_barStopEvent, &g_barWakeEvent}) {
        if (*handle) {
            CloseHandle(*handle);
            *handle = nullptr;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

// The work area is computed by explorer, after that the taskbar is subtracted
// from it for the monitors with a reserved space. The return value of
// RecomputeWorkArea tells whether the computed work area differs from the
// current one of the monitor, and the callers apply it only in that case. With
// auto-hide explorer computes the whole monitor, which is the current work
// area, hence 0, so the return value is computed again after the subtraction.
int WINAPI CTray_RecomputeWorkArea_Hook(void* pThis,
                                        HMONITOR monitor,
                                        RECT* workArea) {
    g_cTray = pThis;

    int ret = CTray_RecomputeWorkArea_Original(pThis, monitor, workArea);

    if (!workArea || IsRectEmpty(workArea)) {
        return ret;
    }

    RECT reservedRect{};
    {
        std::lock_guard<std::mutex> guard(g_reservedSpaceMutex);
        auto it = g_reservedSpace.find(monitor);
        if (it != g_reservedSpace.end()) {
            reservedRect = it->second.rect;
        }
    }

    MONITORINFO monitorInfo{.cbSize = sizeof(monitorInfo)};
    if (!IsRectEmpty(&reservedRect) && GetMonitorInfo(monitor, &monitorInfo)) {
        SubtractRect(workArea, workArea, &reservedRect);
        ret = EqualRect(workArea, &monitorInfo.rcWork) ? 0 : 1;
        Wh_Log(L"Work area of monitor %p: {%d,%d,%d,%d}, current {%d,%d,%d,%d}, "
               L"%s",
               monitor, workArea->left, workArea->top, workArea->right,
               workArea->bottom, monitorInfo.rcWork.left,
               monitorInfo.rcWork.top, monitorInfo.rcWork.right,
               monitorInfo.rcWork.bottom, ret ? L"applied" : L"unchanged");
    }

    return ret;
}

// The interface of the CTray on which WorkAreaMayHaveChanged is called is
// found with its vftable rather than with a fixed offset, and nothing is called
// if it is not found.
void* FindTrayUIHost(void* cTray) {
    if (!cTray || !g_trayUIHostVftable) {
        return nullptr;
    }

    for (size_t offset = 0; offset < 0x100; offset += sizeof(void*)) {
        void** slot = reinterpret_cast<void**>(static_cast<char*>(cTray) +
                                               offset);
        if (*slot == g_trayUIHostVftable) {
            return slot;
        }
    }

    return nullptr;
}

// Asks explorer to compute the work area of the monitor again, and to apply it.
void NotifyWorkAreaChanged(HMONITOR monitor) {
    if (!CTray_WorkAreaMayHaveChanged_Original) {
        return;
    }

    void* host = FindTrayUIHost(g_cTray ? g_cTray : g_cTrayStatic);
    if (!host) {
        Wh_Log(L"CTray not found, the work area is not updated");
        return;
    }

    CTray_WorkAreaMayHaveChanged_Original(host, monitor, nullptr);
}

bool IsSpaceReserved(HMONITOR monitor) {
    std::lock_guard<std::mutex> guard(g_reservedSpaceMutex);
    return g_reservedSpace.contains(monitor);
}

void UpdateReservedSpace(HWND taskbarWnd,
                         HMONITOR monitor,
                         bool reserve,
                         const RECT& taskbarRect) {
    reserve = reserve && g_settings.reserveSpace && !g_unloading &&
              g_workAreaSupported && !IsRectEmpty(&taskbarRect);

    RECT rect = taskbarRect;
    if (reserve && g_barThread) {
        // The space starts at the visible bar. Reserving only once it is
        // measured avoids resizing the windows twice, a message is posted to
        // the taskbar when the measure is done.
        LONG inset;
        if (GetBarInset(taskbarWnd, &inset)) {
            rect.top += inset;
        } else {
            RequestBarMeasure(taskbarWnd);
            reserve = false;
        }
    }

    bool changed;
    {
        std::lock_guard<std::mutex> guard(g_reservedSpaceMutex);
        auto it = g_reservedSpace.find(monitor);

        if (reserve) {
            changed = it == g_reservedSpace.end() ||
                      !EqualRect(&it->second.rect, &rect);
            g_reservedSpace[monitor] = {rect, taskbarWnd};
        } else {
            changed = it != g_reservedSpace.end();
            if (changed) {
                g_reservedSpace.erase(it);
            }
        }
    }

    if (changed) {
        Wh_Log(L"Space of the taskbar of monitor %p: %s", monitor,
               reserve ? L"reserved" : L"released");
        NotifyWorkAreaChanged(monitor);
    }
}

void ReleaseReservedSpace() {
    std::unordered_map<HMONITOR, ReservedSpace> released;
    {
        std::lock_guard<std::mutex> guard(g_reservedSpaceMutex);
        released.swap(g_reservedSpace);
    }

    for (const auto& [monitor, reserved] : released) {
        NotifyWorkAreaChanged(monitor);
    }
}

// Whether the taskbar of the monitor is kept shown. It also decides whether
// the space of the taskbar is kept out of the work area.
bool ShouldKeepTaskbarShown(HWND hTaskbarWnd, HMONITOR monitor) {
    RECT taskbarRect{};
    GetTaskbarRectForMonitor(monitor, &taskbarRect);

    auto result = [&](bool keepShown, bool reserveSpace) {
        UpdateReservedSpace(hTaskbarWnd, monitor, reserveSpace, taskbarRect);
        return keepShown;
    };

    if (!IsMonitorManaged(monitor)) {
        Wh_Log(L"Taskbar %08X: monitor not managed, keeping shown",
               (DWORD)(DWORD_PTR)hTaskbarWnd);
        return result(true, true);
    }

    // Always show taskbar when MultitaskingView (Win+Tab) is active.
    if (g_multitaskingViewHwnd) {
        Wh_Log(L"Taskbar %08X: Win+Tab active, keeping shown",
               (DWORD)(DWORD_PTR)hTaskbarWnd);
        return result(true, false);
    }

    if (IsRectEmpty(&taskbarRect)) {
        Wh_Log(L"Taskbar %08X: empty taskbar rect, only maximized windows "
               L"will be detected",
               (DWORD)(DWORD_PTR)hTaskbarWnd);
    }

    // While the space of the taskbar is reserved, the windows are resized to
    // end just above the taskbar, and do not overlap it anymore. They still
    // have to keep the space reserved, otherwise they would grow back under the
    // taskbar, which would reserve the space again, and so on.
    RECT nearRect = taskbarRect;
    if (IsSpaceReserved(monitor)) {
        nearRect.top -= 32;
    }

    bool canHideTaskbar = false;
    bool excludedWindowCovers = false;

    // The windows are listed from the top of the z-order. A window that hides
    // the taskbar and fully contains an excluded window that is below it hides
    // that window, which then does not need the taskbar.
    std::vector<RECT> hidingWindows;

    auto contains = [](const RECT& outer, const RECT& inner) {
        constexpr LONG kMargin = 2;
        return inner.left >= outer.left - kMargin &&
               inner.top >= outer.top - kMargin &&
               inner.right <= outer.right + kMargin &&
               inner.bottom <= outer.bottom + kMargin;
    };

    DWORD dwTaskbarThreadId = GetCurrentThreadId();

    auto enumWindowsProc = [&](HWND hWnd) -> BOOL {
        if (GetWindowThreadProcessId(hWnd, nullptr) == dwTaskbarThreadId) {
            return TRUE;
        }

        RECT windowRect;
        Cover cover = GetWindowCover(hWnd, monitor, &taskbarRect, &nearRect,
                                     &windowRect);
        if (cover == Cover::none) {
            return TRUE;
        }

        // Resolving the program is the most expensive check, so it is done
        // only for the windows that cover or are next to the taskbar.
        if (IsWindowExcluded(hWnd)) {
            bool hidden = std::any_of(
                hidingWindows.begin(), hidingWindows.end(),
                [&](const RECT& hiding) { return contains(hiding, windowRect); });
            if (!hidden) {
                excludedWindowCovers = true;
            }
            return TRUE;
        }

        if (cover == Cover::covers) {
            if (!canHideTaskbar) {
                Wh_Log(L"Taskbar %08X (rect={%d,%d,%d,%d}) can hide for %s",
                       (DWORD)(DWORD_PTR)hTaskbarWnd, taskbarRect.left,
                       taskbarRect.top, taskbarRect.right, taskbarRect.bottom,
                       GetWindowLogInfo(hWnd).c_str());
            }

            canHideTaskbar = true;
            hidingWindows.push_back(windowRect);
        }

        return TRUE;
    };

    EnumWindows(
        [](HWND hWnd, LPARAM lParam) -> BOOL {
            auto& proc = *reinterpret_cast<decltype(enumWindowsProc)*>(lParam);
            return proc(hWnd);
        },
        reinterpret_cast<LPARAM>(&enumWindowsProc));

    // A window of an excluded program that can be seen keeps the taskbar, even
    // when another window that is not excluded covers it too, for example on
    // the other half of the screen. Its space is kept out of the work area.
    if (excludedWindowCovers && canHideTaskbar) {
        Wh_Log(L"Taskbar %08X: kept shown for an excluded window",
               (DWORD)(DWORD_PTR)hTaskbarWnd);
    }

    return result(!canHideTaskbar || excludedWindowCovers, excludedWindowCovers);
}

// Chromium-based apps (Chrome, Electron apps, ...) stop 2 px short of a screen
// edge when a maximized window has an auto-hide taskbar there, which leaves a
// visible gap. They find out in two ways, both answered here with "no
// auto-hide taskbar" for the monitors managed by the mod:
//
// 1. SHAppBarMessage(ABM_GETAUTOHIDEBAR[EX]) is answered by shell32 from the
//    window properties "WindowOnEdge:<monitor>:<edge>" of the main taskbar
//    window, which hold the taskbar window, or 1 for none. The values are
//    rewritten to 1 when explorer sets them, and the existing ones when the
//    mod is loaded.
// 2. If that finds nothing, SHAppBarMessage(ABM_GETSTATE) is used to check the
//    ABS_AUTOHIDE flag, which is answered by explorer.

constexpr WCHAR kWindowOnEdgePropPrefix[] = L"WindowOnEdge:";
constexpr size_t kWindowOnEdgePropPrefixLen =
    ARRAYSIZE(kWindowOnEdgePropPrefix) - 1;

constexpr UINT kAppBarStateAutoHide = 0x1;  // ABS_AUTOHIDE

std::mutex g_appBarPropsMutex;
std::unordered_map<std::wstring, HANDLE> g_hiddenAppBarProps;
std::atomic<DWORD> g_appBarMessageThreadId;
using SetPropW_t = decltype(&SetPropW);
SetPropW_t SetPropW_Original;

bool IsWindowOnEdgeProp(LPCWSTR name) {
    return !IS_INTRESOURCE(name) &&
           wcsncmp(name, kWindowOnEdgePropPrefix, kWindowOnEdgePropPrefixLen) ==
               0;
}

// Whether the value of this property, which must be a "WindowOnEdge" one, has
// to be replaced with "none".
bool ShouldHideAppBarProp(PCWSTR name, HANDLE value) {
    if (g_unloading || !g_settings.maximizeToEdge || (ULONG_PTR)value <= 1 ||
        !IsWindow((HWND)value) || !IsTaskbarWindow((HWND)value)) {
        return false;
    }

    HMONITOR monitor = (HMONITOR)(ULONG_PTR)wcstoul(
        name + kWindowOnEdgePropPrefixLen, nullptr, 16);
    return IsMonitorManaged(monitor);
}

BOOL WINAPI SetPropW_Hook(HWND hWnd, LPCWSTR lpString, HANDLE hData) {
    // Called very often by explorer, keep the common case cheap.
    if (IsWindowOnEdgeProp(lpString)) {
        std::lock_guard<std::mutex> guard(g_appBarPropsMutex);

        if (ShouldHideAppBarProp(lpString, hData)) {
            Wh_Log(L"Hiding %s = %p from applications", lpString, hData);
            g_hiddenAppBarProps[lpString] = hData;
            hData = (HANDLE)1;
        } else {
            g_hiddenAppBarProps.erase(lpString);
        }
    }

    return SetPropW_Original(hWnd, lpString, hData);
}

// SetProp that does not go through SetPropW_Hook (the caller holds the mutex).
void SetPropDirect(HWND hWnd, PCWSTR name, HANDLE value) {
    if (SetPropW_Original) {
        SetPropW_Original(hWnd, name, value);
    } else {
        SetProp(hWnd, name, value);
    }
}

// Hides the values set before the mod was loaded, and puts back the ones that
// must not be hidden anymore (settings change), or all of them.
void UpdateAppBarProps(HWND hTaskbarWnd, bool restoreAll = false) {
    {
        std::lock_guard<std::mutex> guard(g_appBarPropsMutex);
        if (!restoreAll && !g_settings.maximizeToEdge &&
            g_hiddenAppBarProps.empty()) {
            return;
        }
    }

    struct Prop {
        std::wstring name;
        HANDLE value;
    };

    std::vector<Prop> props;
    EnumPropsEx(
        hTaskbarWnd,
        [](HWND, LPWSTR name, HANDLE value, ULONG_PTR param) -> BOOL {
            if (IsWindowOnEdgeProp(name)) {
                reinterpret_cast<std::vector<Prop>*>(param)->push_back(
                    {name, value});
            }
            return TRUE;
        },
        reinterpret_cast<ULONG_PTR>(&props));

    std::lock_guard<std::mutex> guard(g_appBarPropsMutex);

    for (const auto& prop : props) {
        auto it = g_hiddenAppBarProps.find(prop.name);
        if (it != g_hiddenAppBarProps.end()) {
            if (restoreAll ||
                !ShouldHideAppBarProp(prop.name.c_str(), it->second)) {
                SetPropDirect(hTaskbarWnd, prop.name.c_str(), it->second);
                g_hiddenAppBarProps.erase(it);
            }
        } else if (ShouldHideAppBarProp(prop.name.c_str(), prop.value)) {
            Wh_Log(L"Hiding %s = %p from applications", prop.name.c_str(),
                   prop.value);
            g_hiddenAppBarProps[prop.name] = prop.value;
            SetPropDirect(hTaskbarWnd, prop.name.c_str(), (HANDLE)1);
        }
    }
}

// Marks the appbar messages sent by applications, as opposed to the calls made
// by explorer itself, which must see the real state.
using CTray__OnAppBarMessage_t = INT_PTR(WINAPI*)(void* pThis,
                                                  COPYDATASTRUCT* copyData);
CTray__OnAppBarMessage_t CTray__OnAppBarMessage_Original;
INT_PTR WINAPI CTray__OnAppBarMessage_Hook(void* pThis,
                                           COPYDATASTRUCT* copyData) {
    DWORD prevThreadId = g_appBarMessageThreadId.exchange(GetCurrentThreadId());
    INT_PTR ret = CTray__OnAppBarMessage_Original(pThis, copyData);
    g_appBarMessageThreadId = prevThreadId;
    return ret;
}

using CDeskTray_AppBarGetState_t = UINT(WINAPI*)(void* pThis);
CDeskTray_AppBarGetState_t CDeskTray_AppBarGetState_Original;
UINT WINAPI CDeskTray_AppBarGetState_Hook(void* pThis) {
    UINT state = CDeskTray_AppBarGetState_Original(pThis);

    // The state is not per monitor, it is the one of the main taskbar.
    if ((state & kAppBarStateAutoHide) && g_settings.maximizeToEdge &&
        !g_unloading && g_appBarMessageThreadId == GetCurrentThreadId() &&
        IsMonitorManaged(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY))) {
        state &= ~kAppBarStateAutoHide;
    }

    return state;
}

void* QueryViaVtable(void* object, void* vtable) {
    void* ptr = object;
    while (*(void**)ptr != vtable) {
        ptr = (void**)ptr + 1;
    }
    return ptr;
}

void* QueryViaVtableBackwards(void* object, void* vtable) {
    void* ptr = object;
    while (*(void**)ptr != vtable) {
        ptr = (void**)ptr - 1;
    }
    return ptr;
}

DWORD WINAPI WinEventHookThread(LPVOID lpThreadParameter);

void AdjustTaskbar(HWND hMMTaskbarWnd, bool clearPendingWhenDone = false) {
    if (!g_winEventHookThread) {
        std::lock_guard<std::mutex> guard(g_winEventHookThreadMutex);

        if (!g_winEventHookThread) {
            g_winEventHookThread = CreateThread(
                nullptr, 0, WinEventHookThread, nullptr, 0, nullptr);
        }
    }

    PostMessage(hMMTaskbarWnd, g_updateTaskbarStateRegisteredMsg,
                clearPendingWhenDone, 0);
}

void AdjustAllTaskbars() {
    std::unordered_set<HWND> secondaryTaskbarWindows;
    HWND hWnd = FindTaskbarWindows(&secondaryTaskbarWindows);
    if (hWnd) {
        AdjustTaskbar(hWnd);
    }

    for (HWND hSecondaryWnd : secondaryTaskbarWindows) {
        AdjustTaskbar(hSecondaryWnd);
    }
}

bool AdjustAllTaskbarsIfNotPending() {
    std::unordered_set<HWND> secondaryTaskbarWindows;
    HWND hWnd = FindTaskbarWindows(&secondaryTaskbarWindows);

    DWORD currentTickCount = GetTickCount();
    DWORD pendingTickCount = 0;

    if (hWnd) {
        DWORD tickCount = (DWORD)(DWORD_PTR)GetProp(
            hWnd, kUpdateTaskbarStatePendingTickCount);
        if (tickCount > pendingTickCount) {
            pendingTickCount = tickCount;
        }
    }

    for (HWND hSecondaryWnd : secondaryTaskbarWindows) {
        DWORD tickCount = (DWORD)(DWORD_PTR)GetProp(
            hSecondaryWnd, kUpdateTaskbarStatePendingTickCount);
        if (tickCount > pendingTickCount) {
            pendingTickCount = tickCount;
        }
    }

    // Consider times larger than 10 seconds as expired, to prevent having
    // it stuck in this state.
    if (pendingTickCount && currentTickCount - pendingTickCount < 1000 * 10) {
        return false;
    }

    if (hWnd) {
        SetProp(hWnd, kUpdateTaskbarStatePendingTickCount,
                (HANDLE)(DWORD_PTR)currentTickCount);
        AdjustTaskbar(hWnd, /*clearPendingWhenDone=*/true);
    }

    for (HWND hSecondaryWnd : secondaryTaskbarWindows) {
        SetProp(hSecondaryWnd, kUpdateTaskbarStatePendingTickCount,
                (HANDLE)(DWORD_PTR)currentTickCount);
        AdjustTaskbar(hSecondaryWnd, /*clearPendingWhenDone=*/true);
    }

    return true;
}

using ViewCoordinator_ShouldTaskbarBeExpanded_t =
    bool(WINAPI*)(void* pThis, HWND hMMTaskbarWnd, bool expanded);
ViewCoordinator_ShouldTaskbarBeExpanded_t
    ViewCoordinator_ShouldTaskbarBeExpanded_Original;
bool WINAPI ViewCoordinator_ShouldTaskbarBeExpanded_Hook(void* pThis,
                                                         HWND hMMTaskbarWnd,
                                                         bool expanded) {
    Wh_Log(L"> hMMTaskbarWnd=%08X, expanded=%d",
           (DWORD)(ULONG_PTR)hMMTaskbarWnd, expanded);

    g_taskbarToViewCoordinator[hMMTaskbarWnd] = pThis;

    // Return true if the taskbar should be kept shown.
    for (const auto& pair : g_taskbarsKeptShown) {
        if (pair.second == hMMTaskbarWnd) {
            Wh_Log(L"Returning true for taskbar kept shown");
            return true;
        }
    }

    return ViewCoordinator_ShouldTaskbarBeExpanded_Original(
        pThis, hMMTaskbarWnd, expanded);
}

using ViewCoordinator_UpdateIsExpanded_t = void(WINAPI*)(void* pThis,
                                                         HWND hMMTaskbarWnd,
                                                         int reason);
ViewCoordinator_UpdateIsExpanded_t ViewCoordinator_UpdateIsExpanded_Original;

void UpdateViewCoordinatorIsExpanded(HWND hWnd) {
    // From ViewCoordinator::HandleIsPointerOverTaskbarFrameChanged.
    constexpr int kReasonIsPointerOverTaskbarFrameChanged = 7;

    if (ViewCoordinator_UpdateIsExpanded_Original) {
        auto it = g_taskbarToViewCoordinator.find(hWnd);
        if (it != g_taskbarToViewCoordinator.end()) {
            ViewCoordinator_UpdateIsExpanded_Original(
                it->second, hWnd, kReasonIsPointerOverTaskbarFrameChanged);
        }
    }
}

void* TrayUI_vftable_IInspectable;
void* TrayUI_vftable_ITrayComponentHost;
void* CSecondaryTray_vftable_ISecondaryTray;

using TrayUI_GetStuckMonitor_t = HMONITOR(WINAPI*)(void* pThis);
TrayUI_GetStuckMonitor_t TrayUI_GetStuckMonitor_Original;

using CSecondaryTray_GetMonitor_t = HMONITOR(WINAPI*)(void* pThis);
CSecondaryTray_GetMonitor_t CSecondaryTray_GetMonitor_Original;

using TrayUI_GetStuckRectForMonitor_t = bool(WINAPI*)(void* pThis,
                                                      HMONITOR hMonitor,
                                                      RECT* rect);
TrayUI_GetStuckRectForMonitor_t TrayUI_GetStuckRectForMonitor_Original;

using TrayUI__Hide_t = void(WINAPI*)(void* pThis);
TrayUI__Hide_t TrayUI__Hide_Original;
void WINAPI TrayUI__Hide_Hook(void* pThis) {
    Wh_Log(L">");

    auto it = g_taskbarsKeptShown.find(pThis);
    if (it != g_taskbarsKeptShown.end()) {
        KillTimer(it->second, kTrayUITimerHide);
        return;
    }

    TrayUI__Hide_Original(pThis);
}

using CSecondaryTray__AutoHide_t = void(WINAPI*)(void* pThis, bool param1);
CSecondaryTray__AutoHide_t CSecondaryTray__AutoHide_Original;
void WINAPI CSecondaryTray__AutoHide_Hook(void* pThis, bool param1) {
    Wh_Log(L">");

    auto it = g_taskbarsKeptShown.find(pThis);
    if (it != g_taskbarsKeptShown.end()) {
        KillTimer(it->second, kTrayUITimerHide);
        return;
    }

    CSecondaryTray__AutoHide_Original(pThis, param1);
}

using TrayUI_Unhide_t = void(WINAPI*)(void* pThis,
                                      int trayUnhideFlags,
                                      int unhideRequest);
TrayUI_Unhide_t TrayUI_Unhide_Original;

using CSecondaryTray__Unhide_t = void(WINAPI*)(void* pThis,
                                               int trayUnhideFlags,
                                               int unhideRequest);
CSecondaryTray__Unhide_t CSecondaryTray__Unhide_Original;

using TrayUI_WndProc_t = LRESULT(WINAPI*)(void* pThis,
                                          HWND hWnd,
                                          UINT Msg,
                                          WPARAM wParam,
                                          LPARAM lParam,
                                          bool* flag);
TrayUI_WndProc_t TrayUI_WndProc_Original;
LRESULT WINAPI TrayUI_WndProc_Hook(void* pThis,
                                   HWND hWnd,
                                   UINT Msg,
                                   WPARAM wParam,
                                   LPARAM lParam,
                                   bool* flag) {
    if (Msg == WM_NCCREATE) {
        Wh_Log(L"WM_NCCREATE: %08X", (DWORD)(ULONG_PTR)hWnd);
        AdjustTaskbar(hWnd);
    } else if (Msg == WM_NCDESTROY) {
        Wh_Log(L"WM_NCDESTROY: %08X", (DWORD)(ULONG_PTR)hWnd);
        g_taskbarsKeptShown.erase(
            QueryViaVtableBackwards(pThis, TrayUI_vftable_IInspectable));
        g_taskbarToViewCoordinator.erase(hWnd);
    } else if (Msg == kHandleTrayPrivateSettingMessage) {
        // Prevent auto-hide from being disabled while the mod is loaded.
        if ((DWORD)wParam == kTrayPrivateSettingAutoHideSet) {
            BOOL bSetAutoHideEnabled = (BOOL)lParam;
            if (!bSetAutoHideEnabled) {
                return 0;
            }
        }
    } else if (Msg == g_getTaskbarRectRegisteredMsg) {
        HMONITOR monitor = (HMONITOR)wParam;
        RECT* rect = (RECT*)lParam;
        if (!TrayUI_GetStuckRectForMonitor_Original(pThis, monitor, rect)) {
            SetRectEmpty(rect);
        }
    } else if (Msg == g_releaseWorkAreaRegisteredMsg) {
        ReleaseReservedSpace();
        return 0;
    } else if (Msg == g_updateTaskbarStateRegisteredMsg) {
        UpdateAppBarProps(hWnd);

        if (!g_wasAutoHideProcessed) {
            g_wasAutoHideProcessed = true;
            g_wasAutoHideDisabled =
                !SendMessage(hWnd, kHandleTrayPrivateSettingMessage,
                             kTrayPrivateSettingAutoHideGet, 0);
            if (g_wasAutoHideDisabled) {
                SendMessage(hWnd, kHandleTrayPrivateSettingMessage,
                            kTrayPrivateSettingAutoHideSet, TRUE);
            }
        }

        HMONITOR monitor = TrayUI_GetStuckMonitor_Original(pThis);
        bool keepShown = ShouldKeepTaskbarShown(hWnd, monitor);

        void* pTrayUI_IInspectable =
            QueryViaVtableBackwards(pThis, TrayUI_vftable_IInspectable);

        bool keptShown = g_taskbarsKeptShown.contains(pTrayUI_IInspectable);

        if (keepShown != keptShown) {
            Wh_Log(L"> keepShown=%d", keepShown);

            if (keepShown) {
                g_taskbarsKeptShown[pTrayUI_IInspectable] = hWnd;

                void* pTrayUI_ITrayComponentHost =
                    QueryViaVtable(pThis, TrayUI_vftable_ITrayComponentHost);
                TrayUI_Unhide_Original(pTrayUI_ITrayComponentHost, 0, 0);

                UpdateViewCoordinatorIsExpanded(hWnd);
            } else {
                g_taskbarsKeptShown.erase(pTrayUI_IInspectable);

                SetTimer(hWnd, kTrayUITimerHide, 0, nullptr);

                UpdateViewCoordinatorIsExpanded(hWnd);
            }
        }

        if (wParam) {
            RemoveProp(hWnd, kUpdateTaskbarStatePendingTickCount);
        }
    }

    LRESULT ret =
        TrayUI_WndProc_Original(pThis, hWnd, Msg, wParam, lParam, flag);

    return ret;
}

using CSecondaryTray_v_WndProc_t = LRESULT(
    WINAPI*)(void* pThis, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);
CSecondaryTray_v_WndProc_t CSecondaryTray_v_WndProc_Original;
LRESULT WINAPI CSecondaryTray_v_WndProc_Hook(void* pThis,
                                             HWND hWnd,
                                             UINT Msg,
                                             WPARAM wParam,
                                             LPARAM lParam) {
    if (Msg == WM_NCCREATE) {
        Wh_Log(L"WM_NCCREATE: %08X", (DWORD)(ULONG_PTR)hWnd);
        AdjustTaskbar(hWnd);
    } else if (Msg == WM_NCDESTROY) {
        Wh_Log(L"WM_NCDESTROY: %08X", (DWORD)(ULONG_PTR)hWnd);
        g_taskbarsKeptShown.erase(pThis);
        g_taskbarToViewCoordinator.erase(hWnd);
    } else if (Msg == g_updateTaskbarStateRegisteredMsg) {
        void* pCSecondaryTray_ISecondaryTray =
            QueryViaVtable(pThis, CSecondaryTray_vftable_ISecondaryTray);

        HMONITOR monitor =
            CSecondaryTray_GetMonitor_Original(pCSecondaryTray_ISecondaryTray);

        bool keepShown = ShouldKeepTaskbarShown(hWnd, monitor);

        bool keptShown = g_taskbarsKeptShown.contains(pThis);

        if (keepShown != keptShown) {
            Wh_Log(L"> keepShown=%d", keepShown);

            if (keepShown) {
                g_taskbarsKeptShown[pThis] = hWnd;

                CSecondaryTray__Unhide_Original(pThis, 0, 0);

                UpdateViewCoordinatorIsExpanded(hWnd);
            } else {
                g_taskbarsKeptShown.erase(pThis);

                SetTimer(hWnd, kTrayUITimerHide, 0, nullptr);

                UpdateViewCoordinatorIsExpanded(hWnd);
            }
        }

        if (wParam) {
            RemoveProp(hWnd, kUpdateTaskbarStatePendingTickCount);
        }
    }

    LRESULT ret =
        CSecondaryTray_v_WndProc_Original(pThis, hWnd, Msg, wParam, lParam);

    return ret;
}

void CALLBACK WinEventProc(HWINEVENTHOOK hWinEventHook,
                           DWORD event,
                           HWND hWnd,
                           LONG idObject,
                           LONG idChild,
                           DWORD dwEventThread,
                           DWORD dwmsEventTime) {
    if (idObject != OBJID_WINDOW ||
        (GetWindowLong(hWnd, GWL_STYLE) & WS_CHILD) || IsTaskbarWindow(hWnd)) {
        return;
    }

    HWND hParentWnd = GetAncestor(hWnd, GA_PARENT);
    if (hParentWnd && hParentWnd != GetDesktopWindow()) {
        return;
    }

    // Check for Multitasking View window state changes.
    {
        auto multitaskingViewType = GetMultitaskingViewType(hWnd);
        bool isTrackedWinTab = hWnd == g_multitaskingViewHwnd;
        bool isTrackedAltTab = hWnd == g_altTabViewHwnd;

        if (multitaskingViewType != MultitaskingViewType::None ||
            isTrackedWinTab || isTrackedAltTab) {
            auto& targetHwnd =
                (multitaskingViewType == MultitaskingViewType::AltTab ||
                 isTrackedAltTab)
                    ? g_altTabViewHwnd
                    : g_multitaskingViewHwnd;

            bool entering = event == EVENT_OBJECT_SHOW ||
                            event == EVENT_OBJECT_UNCLOAKED ||
                            event == EVENT_OBJECT_CREATE;
            bool leaving = event == EVENT_OBJECT_HIDE ||
                           event == EVENT_OBJECT_CLOAKED ||
                           event == EVENT_OBJECT_DESTROY;

            if (entering) {
                HWND expected = nullptr;
                if (!targetHwnd.compare_exchange_strong(expected, hWnd)) {
                    return;
                }
                Wh_Log(
                    L"MultitaskingView entering (%s)",
                    &targetHwnd == &g_altTabViewHwnd ? L"Alt+Tab" : L"Win+Tab");
            } else if (leaving) {
                HWND expected = hWnd;
                if (!targetHwnd.compare_exchange_strong(expected, nullptr)) {
                    return;
                }
                Wh_Log(
                    L"MultitaskingView leaving (%s)",
                    &targetHwnd == &g_altTabViewHwnd ? L"Alt+Tab" : L"Win+Tab");
            } else {
                return;
            }

            // Fall through to trigger timer for all taskbars.
        }
    }

    if (g_pendingEventsTimer) {
        return;
    }

    g_pendingEventsTimer = SetTimer(
        nullptr, 0, 200,
        [](HWND hwnd,         // handle of window for timer messages
           UINT uMsg,         // WM_TIMER message
           UINT_PTR idEvent,  // timer identifier
           DWORD dwTime       // current system time
        ) {
            if (!AdjustAllTaskbarsIfNotPending()) {
                Wh_Log(L"Adjustment already pending, will retry later...");
                return;
            }

            KillTimer(nullptr, g_pendingEventsTimer);
            g_pendingEventsTimer = 0;
        });
}

DWORD WINAPI WinEventHookThread(LPVOID lpThreadParameter) {
    HWINEVENTHOOK winObjectEventHook1 =
        SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, nullptr,
                        WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!winObjectEventHook1) {
        Wh_Log(L"Error: SetWinEventHook");
    }

    HWINEVENTHOOK winObjectEventHook2 = SetWinEventHook(
        EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr,
        WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!winObjectEventHook2) {
        Wh_Log(L"Error: SetWinEventHook");
    }

    HWINEVENTHOOK winObjectEventHook3 =
        SetWinEventHook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED, nullptr,
                        WinEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    if (!winObjectEventHook3) {
        Wh_Log(L"Error: SetWinEventHook");
    }

    BOOL bRet;
    MSG msg;
    while ((bRet = GetMessage(&msg, NULL, 0, 0)) != 0) {
        if (bRet == -1) {
            msg.wParam = 0;
            break;
        }

        if (msg.hwnd == NULL && msg.message == WM_APP) {
            PostQuitMessage(0);
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (winObjectEventHook1) {
        UnhookWinEvent(winObjectEventHook1);
    }

    if (winObjectEventHook2) {
        UnhookWinEvent(winObjectEventHook2);
    }

    if (winObjectEventHook3) {
        UnhookWinEvent(winObjectEventHook3);
    }

    g_multitaskingViewHwnd = nullptr;
    g_altTabViewHwnd = nullptr;

    return 0;
}

bool HookTaskbarViewDllSymbols(HMODULE module) {
    // Taskbar.View.dll
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(public: bool __cdecl winrt::Taskbar::implementation::ViewCoordinator::ShouldTaskbarBeExpanded(unsigned __int64,bool))"},
            &ViewCoordinator_ShouldTaskbarBeExpanded_Original,
            ViewCoordinator_ShouldTaskbarBeExpanded_Hook,
            true,
        },
        {
            {LR"(public: void __cdecl winrt::Taskbar::implementation::ViewCoordinator::UpdateIsExpanded(unsigned __int64,enum TaskbarTipTest::TaskbarExpandCollapseReason))"},
            &ViewCoordinator_UpdateIsExpanded_Original,
            nullptr,
            true,
        },
    };

    if (!HookSymbols(module, symbolHooks, ARRAYSIZE(symbolHooks))) {
        Wh_Log(L"HookSymbols failed");
        return false;
    }

    return true;
}

HMODULE GetTaskbarViewModuleHandle() {
    HMODULE module = GetModuleHandle(L"Taskbar.View.dll");
    if (!module) {
        module = GetModuleHandle(L"ExplorerExtensions.dll");
    }

    return module;
}

void HandleLoadedModuleIfTaskbarView(HMODULE module, LPCWSTR lpLibFileName) {
    if (!g_taskbarViewDllLoaded && GetTaskbarViewModuleHandle() == module &&
        !g_taskbarViewDllLoaded.exchange(true)) {
        Wh_Log(L"Loaded %s", lpLibFileName);

        if (HookTaskbarViewDllSymbols(module)) {
            Wh_ApplyHookOperations();
        }
    }
}

VS_FIXEDFILEINFO* GetModuleVersionInfo(HMODULE hModule, UINT* puPtrLen) {
    void* pFixedFileInfo = nullptr;
    UINT uPtrLen = 0;

    HRSRC hResource =
        FindResource(hModule, MAKEINTRESOURCE(VS_VERSION_INFO), RT_VERSION);
    if (hResource) {
        HGLOBAL hGlobal = LoadResource(hModule, hResource);
        if (hGlobal) {
            void* pData = LockResource(hGlobal);
            if (pData) {
                if (!VerQueryValue(pData, L"\\", &pFixedFileInfo, &uPtrLen) ||
                    uPtrLen == 0) {
                    pFixedFileInfo = nullptr;
                    uPtrLen = 0;
                }
            }
        }
    }

    if (puPtrLen) {
        *puPtrLen = uPtrLen;
    }

    return (VS_FIXEDFILEINFO*)pFixedFileInfo;
}

// The taskbar internals hooked here are the ones of Windows 11.
bool IsWindows11() {
    VS_FIXEDFILEINFO* fixedFileInfo = GetModuleVersionInfo(nullptr, nullptr);
    if (!fixedFileInfo) {
        return false;
    }

    WORD major = HIWORD(fixedFileInfo->dwFileVersionMS);
    WORD minor = LOWORD(fixedFileInfo->dwFileVersionMS);
    WORD build = HIWORD(fixedFileInfo->dwFileVersionLS);
    WORD qfe = LOWORD(fixedFileInfo->dwFileVersionLS);

    Wh_Log(L"Version: %u.%u.%u.%u", major, minor, build, qfe);

    return major == 10 && build >= 22000;
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original;
HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                   HANDLE hFile,
                                   DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module) {
        HandleLoadedModuleIfTaskbarView(module, lpLibFileName);
    }

    return module;
}

bool HookTaskbarSymbols() {
    HMODULE module =
        LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        Wh_Log(L"Couldn't load taskbar.dll");
        return false;
    }

    // Taskbar.dll
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(const TrayUI::`vftable'{for `IInspectable'})"},
            &TrayUI_vftable_IInspectable,
        },
        {
            {LR"(const TrayUI::`vftable'{for `ITrayComponentHost'})"},
            &TrayUI_vftable_ITrayComponentHost,
        },
        {
            {LR"(const CSecondaryTray::`vftable'{for `ISecondaryTray'})"},
            &CSecondaryTray_vftable_ISecondaryTray,
        },
        {
            {LR"(public: virtual struct HMONITOR__ * __cdecl TrayUI::GetStuckMonitor(void))"},
            &TrayUI_GetStuckMonitor_Original,
        },
        {
            {LR"(public: virtual struct HMONITOR__ * __cdecl CSecondaryTray::GetMonitor(void))"},
            &CSecondaryTray_GetMonitor_Original,
        },
        {
            {LR"(public: virtual bool __cdecl TrayUI::GetStuckRectForMonitor(struct HMONITOR__ *,struct tagRECT *))"},
            &TrayUI_GetStuckRectForMonitor_Original,
        },
        {
            {LR"(public: void __cdecl TrayUI::_Hide(void))"},
            &TrayUI__Hide_Original,
            TrayUI__Hide_Hook,
        },
        {
            {LR"(private: void __cdecl CSecondaryTray::_AutoHide(bool))"},
            &CSecondaryTray__AutoHide_Original,
            CSecondaryTray__AutoHide_Hook,
        },
        {
            {LR"(public: virtual void __cdecl TrayUI::Unhide(enum TrayCommon::TrayUnhideFlags,enum TrayCommon::UnhideRequest))"},
            &TrayUI_Unhide_Original,
        },
        {
            {LR"(private: void __cdecl CSecondaryTray::_Unhide(enum TrayCommon::TrayUnhideFlags,enum TrayCommon::UnhideRequest))"},
            &CSecondaryTray__Unhide_Original,
        },
        {
            {LR"(public: virtual __int64 __cdecl TrayUI::WndProc(struct HWND__ *,unsigned int,unsigned __int64,__int64,bool *))"},
            &TrayUI_WndProc_Original,
            TrayUI_WndProc_Hook,
        },
        {
            {LR"(private: virtual __int64 __cdecl CSecondaryTray::v_WndProc(struct HWND__ *,unsigned int,unsigned __int64,__int64))"},
            &CSecondaryTray_v_WndProc_Original,
            CSecondaryTray_v_WndProc_Hook,
        },
    };

    return HookSymbols(module, symbolHooks, ARRAYSIZE(symbolHooks));
}

// Explorer.exe
bool HookExplorerSymbols() {
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(protected: __int64 __cdecl CTray::_OnAppBarMessage(struct tagCOPYDATASTRUCT *))"},
            &CTray__OnAppBarMessage_Original,
            CTray__OnAppBarMessage_Hook,
        },
        {
            {LR"(public: virtual unsigned int __cdecl CDeskTray::AppBarGetState(void))"},
            &CDeskTray_AppBarGetState_Original,
            CDeskTray_AppBarGetState_Hook,
        },
    };

    return HookSymbols(GetModuleHandle(nullptr), symbolHooks,
                       ARRAYSIZE(symbolHooks));
}

// Explorer.exe, to keep the space of a visible taskbar out of the work area.
bool HookWorkAreaSymbols() {
    WindhawkUtils::SYMBOL_HOOK symbolHooks[] = {
        {
            {LR"(private: int __cdecl CTray::RecomputeWorkArea(struct HMONITOR__ *,struct tagRECT *))"},
            &CTray_RecomputeWorkArea_Original,
            CTray_RecomputeWorkArea_Hook,
        },
        {
            {LR"(public: virtual void __cdecl CTray::WorkAreaMayHaveChanged(struct HMONITOR__ *,struct HWND__ *))"},
            &CTray_WorkAreaMayHaveChanged_Original,
        },
        // The CTray is also learned from the calls of explorer, so the global
        // is only a fallback for the first update. The vftable is needed to
        // find the interface of the CTray.
        {
            {LR"(struct CTrayStatic c_trayStatic)"},
            &g_cTrayStatic,
            nullptr,
            true,
        },
        {
            {LR"(const CTray::`vftable'{for `ITrayUIHost'})"},
            &g_trayUIHostVftable,
        },
    };

    return HookSymbols(GetModuleHandle(nullptr), symbolHooks,
                       ARRAYSIZE(symbolHooks));
}

void LoadSettings() {
    PCWSTR monitors = Wh_GetStringSetting(L"monitors");
    g_settings.monitors = Monitors::all;
    if (wcscmp(monitors, L"primary") == 0) {
        g_settings.monitors = Monitors::primary;
    } else if (wcscmp(monitors, L"secondary") == 0) {
        g_settings.monitors = Monitors::secondary;
    }
    Wh_FreeStringSetting(monitors);

    g_settings.maximizeToEdge = Wh_GetIntSetting(L"maximizeToEdge");
    g_settings.reserveSpace = Wh_GetIntSetting(L"reserveSpace");

    g_settings.excludedPrograms.clear();

    for (int i = 0;; i++) {
        PCWSTR program = Wh_GetStringSetting(L"excludedPrograms[%d]", i);

        bool hasProgram = *program;
        if (hasProgram) {
            std::wstring programUpper = program;
            LCMapStringEx(
                LOCALE_NAME_USER_DEFAULT, LCMAP_UPPERCASE, &programUpper[0],
                static_cast<int>(programUpper.length()), &programUpper[0],
                static_cast<int>(programUpper.length()), nullptr, nullptr, 0);

            g_settings.excludedPrograms.insert(std::move(programUpper));
        }

        Wh_FreeStringSetting(program);

        if (!hasProgram) {
            break;
        }
    }
}

BOOL Wh_ModInit() {
    Wh_Log(L">");

    LoadSettings();

    if (!IsWindows11()) {
        Wh_Log(L"Unsupported Windows version, Windows 11 is required");
        return FALSE;
    }

    HMODULE hUser32Module =
        LoadLibraryEx(L"user32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (hUser32Module) {
        pGetWindowBand =
            (GetWindowBand_t)GetProcAddress(hUser32Module, "GetWindowBand");

        WindhawkUtils::SetFunctionHook(
            (SetPropW_t)GetProcAddress(hUser32Module, "SetPropW"),
            SetPropW_Hook, &SetPropW_Original);
    }

    if (HMODULE taskbarViewModule = GetTaskbarViewModuleHandle()) {
        g_taskbarViewDllLoaded = true;
        if (!HookTaskbarViewDllSymbols(taskbarViewModule)) {
            return FALSE;
        }
    } else {
        Wh_Log(L"Taskbar view module not loaded yet");
    }

    if (!HookTaskbarSymbols()) {
        return FALSE;
    }

    // Only needed to remove the gap of Chromium-based apps.
    if (!HookExplorerSymbols()) {
        Wh_Log(L"HookExplorerSymbols failed, applications may leave a gap");
    }

    // Only needed to keep the space of a visible taskbar.
    g_workAreaSupported = HookWorkAreaSymbols();
    if (g_workAreaSupported) {
        StartBarThread();
    } else {
        Wh_Log(L"HookWorkAreaSymbols failed, maximized windows may go under a "
               L"visible taskbar");
    }

    HMODULE kernelBaseModule = GetModuleHandle(L"kernelbase.dll");
    auto pKernelBaseLoadLibraryExW = (decltype(&LoadLibraryExW))GetProcAddress(
        kernelBaseModule, "LoadLibraryExW");
    WindhawkUtils::SetFunctionHook(pKernelBaseLoadLibraryExW,
                                   LoadLibraryExW_Hook,
                                   &LoadLibraryExW_Original);

    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L">");

    if (!g_taskbarViewDllLoaded) {
        if (HMODULE taskbarViewModule = GetTaskbarViewModuleHandle()) {
            if (!g_taskbarViewDllLoaded.exchange(true)) {
                Wh_Log(L"Got Taskbar.View.dll");

                if (HookTaskbarViewDllSymbols(taskbarViewModule)) {
                    Wh_ApplyHookOperations();
                }
            }
        }
    }

    WNDCLASS wndclass;
    if (GetClassInfo(GetModuleHandle(nullptr), L"Shell_TrayWnd", &wndclass)) {
        AdjustAllTaskbars();
    }
}

void Wh_ModUninit() {
    Wh_Log(L">");

    if (g_winEventHookThread) {
        PostThreadMessage(GetThreadId(g_winEventHookThread), WM_APP, 0, 0);
        WaitForSingleObject(g_winEventHookThread, INFINITE);
        CloseHandle(g_winEventHookThread);
        g_winEventHookThread = nullptr;
    }

    g_unloading = true;

    StopBarThread();

    if (HWND hMainTaskbarWnd = FindCurrentProcessTaskbarWnd()) {
        UpdateAppBarProps(hMainTaskbarWnd, /*restoreAll=*/true);

        // Give back the space of the taskbars, from the thread of the taskbar.
        DWORD_PTR result;
        SendMessageTimeout(hMainTaskbarWnd, g_releaseWorkAreaRegisteredMsg, 0,
                           0, SMTO_ABORTIFHUNG, 2000, &result);
    }

    if (g_wasAutoHideDisabled) {
        HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
        if (hTaskbarWnd) {
            SendMessage(hTaskbarWnd, kHandleTrayPrivateSettingMessage,
                        kTrayPrivateSettingAutoHideSet, FALSE);
        }
    }
}

BOOL Wh_ModSettingsChanged() {
    Wh_Log(L">");

    LoadSettings();
    AdjustAllTaskbars();

    return TRUE;
}
