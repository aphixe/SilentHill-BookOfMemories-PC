# Changes from Project Nativ v0.1.5

This repository's first commit is the unmodified source of the Project Nativ v0.1.5 Windows release. Everything after it is this
port's work, so `git diff` from that commit shows every change. Started November 2026.

## Added

- `packs/PCSE00011/`: the game pack for Silent Hill: Book of Memories (US, 1.00), with its code hooks
- `releases/`: the setup and launcher program, default controls and the release builder
- `third_party/imgui/backends/`: Dear ImGui's Windows and Direct3D 11 backends, unmodified, for that program
- `dev/sh-bom/`: shader sources and the replay build script used for this port
- `.gitignore`, `.gitattributes`, this file, and a new `README.md`. Project Nativ's is kept as `docs/NATIV-README.md`.

## Recompiler (`recompiler/recomp.py`)

- Tail calls no longer overwrite the link register
- Added the NEON `vzip` and `vuzp` instructions

## Shaders (`runtime/gfx/usse.c`, `usse_internal.h`, `usse_spirv.c`)

- Corrected the repeat rule for internal registers, which fixed black scenes and lighting
- Corrected the VBW repeat count, and added subroutine calls and returns, which fixed invisible UI
- Added the SOP2, SOP2M and SOP3 instructions, dispatch-loop branches and texture-buffer sampling

## Graphics (`runtime/gfx/gfx_gpu.c`, `vk_shaders.h`, `display.c`)

- A G-buffer stored as F16F16F16F16 and read as U8U8U8U8 is now read at the render scale. Gameplay lighting used to read it at
  the Vita's resolution, which left the 3D blocky at high resolutions.
- Added `sceDisplayGetVcount`
- Added debug switches, which do nothing unless their `RECOMP_*` environment variables are set

## Window and input (`runtime/ui/window.c`, `overlay.cpp`)

- Mouse buttons can be bound to Vita buttons, and menus work with the arrow keys, Enter and Esc
- Alt+Enter fullscreen at the display's resolution
- Volume keys, with the level saved

## Kernel and files (`runtime/core/kernel.c`, `main.c`, `io.c`)

- Added missing thread functions (current priority, exit thread)
- Added `RECOMP_SAVEDATA`, which keeps saves in a folder of their own, and `RECOMP_PLAIN_TITLE` for the window title
- Added `RECOMP_IO_OPEN_DELAY_MS`, because the game's loading code expects slower storage than a PC's
- Added debug logging for aborts and hangs
