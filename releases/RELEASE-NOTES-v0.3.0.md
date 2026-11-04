This release adds a Linux and Steam Deck build, with the same setup program and portable folder as Windows.

**No game files are included.** You need your own copy of the game (US, `PCSE00011`, version 1.00) and Sony's Vita firmware.

## Downloads

- **Windows:** `SilentHillBoM-PC-Setup-v0.3.0.exe`
- **Steam Deck and Linux:** `SilentHillBoM-Linux-Setup-v0.3.0`

Each file holds everything its setup needs, so nothing else is downloaded.

## Install on Windows

Run the exe, choose an install folder, your firmware and your game folder, then press Install. Windows may warn you, because the program isn't signed. The build takes about 8 minutes.

## Install on a Steam Deck

1. Switch to Desktop Mode, and copy your firmware file and game folder onto the Deck.
2. Right-click the downloaded file, open **Properties > Permissions**, and turn on **Is executable**.
3. Run it and press Install. Keep the Deck plugged in, because the build uses all its CPU cores.
4. Press **Add to Steam** in the launcher to play from Gaming Mode.

## What's new

- Linux and Steam Deck support, with controller navigation in the launcher
- An **Add to Steam** button, and a `--play` option that goes straight into the game on Linux
- Saves work on both systems, so one Syncthing folder can keep a PC and a Deck in step

## Please test on a Deck

The Linux build has been installed and run only in a test setup on a Windows PC, with a software GPU. It has not yet run on a real Steam Deck, in Gaming Mode, or with a physical controller. Reports are welcome.

## Known problems

This is an early port. Expect bugs and possible crashes, and keep a copy of your saves.

- Very bright surfaces can show small teal flecks
- Small dark patches may appear in some blood decals and shadows
- Text entry through the Vita's on-screen keyboard is not implemented
- Multiplayer is not implemented
