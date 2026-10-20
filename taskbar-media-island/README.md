# Taskbar Media Island

A Windhawk mod for Windows 11. A simple, minimalistic media control on the taskbar: the album art, the title and artist of the current track, and previous / play-pause / next buttons. It is a deliberately basic first version: it does what is needed to control the music without leaving a game or an application, nothing more. It shows the media session Windows considers current, so it works with any player that integrates with the system media controls (browsers, Spotify, music players, ...).

By default it appears on the taskbars of the **secondary** monitors only, which is useful next to a mod that already shows a media control on the main taskbar. It can also be shown on the main taskbar or on all of them.

## Behaviour

- It is shown while a media session exists, paused or playing, and hidden when there is none.
- It is hidden while the taskbar is hidden (auto-hide), and while a full-screen application or a presentation is running.
- It follows the taskbar when it moves, and stays right above it in the z-order.
- The buttons react to the pointer, a click on the island outside the buttons does nothing.
- It follows the light or dark theme of the system.

## Settings

| Setting | Values | Default | Effect |
| --- | --- | --- | --- |
| Monitors | secondary, primary, all | secondary | The taskbars on which the island is shown. |
| Position | left, right | left | Side of the taskbar on which the island is placed. |
| Style | pill, transparent | pill | A rounded background, or no background so that the controls blend into the taskbar. |
| Margin | pixels at 100% scaling | 12 | Distance between the island and the edge of the taskbar. On the right side, leave room for the clock. |
| Width | pixels at 100% scaling | 300 | Width of the island. |

## How it works

The island is a separate, borderless, layered window placed above the taskbar, not an element inside the taskbar. Its shapes and the album art are drawn with GDI+, and its text and buttons with DirectWrite, at the real pixels of the monitor. It therefore does not take part in the taskbar layout: choose a position and a margin that leave it clear of the taskbar buttons (with centered icons, the left side is free).

- A worker thread reads the current session with the Windows media control API (`GlobalSystemMediaTransportControlsSessionManager`) and reacts to its change events. It publishes what is quick to get first (is a session playing), and requests the title, artist and album art asynchronously, so that a slow player never delays the handling of the next event (a new track, the next button, ...). A newer request replaces an older one that is still in progress. The playback state is also polled, once per second and every 150 ms for a few seconds after a click, because the playback events of some players are missing or come before the state is updated. After a click on play/pause the expected state is shown at once and kept until the player reports it. Players often provide the album art after the title, so it is requested again on the next event until it is loaded. The Windhawk log shows how long each step took.
- A UI thread owns one island window per managed taskbar and a hook on the window events of the taskbar, to follow its position and to react when a taskbar appears or disappears (for example when a monitor is connected).
- A timer, once per second, checks the full-screen state and the theme, and creates or removes islands when the monitors change.

Because of the way it is drawn, it does not use any private Windows internals, and should survive Windows updates better than the mods that hook the taskbar. It does not copy the look of other media islands.

## Credits

Original code, published under GPL-3.0 like the rest of this repository. The existing taskbar media mods of the Windhawk catalogue were looked at to see what already exists, no code was taken from them.
