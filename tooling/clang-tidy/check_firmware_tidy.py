#!/usr/bin/env python3
"""Gate first-party firmware sources with clang-tidy.

Runs clang-tidy over every first-party translation unit in a sanitized
firmware compile database (see sanitize_compile_db.py) and fails on any
finding.

Rules:
- Only findings located in first-party files (<repo>/src, <repo>/include)
  are gated. Findings located in Zephyr or ZMK headers are upstream
  artifacts (e.g. misc-header-include-cycle inside zephyr/kernel.h) and are
  ignored.
- Any first-party finding fails the gate: the code must stay clean, and a
  new finding means either a real regression or a tool-update artifact that
  needs triage (fix, or a documented NOLINT / check exclusion).

Usage:
    check_firmware_tidy.py --db-dir <dir-with-compile_commands.json> \
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
FIRST_PARTY_PREFIXES = ("src/", "include/")


class Finding:
    def __init__(self, path, line, msg, checks):
        self.path = path
        self.line = line
        self.msg = msg
        self.checks = [c for c in checks.split(",") if c and c != "-warnings-as-errors"]

    def key(self):
        return f"{self.path}:{self.line} [{','.join(self.checks)}] {self.msg}"


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
            findings.append(Finding(match["path"], int(match["line"]), match["msg"], match["checks"]))
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
        print(f"error: {db_path} not found (run sanitize_compile_db.py first)", file=sys.stderr)
        return 2
    with open(db_path) as f:
        files = [e["file"] for e in json.load(f)]
    if not files:
        print("error: sanitized compile database is empty", file=sys.stderr)
        return 2

    print(f"running {args.clang_tidy} over {len(files)} files with {args.jobs} jobs...")
    findings = []
    failures = []
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        futures = {
            pool.submit(run_clang_tidy, f, db_dir, args.clang_tidy): f for f in files
        }
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

    for finding in findings:
        if os.path.isabs(finding.path):
            finding.path = os.path.relpath(finding.path, repo)
    first_party = [f for f in findings if f.path.startswith(FIRST_PARTY_PREFIXES)]

    print(f"\n{len(first_party)} first-party finding(s)")

    if first_party:
        for finding in first_party:
            print(f"FAIL: {finding.key()}", file=sys.stderr)
        print("\nFAIL: fix the finding(s) above, or document them with a "
              "NOLINT / check exclusion if they are accepted artifacts.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
