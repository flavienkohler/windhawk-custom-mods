# windhawk-custom-mods

Personal [Windhawk](https://windhawk.net) mods, written to fix or replace behaviours that did not work as expected on my setup (Windows 11, two monitors).

## Mods

| Mod | Description |
| --- | --- |
| [auto-hide-taskbar](auto-hide-taskbar) | Hides each taskbar only when a window on its own monitor is maximized or overlaps it. |

## Install a mod

1. Install [Windhawk](https://windhawk.net).
2. In Windhawk, choose *Create a new mod*, replace the whole editor content with the content of the mod's `mod.wh.cpp`, and click *Compile Mod*.
3. Enable the mod, and change its settings from the *Settings* tab.

## Structure

Each mod lives in its own folder, with its code in `mod.wh.cpp` (the Windhawk source format), a `README.md` describing its behaviour and settings, and a `dev/` folder for design notes.

## Credits and license

Mods derived from someone else's work say so in their header and in their README. The repository is published under the GNU General Public License v3.0, see [LICENSE](LICENSE).

## AI assistance

This repository was developed with AI assistance: the code and documentation were written with Claude Sonnet 5.5 (Anthropic) under my direction, and tested by me on my own setup.
