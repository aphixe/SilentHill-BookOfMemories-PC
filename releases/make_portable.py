#!/usr/bin/env python3
"""Turn a finished build into the portable folder: make_portable.py <target folder>
Run by the app from its temporary build folder. Everything the game needs goes to <target>/Game, with no absolute path in it,
so the folder can be moved or copied. Firmware, licences and generated code are not kept. Saves and settings already there stay."""
import os, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "pipeline"))
os.environ.setdefault("PYTHONUTF8", "1")
import vitarecomp  # noqa: E402

TITLE = "PCSE00011"
target = os.path.abspath(sys.argv[1])
g = vitarecomp.Game(TITLE, "run")
game = os.path.join(target, "Game")
saves = os.path.join(target, "Saves")
os.makedirs(game, exist_ok=True)
os.makedirs(saves, exist_ok=True)

# the program and what it loads
sdl = "SDL3.dll" if os.name == "nt" else "libSDL3.so.0"
for f in [g.EXE, sdl, g.BASE + ".img"] + [m + ".img" for m in g.MODS]:
    shutil.copy2(os.path.join(g.OUT, f), os.path.join(game, f))

# the game's data: a real folder now, not a link to the library. (moved, not copied: a move on one drive takes no time)
fs_dir = os.path.join(game, "fs")
os.makedirs(fs_dir, exist_ok=True)
old = os.path.join(fs_dir, "app0")
if os.path.isdir(old): vitarecomp.remove_tree(old)
shutil.move(os.path.join(g.W, "app0"), old)

# controls and the like. Kept on update. Not in Saves: a PC and a Steam Deck want different settings.
settings = os.path.join(game, "settings.ini")
if not os.path.isfile(settings):
    shutil.copy2(os.path.join(HERE, "default-settings.ini"), settings)
    # (a Steam Deck plays on its own screen, and has no easy Alt+Enter)
    if os.environ.get("SteamDeck") == "1" or "steamos" in (open("/etc/os-release").read().lower() if os.path.isfile("/etc/os-release") else ""):
        with open(settings, "a", encoding="utf-8", newline="\n") as f: f.write("fullscreen = 1\n")

# how to start it, for the launcher: everything relative to Game
shipped = os.path.join(vitarecomp.PACKS, g.PACK, "shaders.txt")
env = [w.split("=", 1) for w in (g.RUN_ENV or "").split() if "=" in w]
if os.path.isfile(shipped):
    shutil.copy2(shipped, os.path.join(game, "shaders.txt"))
    env.append(["RECOMP_SHADER_RECIPES_SHIPPED", "shaders.txt"])
env += [["RECOMP_MODULES", ",".join(m + ".img" for m in g.MODS)], ["RECOMP_TITLE", g.TITLE_ID], ["RECOMP_GAME_NAME", g.NAME],
        ["RECOMP_LOG", "0"], ["RECOMP_HITCH_LOG", "50"], ["RECOMP_STALL_PROF", "150"],
        ["RECOMP_FS", "fs"], ["RECOMP_SAVEDATA", "../Saves"], ["RECOMP_SETTINGS", "settings.ini"], ["RECOMP_PLAIN_TITLE", g.NAME]]
with open(os.path.join(game, "launch.cfg"), "w", encoding="utf-8", newline="\n") as f:
    f.write(f"exe={g.EXE}\n")
    for a in (g.BASE + ".img", g.entry(), "100000000000"): f.write(f"arg={a}\n")
    for k, v in env: f.write(f"env={k}={v}\n")

# licences, and what the two folders are
for name in ("LICENSE", "THIRD_PARTY.md"):
    shutil.copy2(os.path.join(ROOT, name), os.path.join(target, name))
with open(os.path.join(saves, "ABOUT SAVES.txt"), "w", encoding="utf-8") as f:
    f.write("Your Silent Hill: Book of Memories saves are in this folder.\n\n"
            "To keep them in step between machines, point Syncthing or a similar tool at this folder.\n"
            "Close the game on one machine and let the sync finish before you play on another.\n\n"
            "The launcher's Export and Import buttons make and restore a zip of this folder.\n"
            "Import keeps a copy of what it replaces in the Save Backups folder beside this one.\n")

# the library held the firmware, the licence and the generated code: none of it stays
vitarecomp.remove_tree(vitarecomp.DUMP)
print("portable folder ready:", target)
