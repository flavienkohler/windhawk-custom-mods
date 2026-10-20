# Auto-Hide Taskbar (per monitor)

A Windhawk mod for Windows 11. Each taskbar stays visible, and slides away only when a window **on the same monitor** is maximized, fullscreen, or overlaps the taskbar area. The monitors are fully independent: a maximized window on one screen never touches the taskbar of the other. Hovering the screen edge reveals a hidden taskbar as usual.

## Behaviour

For each taskbar, the windows of the desktop are checked. The taskbar hides if at least one of them:

- is visible, not minimized, not cloaked, and not a tool window;
- is not from an excluded program;
- is maximized on this monitor, or its visible frame overlaps the taskbar rectangle of this monitor (which covers fullscreen and snapped windows).

The taskbar is also kept shown during Alt+Tab and Win+Tab.

The state is re-evaluated when a window is created, shown, hidden, moved, resized, cloaked or uncloaked.

## Settings

| Setting | Values | Default | Effect |
| --- | --- | --- | --- |
| Monitors | all, primary, secondary | all | Monitors whose taskbar is managed by the mod. The taskbar of the other monitors is always visible. |
| Maximize windows to the screen edge | on, off | on | Removes the gap that Chromium-based apps leave at the bottom of a maximized window, see below. |
| Excluded programs | process names, paths or application IDs | empty | Windows of these programs never hide the taskbar. |

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
- `CTray::_OnAppBarMessage` and `CDeskTray::AppBarGetState` of `explorer.exe`, for the appbar state.

Because of these private hooks it only works on Windows 11, and may need an update after a Windows update that changes these internals. It was developed and tested on Windows 11 24H2 (build 26100) with two monitors.

## Debugging

Windhawk's log view shows, for each decision, the monitor's taskbar rectangle and the window that caused the taskbar to hide, and the appbar properties that are hidden from applications.

## Credits

Derived from [Taskbar auto-hide when maximized](https://windhawk.net/mods/taskbar-auto-hide-when-maximized) by [m417z](https://github.com/m417z), published under GPL-3.0. The taskbar hooking, the Alt+Tab / Win+Tab detection and the program exclusion come from that mod. This version was created after the original did not behave as expected on a two-monitor setup, see [dev/issues.md](dev/issues.md).
