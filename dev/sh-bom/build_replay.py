#!/usr/bin/env python3
"""Windows equivalent of Project Nativ's runtime/debug/build_replay.sh: builds gfxreplay, the graphics layer alone,
for replaying RECOMP_CAPTURE files (.vfc).

    python -B build_replay.py [-f] [-j N]

Compiles the INSTALL's runtime sources as they are now (user edits included) with the install's llvm-mingw, using the
same flags as runtime/build.py on Windows, so a replay draws what the game's own build draws. Nothing is written under
ProjectNativ: objects, the exe and SDL3.dll all go to port/replay/. (Run Python with -B: bytecode for the install's
own python would otherwise be written under ProjectNativ.)

Debug sources the release leaves out (gfxreplay.c) are copied once from the upstream checkout into port/replay/ and
used from there, so that copy can be edited. Delete it to take upstream's again. A source the install has always wins.

Each source is compiled to its own object and reused while newer than the source and every header it included
(clang's -MD), so after editing usse_spirv.c only that file recompiles.

Environment: NATIV_ROOT (the install), NATIV_UPSTREAM (the checkout), RECOMP_CPU (native, as build.py; or generic),
RECOMP_CFLAGS (more flags; objects are kept per flag set).
"""
import argparse
import concurrent.futures
import hashlib
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("NATIV_ROOT") or os.path.join(os.path.dirname(HERE), "ProjectNativ")
UPSTREAM = os.environ.get("NATIV_UPSTREAM") or r"C:\Users\jason\AppData\Local\Temp\claude\895f268953e51da3\bde5fab7-dd9d-46f9-bb9e-9880200ad151\scratchpad\refs\nativ"
OUT_DIR = os.path.join(HERE, "replay")
RT = os.path.join(ROOT, "runtime")
TC = os.path.join(ROOT, "toolchain")
SDLW = os.path.join(TC, "SDL3", "x86_64-w64-mingw32")
CC = os.path.join(TC, "llvm-mingw", "bin", "x86_64-w64-mingw32-clang.exe")

# build_replay.sh's SRCS, with platform_win32.c + win32_compat.c for platform_posix.c (as build.py on Windows)
SOURCES = """
debug/gfxreplay.c platform/platform_win32.c platform/win32_compat.c
gfx/gfx_dispatch.c gfx/gfx_soft.c gfx/gfx_gpu.c gfx/gpulock.c
debug/gfx_coverage.c debug/gfx_capture.c debug/waitstats.c
gfx/guestwatch.c core/metrics.c core/settings.c gfx/usse.c gfx/usse_spirv.c
core/psarc.c gfx/gxp_index.c gfx/ssao.c gfx/fsr/fsr.c
""".split()
SUBDIRS = ("core", "gfx", "audio", "media", "input", "net", "ui", "platform", "debug")


def resolve(rel):
    """the source's path: the install's, else our copy in port/replay/, else copied there from upstream"""
    mine = os.path.join(RT, *rel.split("/"))
    if os.path.isfile(mine): return mine
    copy = os.path.join(OUT_DIR, os.path.basename(rel))
    if not os.path.isfile(copy):
        src = os.path.join(UPSTREAM, "runtime", *rel.split("/"))
        if not os.path.isfile(src): sys.exit(f"build_replay: {rel} is in neither the install nor {UPSTREAM}")
        shutil.copy2(src, copy)
        print(f"  copied {rel} from upstream to {copy}")
    return copy


# gfxreplay.c is written for POSIX and calls setenv, which mingw-w64 lacks; force-included for that one file, so the
# copy from upstream stays unedited
SHIM = """/* written by build_replay.py: the POSIX names gfxreplay.c uses that win32_compat.h doesn't give it */
#include <stdlib.h>
static inline int setenv(const char *k, const char *v, int overwrite) { return !overwrite && getenv(k) ? 0 : _putenv_s(k, v); }
"""


def write_shim():
    p = os.path.join(OUT_DIR, "replay_win_compat.h")
    if not os.path.isfile(p) or open(p).read() != SHIM:
        with open(p, "w") as f: f.write(SHIM)
    return p


def resolved_cpu(flag):
    """the processor -march=native means to this compiler: objects are kept per processor, as build.py keeps them"""
    err = subprocess.run([CC, flag, "-###", "-x", "c", "-c", os.devnull, "-o", os.devnull], capture_output=True, text=True).stderr
    m = re.search(r'"-target-cpu" "([\w.-]+)"', err)
    return m[1] if m else "native"


def depfile_inputs(d):
    """what a clang .d file lists: every file the object was built from"""
    try: text = open(d, encoding="utf-8", errors="replace").read()
    except OSError: return None
    text = text.split(":", 1)[-1] if not re.match(r"^[A-Za-z]:", text) else re.sub(r"^.*?\.o:\s", "", text, count=1, flags=re.S)
    text = text.replace("\\\r\n", " ").replace("\\\n", " ")
    return [p.replace("\\ ", " ") for p in re.findall(r"(?:\\ |\S)+", text)]


def stale(obj, dep, src):
    try: built = os.stat(obj).st_mtime_ns
    except OSError: return True
    inputs = depfile_inputs(dep)
    if not inputs: return True
    for p in [src] + inputs:
        try:
            if os.stat(p).st_mtime_ns > built: return True
        except OSError: return True   # a header moved or gone
    return False


def main():
    ap = argparse.ArgumentParser(description="Build gfxreplay.exe for Windows into port/replay/")
    ap.add_argument("-f", "--force", action="store_true", help="recompile everything")
    ap.add_argument("-j", "--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 2), help="compiles at once")
    args = ap.parse_args()
    if not os.path.isfile(CC): sys.exit(f"build_replay: no compiler at {CC}")
    os.makedirs(OUT_DIR, exist_ok=True)
    srcs = [resolve(r) for r in SOURCES]
    shim = write_shim()

    cpu = os.environ.get("RECOMP_CPU") or "native"
    cpuflags = [] if cpu == "generic" else ["-march=" + cpu]
    extra = (os.environ.get("RECOMP_CFLAGS") or "").split()
    tag = "generic" if not cpuflags else (resolved_cpu(cpuflags[0]) if cpu == "native" else cpu)
    if extra: tag += "_" + hashlib.sha1(" ".join(extra).encode()).hexdigest()[:8]
    obj_dir = os.path.join(OUT_DIR, "obj_" + tag)
    os.makedirs(obj_dir, exist_ok=True)

    includes = [f for d in SUBDIRS for f in ("-I", os.path.join(RT, d))] + ["-I", os.path.join(ROOT, "recompiler"), "-I", ROOT]
    flags = ["-O2", "-ffp-contract=off"] + cpuflags + extra + ["-w"] + includes + [
        "-D_FILE_OFFSET_BITS=64", "-D_POSIX_THREAD_SAFE_FUNCTIONS",
        "-include", os.path.join(RT, "platform", "win32_compat.h"),
        "-I", os.path.join(TC, "zlib", "include"), "-I" + os.path.join(SDLW, "include")]
    libs = [os.path.join(SDLW, "lib", "libSDL3.dll.a"), "-L" + os.path.join(TC, "zlib", "lib"), "-lz", "-lpthread", "-lwinmm", "-lws2_32", "-static"]

    def obj_of(s):   # (flat: no two sources here share a name)
        return os.path.join(obj_dir, os.path.splitext(os.path.basename(s))[0] + ".o")

    def compile_one(s):
        o = obj_of(s); d = o[:-2] + ".d"
        own = ["-include", shim] if os.path.basename(s) == "gfxreplay.c" else []
        r = subprocess.run([CC, "-c"] + flags + own + ["-MD", "-MF", d, s, "-o", o], capture_output=True, text=True)
        print(f"  cc {os.path.basename(s)}" + (" FAILED" if r.returncode else ""), flush=True)
        if r.stderr: print(r.stderr, end="", flush=True)
        return None if r.returncode == 0 else s

    work = [s for s in srcs if args.force or stale(obj_of(s), obj_of(s)[:-2] + ".d", s)]
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        failed = [f for f in ex.map(compile_one, work) if f]
    if failed: sys.exit("build_replay: " + ", ".join(os.path.basename(f) for f in failed) + " did not compile")

    out = os.path.join(OUT_DIR, "gfxreplay.exe")
    objs = [obj_of(s) for s in srcs]
    if work or not os.path.isfile(out) or any(os.stat(o).st_mtime_ns > os.stat(out).st_mtime_ns for o in objs):
        r = subprocess.run([CC] + objs + libs + ["-o", out])
        if r.returncode: sys.exit("build_replay: link failed")
        print(f"built {out}")
    else:
        print(f"up to date: {out}")
    # SDL3.dll beside the exe, replaced whole (a running replay keeps the file it loaded)
    dll = os.path.join(OUT_DIR, "SDL3.dll"); src = os.path.join(SDLW, "bin", "SDL3.dll")
    if not os.path.isfile(dll) or os.stat(src).st_mtime_ns > os.stat(dll).st_mtime_ns: shutil.copy2(src, dll)
    return 0


if __name__ == "__main__":
    sys.exit(main())
