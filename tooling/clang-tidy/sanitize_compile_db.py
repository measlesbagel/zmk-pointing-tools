#!/usr/bin/env python3
"""Rewrite a Zephyr (GCC) compile database so clang-tidy can use it.

Zephyr builds with the Zephyr SDK's GCC. The compile database it emits is
not directly consumable by clang-tidy for two reasons:

1. GCC-only command-line flags that clang rejects.
2. The SDK's libc (newlib) include paths are baked into the gcc binary's
   specs files; clang never reads specs, so its system include search would
   fall through to the host's glibc and misparse <limits.h> and friends.

This script keeps only entries for first-party sources (under <src-root>/src
and <src-root>/include), drops the GCC-only flags, and re-adds the SDK's
libc include directories explicitly, in the same order gcc uses them.
include-fixed must come first: its limits.h wrapper defines _GCC_LIMITS_H_,
which stops newlib's #include_next chain from reaching the host glibc.

Usage:
    sanitize_compile_db.py <compile_commands.json> <out.json> <src-root>

<src-root> is the absolute path of the repository root (the west workspace
root when building locally).
"""

import glob
import json
import os
import re
import sys

# Flags accepted by the Zephyr SDK's GCC but rejected or ignored by clang.
GCC_ONLY_FLAGS = (
    "-mfp16-format=ieee",
    "-specs=picolibc.specs",
    "-fno-defer-pop",
    "-fno-reorder-functions",
    "-Wexpansion-to-defined",
    "-Wfatal-errors",
    "--param=min-pagesize=0",
)


def sanitize_command(command: str) -> str:
    for flag in GCC_ONLY_FLAGS:
        command = command.replace(flag + " ", "")

    gcc_bin = command.split()[0]
    sdk = os.path.dirname(os.path.dirname(gcc_bin))

    sysroot_match = re.search(r"--sysroot=(\S+)", command)
    # Fallback: the toolchain directory itself is the sysroot in the SDK
    # layout (bin/, include/, lib/, sys-include/ all live directly under
    # it).
    sysroot = sysroot_match.group(1) if sysroot_match else sdk

    extra = ""
    # lib/gcc/<triple>/<version>/include-fixed
    inc_fixed = sorted(glob.glob(os.path.join(sdk, "lib", "gcc", "*", "*", "include-fixed")))
    if inc_fixed:
        extra += " -isystem " + inc_fixed[0]
    extra += " -isystem " + sysroot + "/include"
    extra += " -isystem " + sysroot + "/sys-include"

    if "--sysroot=" in command:
        command = command.replace("--sysroot=" + sysroot, "--sysroot=" + sysroot + extra, 1)
    else:
        command += " --sysroot=" + sysroot + extra
    return command


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    src_db, dst_db, src_root = sys.argv[1], sys.argv[2], os.path.abspath(sys.argv[3])

    with open(src_db) as f:
        db = json.load(f)

    kept = []
    for entry in db:
        file_path = os.path.abspath(entry["file"])
        if not file_path.startswith(src_root + os.sep):
            continue
        rel_path = os.path.relpath(file_path, src_root)
        if not rel_path.startswith(("src/", "include/")):
            continue
        if not file_path.endswith((".c", ".h")):
            continue
        kept.append({**entry, "command": sanitize_command(entry["command"])})

    with open(dst_db, "w") as f:
        json.dump(kept, f, indent=1)
    print(f"kept {len(kept)}/{len(db)} entries -> {dst_db}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
