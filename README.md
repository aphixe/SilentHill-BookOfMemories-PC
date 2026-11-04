# Silent Hill: Book of Memories for PC

An unofficial native Windows port of the PS Vita game *Silent Hill: Book of Memories*, built with
[Project Nativ](https://github.com/TheLostPantheon/nativ). The game's code is recompiled into a Windows program on your
own machine, from your own copy of the game.

**This repository contains no game files, game code or firmware, and neither do its releases.** You need your own copy of
the game. Setup never downloads it for you. A Steam Deck and Linux build is planned.

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
- About 5 GB of free disk space during setup, and about 1.6 GB after
- **Your copy of the game**: the US release, title ID `PCSE00011`, version 1.00, as a folder copied from `ux0:app/PCSE00011`
  on your Vita. Its folder holds `eboot.bin`. Other regions and versions are refused, because the port's patches are made for
  this exact build (SHA-256 of its main program:
  `4693b338434d8f7fdd913002672f530a145eab1ecda6bd1eae68668465d3fc5c`).
- **Sony's Vita firmware**, either of:
  - the firmware update file (`PSP2UPDAT.PUP` or `PSVUPDAT.PUP`) from Sony's PS Vita system software update page
  - a [Vita3K](https://github.com/Vita3K/Vita3K) folder with the firmware installed

## Install

1. Download `SilentHillBoM-PC-Setup-<version>.exe` from the Releases page. It holds everything setup needs, so nothing else
   is downloaded.
2. Run it. Windows may warn you, because the program isn't signed.
3. Choose an install folder, your firmware and your game folder, then press **Install**. The build takes about 8 minutes on
   a modern PC and uses all its CPU cores.
4. Press **PLAY**.

When setup finishes, you can delete the download. Setup builds the game on your PC from your own files, then removes the
tools, the firmware and everything else it no longer needs.

## The portable folder

```text
Silent Hill Book of Memories/
    Silent Hill Book of Memories.exe    launcher
    Game/                               the game and its settings
    Saves/                              your saves, and nothing else
    Save Backups/                       what an import replaced
```

The folder is about 1.6 GB, and all its paths are relative. Move it, rename it or copy it to another PC, and it keeps working.
A desktop shortcut is optional, and the launcher updates it after a move.

### Saves

- **Syncing:** point [Syncthing](https://syncthing.net) or a similar tool at the `Saves` folder. Close the game and let the
  sync finish before you play on another machine. Settings stay in `Game`, so each machine keeps its own.
- **Export** writes your saves to a zip file.
- **Import** takes a zip file or a folder. Your current saves are copied to `Save Backups` first.

To update, run a newer setup program and choose the same folder. You will need your firmware and game files again. Your saves
and settings are kept.

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

The build tools aren't in this repository. Unpack `python/`, `toolchain/`, `SDL3.dll` and `Project Nativ.exe` from the
[Project Nativ v0.1.5 Windows release](https://github.com/TheLostPantheon/nativ/releases/tag/v0.1.5) into the repository's
top folder. Then build the setup program, which refuses to run if a tracked file looks like game data:

```bash
python/python.exe releases/build_app.py v0.2.0
```

The result is in `releases/out`. The program's source is in [`releases/app`](releases/app), and
[`releases/make_portable.py`](releases/make_portable.py) lays out the portable folder.

To work on the game itself, add your files and run it in place:

```bash
python/python.exe pipeline/vitarecomp.py firmware <firmware file>
```

```bash
python/python.exe pipeline/vitarecomp.py add <game folder>
```

```bash
python/python.exe pipeline/vitarecomp.py run PCSE00011
```

[`CHANGES.md`](CHANGES.md) lists what differs from Project Nativ. [`docs/NATIV-README.md`](docs/NATIV-README.md) is
Project Nativ's own README, and some of its links point to documents that are only in
[its repository](https://github.com/TheLostPantheon/nativ). The GLSL sources for the shader tables this port changed are in
[`dev/sh-bom/shaders`](dev/sh-bom/shaders).

The [`.gitignore`](.gitignore) keeps your game files, firmware, licences, generated code, builds and saves out of git. Please
don't commit or share any of them.

## Legal

This is an unofficial fan project, not affiliated with or endorsed by Konami, Sony or the game's developers. *Silent Hill* is
a trademark of Konami. Use this only with a copy of the game you own.

This port is based on Project Nativ and is free software under the GNU General Public License, version 2 or (at your option)
any later version: see [`LICENSE`](LICENSE). Third-party code keeps its own licence: see
[`THIRD_PARTY.md`](THIRD_PARTY.md). The setup program also carries Python and the LLVM-MinGW compiler, with their licences
inside it.

## Credits

- [Project Nativ](https://github.com/TheLostPantheon/nativ) for the recompiler, runtime and tools
- [Vita3K](https://github.com/Vita3K/Vita3K), used as a reference for Vita behaviour

Much of this port's code and documentation was written with AI assistance.
