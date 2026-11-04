#!/usr/bin/env python3
"""Build the GitHub release zip from what is committed: build_release.py <tag> -> releases/out/SilentHillBoM-PC-<tag>.zip
Refuses to build if a tracked file looks like game, firmware or licence data."""
import fnmatch, hashlib, os, subprocess, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BANNED = ["eboot.bin", "eboot.bin.elf", "param.sfo", "work.bin", "icon0.png", "*.rif", "*.pod", "*.suprx", "*.skprx", "*.self", "*.elf", "*.pup", "*.pkg", "*.vpk",
          "game_part*.c", "mod_lib*", "*.img", "*.vfc", "*.ppm", "*.exe", "*.dll", "savedata*", "*.lnk", "silenthillbom.ico"]
LARGEST = 20 * 1024 * 1024

def git(*args): return subprocess.run(["git", "-C", ROOT, *args], check=True, capture_output=True).stdout

def check():
    bad = []
    for f in git("ls-files", "-z").decode().split("\0"):
        if not f: continue
        parts = f.split("/")
        if parts[0] in ("dump", "python", "toolchain"): bad.append(f)
        elif any(fnmatch.fnmatch(p.lower(), b) for p in parts for b in BANNED): bad.append(f)
        elif os.path.getsize(os.path.join(ROOT, f)) > LARGEST: bad.append(f + " (too large)")
    return bad

def main():
    if len(sys.argv) != 2: sys.exit(__doc__)
    tag = sys.argv[1]
    bad = check()
    if bad: sys.exit("not building, these tracked files should not ship:\n  " + "\n  ".join(bad))
    if git("status", "--porcelain").strip(): sys.exit("not building: commit your changes first, the zip is made from the last commit")
    out = os.path.join(ROOT, "releases", "out"); os.makedirs(out, exist_ok=True)
    name = f"SilentHillBoM-PC-{tag}"; path = os.path.join(out, name + ".zip")
    git("archive", "--format=zip", f"--prefix={name}/", "-o", path, "HEAD")
    digest = hashlib.sha256(open(path, "rb").read()).hexdigest()
    with open(path + ".sha256", "w", newline="\n") as f: f.write(f"{digest}  {name}.zip\n")
    print(f"{path}: {os.path.getsize(path) / 1048576:.1f} MB\nsha256 {digest}")

if __name__ == "__main__": main()
