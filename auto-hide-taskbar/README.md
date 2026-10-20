# Auto-Hide Taskbar (per monitor)

A Windhawk mod for Windows 11. Each taskbar stays visible, and slides away only when a window **on the same monitor** is maximized, fullscreen, or overlaps the taskbar area. The monitors are fully independent: a maximized window on one screen never touches the taskbar of the other. Hovering the screen edge reveals a hidden taskbar as usual.

## Behaviour

For each taskbar, the windows of the desktop are checked. The taskbar hides if at least one of them:

- is visible, not minimized, not cloaked, and not a tool window;
- is not from an excluded program (a visible window of an excluded program keeps the taskbar shown, see below);
- is maximized on this monitor, or its visible frame overlaps the taskbar rectangle of this monitor (which covers fullscreen and snapped windows).

The taskbar is also kept shown during Alt+Tab and Win+Tab.

The state is re-evaluated when a window is created, shown, hidden, moved, resized, cloaked or uncloaked.

## Settings

| Setting | Values | Default | Effect |
| --- | --- | --- | --- |
| Monitors | all, primary, secondary | all | Monitors whose taskbar is managed by the mod. The taskbar of the other monitors is always visible. |
| Maximize windows to the screen edge | on, off | on | Removes the gap that Chromium-based apps leave at the bottom of a maximized window, see below. |
| Keep the space of a visible taskbar | on, off | on | When the taskbar stays visible because of an excluded program or an unmanaged monitor, maximized windows stop above it instead of going under it, see below. |
| Excluded programs | process names, paths or application IDs | empty | Windows of these programs never hide the taskbar. |

## Space of a visible taskbar

The mod keeps the Windows auto-hide setting on, and Windows reserves no space for an auto-hide taskbar: a maximized window uses the whole screen, and goes under the taskbar when it is shown. That is wanted while the taskbar only waits for a window to cover it, but not when it stays visible for a lasting reason, because the window would then be partly hidden by the taskbar.

When the taskbar of a monitor is kept visible because only excluded programs cover it, or because the monitor is not managed by the mod, its space is subtracted from the work area of the monitor and Windows resizes the maximized windows to stay above it. A window of an excluded program that can be seen wins: while one covers or touches the taskbar of a monitor, the taskbar stays visible and its space stays reserved, even if a window that is not excluded is maximized or snapped on the other part of the screen. An excluded window that is completely hidden behind a window that covers the taskbar is ignored. The space is given back when no excluded window needs the taskbar anymore. Nothing is reserved while the taskbar is merely waiting for a window, otherwise every maximized window would first be sized under the reduced area and then grow again.

The space starts at the top of the bar that is seen, not at the top of the taskbar window: a styled taskbar that floats with margins is smaller than its window, and the maximized windows would otherwise stop short of the bar, with a strip of wallpaper in between. The position of the bar is measured with UI Automation on a worker thread (UI Automation must not be called from the thread of the taskbar), and measured again every few seconds. If it cannot be measured, the whole taskbar window is kept.

A window that is just above the taskbar while the space is reserved (the window was resized to end there) still counts as next to the taskbar. Without that, a window snapped to a side of the screen would grow back under the taskbar as soon as the space is released, which would reserve it again, in an endless loop.

If the symbols needed are not found in the explorer of your Windows version, the mod logs it and works as without this setting.

## Gap of Chromium-based apps

Chromium-based apps (Chrome, Electron apps such as VSCodium or Discord, ...) stop 2 px short of the screen edge when a maximized window has an auto-hide taskbar there, so that the taskbar can still be reached. With this mod the taskbar is only auto-hidden while a window covers it, and that gap shows as a thin line at the bottom of the window.

When *Maximize windows to the screen edge* is on, applications are told that there is no auto-hide taskbar on the managed monitors, so they maximize to the true edge. The taskbar is still revealed by moving the pointer to the edge. They find out in two ways, both answered by the mod:

- `ABM_GETAUTOHIDEBAR` and `ABM_GETAUTOHIDEBAREX` are answered by `shell32` from the `WindowOnEdge:<monitor>:<edge>` properties of the main taskbar window. The mod rewrites their value to "none".
- Otherwise they check the `ABS_AUTOHIDE` flag of `ABM_GETSTATE`, which explorer answers. The mod clears the flag in the answers to applications, not in explorer's own calls.

Limitations: it applies to windows maximized after the mod is enabled or the setting changed, and `ABM_GETSTATE` is not per monitor, it describes the main taskbar. If revealing the taskbar by moving the pointer to the edge misbehaves, turn the setting off.

## How it works

The mod turns on the Windows auto-hide setting, then overrides the taskbar's own hide logic: a managed taskbar that no window covers is kept shown, and released (so Windows hides it) when a window covers it. It hooks:

- the private `TrayUI` and `CSecondaryTray` classes of `taskbar.dll`, and the `ViewCoordinator` of `Taskbar.View.dll`, for the hide logic;
- `SetPropW`, for the `WindowOnEdge` properties;
- `CTray::_OnAppBarMessage` and `CDeskTray::AppBarGetState` of `explorer.exe`, for the appbar state;
- `CTray::RecomputeWorkArea` and `CTray::WorkAreaMayHaveChanged` of `explorer.exe`, for the space of a visible taskbar, with UI Automation to find the visible bar.

Because of these private hooks it only works on Windows 11, and may need an update after a Windows update that changes these internals. It was developed and tested on Windows 11 24H2 (build 26100) with two monitors.

## Debugging

Windhawk's log view shows, for each decision, the monitor's taskbar rectangle and the window that caused the taskbar to hide, and the appbar properties that are hidden from applications.

## Credits

This is a modified version of [Taskbar auto-hide when maximized](https://windhawk.net/mods/taskbar-auto-hide-when-maximized) 1.2.6 by [m417z](https://github.com/m417z), published under GPL-3.0, like this mod. Most of the taskbar hooking, the Alt+Tab / Win+Tab detection and the program exclusion are his work. It was modified on 2026-10-20 after the original did not behave as expected on a two-monitor setup: the decision is taken for each monitor on its own, monitors can be left alone, and the gap of Chromium-based apps is removed.
