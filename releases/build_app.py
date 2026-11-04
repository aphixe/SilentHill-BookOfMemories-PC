#!/usr/bin/env python3
"""Build the single-exe setup program: build_app.py <version> [--no-payload]
  -> releases/out/SilentHillBoM-PC-Setup-<version>.exe and its .sha256

The exe is the app (releases/app) with a payload after it: this repository's tracked files, Python and the compiler. It holds no game
files, firmware or licences, and refuses to build if a tracked file looks like one. --no-payload: the app alone, for working on the window.

On Linux this makes SilentHillBoM-Linux-Setup-<version>, laid out as:
    [static stub_linux][app][libSDL3.so.0][payload][parts table][footer]
Run it from a repository with the Linux python/ and toolchain/ from Project Nativ's AppImage."""
import hashlib, os, struct, subprocess, sys, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import build_release  # noqa: E402

APP = os.path.join(HERE, "app")
OUT = os.path.join(HERE, "out")
OBJ = os.path.join(OUT, "app_obj")
TC = os.path.join(ROOT, "toolchain")
BIN = os.path.join(TC, "llvm-mingw", "bin")
CXX = os.path.join(BIN, "x86_64-w64-mingw32-clang++.exe")
LINUX = os.name != "nt"
IM = os.path.join(ROOT, "third_party", "imgui")

# what building for x86-64 Windows never touches: the compiler's other targets and its own Python
SKIP = ("toolchain/llvm-mingw/aarch64-w64-mingw32/", "toolchain/llvm-mingw/armv7-w64-mingw32/", "toolchain/llvm-mingw/i686-w64-mingw32/",
        "toolchain/llvm-mingw/python/", "toolchain/llvm-mingw/share/", "releases/out/")


def icon_png(size):
    """the release's own icon, nothing from the game: a closed red book on black"""
    def px(x, y):
        u, v = (x + .5) / size, (y + .5) / size
        if not (.05 < u < .95 and .05 < v < .95): return 0, 0, 0, 0
        if .24 < u < .78 and .14 < v < .86:
            if u < .32: return 150, 8, 8, 255
            if .40 < u < .70 and .26 < v < .32: return 18, 18, 20, 255
            return 232, 16, 16, 255
        return 18, 18, 20, 255
    raw = b"".join(b"\0" + b"".join(bytes(px(x, y)) for x in range(size)) for y in range(size))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def write_resources():
    pngs = [icon_png(s) for s in (16, 32, 48, 256)]
    off = 6 + 16 * len(pngs)
    head = struct.pack("<HHH", 0, 1, len(pngs))
    for s, b in zip((16, 32, 48, 256), pngs):
        head += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(b), off)
        off += len(b)
    ico = os.path.join(OBJ, "app.ico")
    with open(ico, "wb") as f: f.write(head + b"".join(pngs))
    rc = os.path.join(OBJ, "app.rc")
    with open(rc, "w") as f:
        f.write('LANGUAGE 0, 0\n1 ICON "%s"\n1 24 "%s"\n' % (ico.replace("\\", "/"), os.path.join(APP, "app.manifest").replace("\\", "/")))
    res = os.path.join(OBJ, "app_res.o")
    subprocess.run([os.path.join(BIN, "x86_64-w64-mingw32-windres.exe"), rc, "-O", "coff", "-o", res], check=True)
    return res


def build_program(version, out):
    os.makedirs(OBJ, exist_ok=True)
    env = dict(os.environ, PATH=BIN + os.pathsep + os.environ["PATH"])
    flags = ["-O2", "-std=c++17", "-w", "-I", IM, "-I", os.path.join(IM, "backends"), "-I", os.path.join(ROOT, "third_party", "stb"), "-I", APP,
             "-I", os.path.join(TC, "zlib", "include"), "-DAPP_VERSION=\"%s\"" % version, "-DUNICODE", "-D_UNICODE", "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00", "-municode"]
    objs = [write_resources()]
    for s in [os.path.join(IM, n) for n in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp")] + \
             [os.path.join(IM, "backends", n) for n in ("imgui_impl_win32.cpp", "imgui_impl_dx11.cpp")]:
        o = os.path.join(OBJ, os.path.basename(s) + ".o")
        objs.append(o)
        if not os.path.isfile(o) or os.path.getmtime(s) > os.path.getmtime(o):
            print("  c++", os.path.basename(s))
            subprocess.run([CXX, "-c"] + flags + [s, "-o", o], check=True, env=env)
    for n in ("app.cpp", "platform_win32.cpp"):
        s = os.path.join(APP, n)
        o = os.path.join(OBJ, n + ".o")
        objs.append(o)
        print("  c++", n)
        subprocess.run([CXX, "-c"] + flags + [s, "-o", o], check=True, env=env)
    libs = [os.path.join(TC, "zlib", "lib", "libz.a"), "-ld3d11", "-ldxgi", "-ld3dcompiler", "-ldwmapi", "-lole32", "-lshell32", "-luuid", "-luser32", "-lgdi32", "-limm32"]
    subprocess.run([CXX, "-municode", "-mwindows", "-static", "-s", "-o", out] + objs + libs, check=True, env=env)


def build_linux_parts(version):
    """the stub, and the app built against the toolchain's SDL; the app looks for SDL beside itself, then in Game/"""
    os.makedirs(OBJ, exist_ok=True)
    cxx, sdl = os.path.join(TC, "bin", "c++"), os.path.join(TC, "SDL3")
    flags = ["-O2", "-std=c++17", "-w", "-I", IM, "-I", os.path.join(IM, "backends"), "-I", os.path.join(ROOT, "third_party", "stb"), "-I", APP,
             "-I", os.path.join(TC, "zlib", "include"), "-I", os.path.join(sdl, "include"), "-DAPP_VERSION=\"%s\"" % version]
    objs = []
    for s in [os.path.join(IM, n) for n in ("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp")] + \
             [os.path.join(IM, "backends", n) for n in ("imgui_impl_sdl3.cpp", "imgui_impl_sdlrenderer3.cpp")] + [os.path.join(APP, n) for n in ("app.cpp", "platform_linux.cpp")]:
        o = os.path.join(OBJ, os.path.basename(s) + ".o")
        objs.append(o)
        if s.startswith(APP) or not os.path.isfile(o) or os.path.getmtime(s) > os.path.getmtime(o):
            print("  c++", os.path.basename(s))
            subprocess.run([cxx, "-c"] + flags + [s, "-o", o], check=True)
    app = os.path.join(OBJ, "shbom-app")
    subprocess.run([cxx, "-s", "-o", app] + objs + [os.path.join(TC, "zlib", "lib", "libz.a"), "-L", os.path.join(sdl, "lib"), "-lSDL3", "-Wl,-rpath,$ORIGIN:$ORIGIN/Game", "-lpthread"], check=True)
    stub = os.path.join(OBJ, "stub")
    subprocess.run([os.path.join(TC, "zig", "zig"), "cc", "-target", "x86_64-linux-musl", "-static", "-Os", "-s", "-o", stub, os.path.join(APP, "stub_linux.c")], check=True)
    return stub, app, os.path.join(sdl, "lib", "libSDL3.so.0")


def payload_files():
    files = [f.decode() for f in subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], check=True, capture_output=True).stdout.split(b"\0") if f]
    for top in ("python", "toolchain"):
        for d, dirs, names in os.walk(os.path.join(ROOT, top)):
            if os.path.basename(d) == "__pycache__": continue
            files += [os.path.relpath(os.path.join(d, n), ROOT).replace("\\", "/") for n in names + [x for x in dirs if os.path.islink(os.path.join(d, x))]]
    return sorted(f for f in set(files) if not f.startswith(SKIP) and os.path.lexists(os.path.join(ROOT, f)))


def append_payload(path, version, parts=b""):
    bad = build_release.check()
    if bad: sys.exit("Refusing to build: these tracked files look like game data:\n  " + "\n  ".join(bad))
    files = payload_files()
    with open(path, "ab") as out:
        data_off = out.tell()
        index = bytearray()
        total = 0
        for i, f in enumerate(files):
            # (version.txt: what the tools report, since a player has no git)
            full, mode = os.path.join(ROOT, f), 0
            if os.path.islink(full): raw, mode = os.readlink(full).encode(), 2
            else:
                with open(full, "rb") as src: raw = version.encode() + b"\n" if f == "version.txt" else src.read()
                if LINUX and os.access(full, os.X_OK): mode = 1
            co = zlib.compressobj(9, zlib.DEFLATED, -15)
            c = co.compress(raw) + co.flush()
            name = f.encode("utf-8")
            index += struct.pack("<H", len(name)) + name + struct.pack("<QQQB", out.tell(), len(c), len(raw), mode)
            out.write(c)
            total += len(raw)
            if i % 2000 == 0: print(f"  {i}/{len(files)} files")
        co = zlib.compressobj(9, zlib.DEFLATED, -15)
        ci = co.compress(bytes(index)) + co.flush()
        index_off = out.tell()
        out.write(ci)
        out.write(parts)
        out.write(b"SHBMPAK1" + struct.pack("<QQQQ", data_off, index_off, len(ci), len(index)))
    print(f"  payload: {len(files)} files, {total / 1048576:.0f} MB unpacked")


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else "dev"
    out = os.path.join(OUT, f"SilentHillBoM-Linux-Setup-{version}" if LINUX else f"SilentHillBoM-PC-Setup-{version}.exe")
    os.makedirs(OUT, exist_ok=True)
    if LINUX:
        stub, app, sdl = build_linux_parts(version)
        if "--no-payload" in sys.argv: return
        with open(out, "wb") as f:
            table = b"SHBMPART"
            for part in (stub, app, sdl):
                off = f.tell()
                with open(part, "rb") as src: f.write(src.read())
                if part != stub: table += struct.pack("<QQ", off, f.tell() - off)
        os.chmod(out, 0o755)
        print(f"  program: {os.path.getsize(out) / 1048576:.1f} MB")
        append_payload(out, version, table)
    else:
        build_program(version, out)
        print(f"  program: {os.path.getsize(out) / 1048576:.1f} MB")
        if "--no-payload" not in sys.argv: append_payload(out, version)
    h = hashlib.sha256()
    with open(out, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""): h.update(b)
    with open(out + ".sha256", "w", newline="\n") as f: f.write(f"{h.hexdigest()}  {os.path.basename(out)}\n")
    print(f"{out}\n  {os.path.getsize(out) / 1048576:.0f} MB  sha256 {h.hexdigest()}")


if __name__ == "__main__":
    main()
