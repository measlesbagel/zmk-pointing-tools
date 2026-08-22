#!/usr/bin/env python3
"""Gate first-party firmware sources with clang-tidy and a frozen baseline.

Runs clang-tidy over every first-party translation unit in a sanitized
firmware compile database (see sanitize_compile_db.py) and gates the
results against a frozen baseline file of pre-existing findings.

Rules:
- Only findings located in first-party files (<repo>/src, <repo>/include)
  are gated. Findings located in Zephyr or ZMK headers are upstream
  artifacts (e.g. misc-header-include-cycle inside zephyr/kernel.h) and are
  ignored.
- A finding that matches a baseline entry is pre-existing debt: it is
  reported but does not fail the gate.
- Any other finding fails the gate: new code must be clean, and existing
  code may only get cleaner.
- Baseline entries that no longer produce a finding are reported as stale
  (warning only): remove them and lower the debt.

Baseline entry format (one per line; '#' starts a comment):

    <file>::<function>::<check>

<file> is relative to the repository root, <check> is a clang-tidy check
name, and <function> is the function name as reported by clang-tidy (from
the diagnostic message). An empty <function> matches every finding of that
check in that file.

Usage:
    check_firmware_tidy.py --db-dir <dir-with-compile_commands.json> \
        --repo <repo-root> [--clang-tidy <path>] [--baseline <file>] \
        [--jobs N]
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
FUNCTION_RE = re.compile(r"\bfunction '([^']+)'")
FIRST_PARTY_PREFIXES = ("src/", "include/")


class Finding:
    def __init__(self, path, line, msg, checks):
        self.path = path
        self.line = line
        self.msg = msg
        self.checks = [c for c in checks.split(",") if c and c != "-warnings-as-errors"]
        match = FUNCTION_RE.search(msg)
        self.function = match.group(1) if match else None

    def key(self):
        return f"{self.path}:{self.line} [{','.join(self.checks)}] {self.msg}"

    def matches(self, entry_file, entry_function, entry_check):
        if entry_check not in self.checks or self.path != entry_file:
            return False
        if entry_function:
            return self.function is not None and self.function == entry_function
        return True


def load_baseline(path):
    entries = []
    if not os.path.exists(path):
        return entries
    with open(path) as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("::")
            if len(parts) != 3:
                raise ValueError(f"malformed baseline entry: {line!r}")
            entries.append(tuple(parts))
    return entries


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
    parser.add_argument("--baseline", default=None, help="baseline file (default: next to this script)")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    args = parser.parse_args()

    if not args.clang_tidy or not os.path.isfile(args.clang_tidy):
        print(f"error: clang-tidy not found at {args.clang_tidy!r} "
              "(pass --clang-tidy /path/to/clang-tidy)", file=sys.stderr)
        return 2

    repo = os.path.abspath(args.repo)
    db_dir = os.path.abspath(args.db_dir)
    baseline_path = args.baseline or os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "firmware_baseline.txt"
    )

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
    baseline = load_baseline(baseline_path)

    new_findings = []
    matched_entries = set()
    for finding in first_party:
        for index, entry in enumerate(baseline):
            if finding.matches(*entry):
                matched_entries.add(index)
                break
        else:
            new_findings.append(finding)

    print(f"\n{len(first_party)} first-party findings, "
          f"{len(new_findings)} new, {len(baseline) - len(matched_entries)} stale baseline entries\n")

    for finding in new_findings:
        print(f"NEW: {finding.key()}", file=sys.stderr)

    stale = [baseline[i] for i in range(len(baseline)) if i not in matched_entries]
    for entry in stale:
        print(f"stale baseline entry (no longer produces a finding; remove it): "
              f"{entry[0]}::{entry[1]}::{entry[2]}", file=sys.stderr)

    if new_findings:
        print(f"\nFAIL: {len(new_findings)} new finding(s) not in the baseline. "
              f"Fix them, or add them to {os.path.relpath(baseline_path, repo)} "
              f"if they are accepted pre-existing debt.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
