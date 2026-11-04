#!/usr/bin/env python3
"""Copy the firmware modules out of a Vita3K install into the library and decrypt them: vita3k_firmware.py <vita3k folder> <dump folder>"""
import os, shutil, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "decrypt"))
from vitadecrypt.pup import decrypt_tree_selfs

src, dump = sys.argv[1], sys.argv[2]
fw = os.path.join(dump, "firmware", "vita3k")
parts = (("os0", "us"), ("vs0", os.path.join("sys", "external")))
for root, sub in parts:
    if not os.path.isdir(os.path.join(src, root, sub)):
        sys.exit(f"{src} has no {os.path.join(root, sub)}: is its firmware installed?")
for root, sub in parts:
    shutil.copytree(os.path.join(src, root, sub), os.path.join(fw, root, sub), dirs_exist_ok=True)
    ok, failed = decrypt_tree_selfs(os.path.join(fw, root))
    print(f"{root}: {ok} modules decrypted, {len(failed)} failed")
    if failed: sys.exit(f"could not decrypt: {failed[:5]}")
