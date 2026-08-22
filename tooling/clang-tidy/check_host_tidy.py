#!/usr/bin/env python3
"""Gate the host test/runner build with clang-tidy.

Runs clang-tidy over every translation unit in the host compile database
(build/host, exported by the host CMake build) and fails on any finding.
Unlike the firmware gate there is no baseline: the host tree is test
scaffolding, new code must be clean, and existing code may only get
cleaner.

The compile database is parsed with `-p`, so each file is analyzed with
its own flags. Findings are reported at the location clang-tidy prints
them; findings in system headers are a tooling artifact of the check set
and are already excluded in host/.clang-tidy where applicable.

Usage:
    check_host_tidy.py --db-dir <dir-with-compile_commands.json> \
        --repo <repo-root> [--clang-tidy <path>] [--jobs N]
"""

import argparse
import concurrent.futures
import json
import os
import re
import subprocess
import sys

FINDING_RE = re.compile(
    r"^(?P<path>\S.*?):(?P<line>\d+):(?P<col>\d+): (?P<level>error|warning): "
    r"(?P<msg>.+) \[(?P<checks>[A-Za-z0-9:.\-*]+(?:,?\s*[A-Za-z0-9:.\-*]+)*)\]$"
)


def run_clang_tidy(file, db_dir, clang_tidy):
    result = subprocess.run(
        [clang_tidy, "-p", db_dir, "--quiet", file],
        capture_output=True,
        text=True,
        timeout=600,
    )
    if result.returncode > 1:
        raise RuntimeError(
            f"clang-tidy failed on {file} (exit {result.returncode}):\n"
            + result.stderr[-2000:]
        )
    findings = []
    for line in (result.stdout + "\n" + result.stderr).splitlines():
        match = FINDING_RE.match(line)
        if match:
            path = match["path"]
            findings.append(f"{path}:{match['line']}:{match['col']}: "
                            f"{match['level']}: {match['msg']} [{match['checks']}]")
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--db-dir", required=True, help="directory containing compile_commands.json")
    parser.add_argument("--repo", required=True, help="repository root")
    parser.add_argument("--clang-tidy", default="clang-tidy", help="clang-tidy executable")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    args = parser.parse_args()

    if not args.clang_tidy or not os.path.isfile(args.clang_tidy):
        print(f"error: clang-tidy not found at {args.clang_tidy!r} "
              "(pass --clang-tidy /path/to/clang-tidy)", file=sys.stderr)
        return 2

    repo = os.path.abspath(args.repo)
    db_dir = os.path.abspath(args.db_dir)

    db_path = os.path.join(db_dir, "compile_commands.json")
    if not os.path.exists(db_path):
        print(f"error: {db_path} not found (build the host tree first: "
              f"cmake -S host -B build/host -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON)",
              file=sys.stderr)
        return 2
    with open(db_path) as f:
        files = sorted({e["file"] for e in json.load(f)})
    if not files:
        print("error: host compile database is empty", file=sys.stderr)
        return 2

    print(f"running {args.clang_tidy} over {len(files)} files with {args.jobs} jobs...")
    findings = []
    failures = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(run_clang_tidy, file, db_dir, args.clang_tidy): file
                   for file in files}
        for future in concurrent.futures.as_completed(futures):
            file = futures[future]
            try:
                findings.extend(future.result())
            except (RuntimeError, subprocess.TimeoutExpired) as exc:
                failures.append((file, str(exc)))
            print(f"  done: {os.path.relpath(file, repo)}")

    if failures:
        for file, error in failures:
            print(f"\nERROR: {file}\n{error}", file=sys.stderr)
        return 2

    findings.sort()
    for finding in findings:
        print(finding)
    if findings:
        print(f"\nFAIL: {len(findings)} finding(s). Fix them, or document a "
              "version-robust exclusion in host/.clang-tidy if they are "
              "accepted.", file=sys.stderr)
        return 1
    print(f"\nclean: {len(files)} files, 0 findings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
