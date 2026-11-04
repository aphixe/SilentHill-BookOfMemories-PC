# Silent Hill: Book of Memories for PC

An unofficial native Windows port of the PS Vita game *Silent Hill: Book of Memories*, built with
[Project Nativ](https://github.com/TheLostPantheon/nativ). The game's code is recompiled into a Windows program on your
own machine, from your own copy of the game.

**This repository contains no game files, game code or firmware, and neither do its releases.** You need your own copy of
the game. Setup never downloads it for you.

## Status

This is an early port, tested on one Windows PC. The game runs from the menus into gameplay, with sound, saves, controller
support and keyboard and mouse controls. Expect bugs and possible crashes, and keep a copy of your saves.

| Works | Details |
|---|---|
| Graphics | Deferred lighting, UI and depth, drawn on your GPU with Vulkan |
| Resolution | Gameplay 3D renders at full 4K, not stretched from the Vita's resolution |
| Fullscreen | Alt+Enter uses your display's resolution and keeps the game's aspect ratio |
| Controls | Keyboard and mouse, and controllers |

Known problems:

- Very bright surfaces, like faces under strong light, can show small teal flecks.
- Small dark patches may appear in some blood decals and shadows.
- Text entry through the Vita's on-screen keyboard is not implemented.
- Multiplayer is not implemented.
- Long play sessions have had little testing.

## What you need

- 64-bit Windows 10 or 11 and a GPU with Vulkan drivers
- About 5 GB of free disk space
- **Your copy of the game**: the US release, title ID `PCSE00011`, version 1.00, as a folder copied from `ux0:app/PCSE00011`
  on your Vita. Its folder holds `eboot.bin`. Other regions and versions are refused, because the port's patches are made for
  this exact build (SHA-256 of its main program:
  `4693b338434d8f7fdd913002672f530a145eab1ecda6bd1eae68668465d3fc5c`).
- **Sony's Vita firmware**, either of:
  - the firmware update file (`PSP2UPDAT.PUP` or `PSVUPDAT.PUP`) from Sony's PS Vita system software update page
  - a [Vita3K](https://github.com/Vita3K/Vita3K) folder with the firmware installed

## Install

1. Download `SilentHillBoM-PC-<version>.zip` from the Releases page and unpack it somewhere with enough space. Or clone this
   repository.
2. Run `releases\Setup.bat`.
3. Choose your firmware and game folder when asked.
4. Wait for the build, which takes several minutes. Then use the new desktop shortcut.

Windows may warn about running a downloaded script, since it isn't signed. You can read
[`releases/setup.ps1`](releases/setup.ps1) first.

Setup does the following, all inside the folder you unpacked:

1. Downloads the Project Nativ v0.1.5 Windows release from that project's GitHub page and checks its SHA-256. It takes only
   Python, the compiler and SDL from it.
2. Decrypts your firmware and game into `dump\`.
3. Recompiles and builds the game.
4. Adds default controls and a desktop shortcut, with an icon made from your copy's artwork.

After updating to a newer version, run `releases\Setup.bat` again. It rebuilds the game and keeps your saves and settings.
Saves are in `dump\decrypted\PCSE00011\recomp\fs\savedata0`.

## Controls

| Vita | Keyboard and mouse |
|---|---|
| Left stick | W A S D |
| Cross | Space |
| Circle | E |
| Square | 1, or left mouse button |
| Triangle | 2, or right mouse button |
| L / R | Left Shift / G |
| Start / Select | Tab / F |
| D-pad | T V Q X (up, down, left, right) |
| Touch screen | Hold Left Alt and use the mouse |

In menus, the arrow keys move, Enter is Cross and Esc is Start.

| Key | Action |
|---|---|
| Alt+Enter | Toggle fullscreen |
| `-` and `=` (also keypad `-` and `+`) | Volume down and up. The level is remembered. |
| F10 | In-game menu: settings and key bindings |
| F1 | Performance display |
| F2 | Change render scale |

## Building from source

The commands behind setup can be run by hand from the repository's top folder, once setup has installed the tools:

```bash
python/python.exe pipeline/vitarecomp.py build PCSE00011
```

```bash
python/python.exe pipeline/vitarecomp.py run PCSE00011
```

To build a release zip from the last commit, which also refuses to run if a tracked file looks like game data:

```bash
python/python.exe releases/build_release.py v0.1.0
```

[`CHANGES.md`](CHANGES.md) lists what differs from Project Nativ. [`docs/NATIV-README.md`](docs/NATIV-README.md) is
Project Nativ's own README, and some of its links point to documents that are only in
[its repository](https://github.com/TheLostPantheon/nativ). The GLSL sources for the shader tables this port changed are in
[`dev/sh-bom/shaders`](dev/sh-bom/shaders).

The [`.gitignore`](.gitignore) keeps your game files, firmware, licences, generated code, builds and saves out of git, so a
clone can be set up and played in place. Please don't commit or share any of them.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Konami, Sony or the game's developers. *Silent Hill* is
a trademark of Konami. Use this only with a copy of the game you own.

This port is based on Project Nativ and is free software under the GNU General Public License, version 2 or (at your option)
any later version: see [`LICENSE`](LICENSE). Third-party code keeps its own licence: see
[`THIRD_PARTY.md`](THIRD_PARTY.md).

## Credits

- [Project Nativ](https://github.com/TheLostPantheon/nativ) for the recompiler, runtime and tools
- [Vita3K](https://github.com/Vita3K/Vita3K), used as a reference for Vita behaviour

Much of this port's code and documentation was written with AI assistance.
