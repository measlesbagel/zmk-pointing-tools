# C code quality

C-side quality tooling for the firmware sources (`src/`, `include/`) and the
native host harness (`host/`). Web-side tooling is tracked separately. The
full plan — static analysis, complexity gates, sanitizers, Zephyr compliance,
firmware clang-tidy, and BSim/ztest — is tracked in
[issue #76](https://github.com/measlesbagel/zmk-pointing-tools/issues/76).

## Formatting (clang-format)

The repository style is defined by the root `.clang-format` file
(LLVM-based, 4-space indent, 100-column limit). Formatting applies to all C
sources under `src/`, `include/`, and `host/`; vendored trees (`zephyr/`,
`zmk/`, `modules/`, `optional/`, `build/`) are never touched.

- Check: `devenv task c:format:check`
  (equivalent:
  `find src include host -type f \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format --dry-run --Werror`)
- Fix: `devenv task c:format`
- CI: the `c-style` job in `.github/workflows/check.yml` runs the check on
  every pull request and push to `main`.

`clang-tools` is part of the default devenv package set. CI uses the
`clang-format` preinstalled on the runner image; if a runner update changes
its output, re-run the fix task and commit the result.

## Cyclomatic complexity (lizard)

`lizard` gates function cyclomatic complexity (CCN) without a build:

- `src/` and `include/` (firmware): CCN 15
- `host/` (test and runner harness): CCN 15

The ceiling is a flat 15 for both scopes: every function in the tree was
verified against it (the former worst offenders — `coherent_stage_process`
at 26 and the replay-runner `main`s at 31/26/19 — were decomposed into
small helpers, tracked in #76). `lizard` measures source-level branching and
does not expand macros, so the values are stable and comparable between
firmware and host code. CI pins `lizard==1.23.0`; `lizard` is part of the
default devenv package set.

- Check: `devenv task c:complexity:check`
- CI: `c-style` job in `.github/workflows/check.yml`.

## Sanitizers (host test build)

The host CMake project accepts `-DZPT_ENABLE_SANITIZERS=ON`, which builds all
host test targets and replay runners with AddressSanitizer and
UndefinedBehaviorSanitizer (`-fno-sanitize-recover=all`, so any undefined
behavior fails the test instead of printing a note).

- Local: `devenv task host:test:asan`
- CI: `host-sanitizers` job in `.github/workflows/check.yml` runs the full
  CTest suite (including the Node.js trace-replay harness) under
  sanitizers on every PR and push to `main`.

## Static analysis (clang-tidy and cppcheck)

Two static analyzers cover the C sources:

- **clang-tidy** runs over the host compile database
  (`build/host/compile_commands.json`), covering everything the host CMake
  project compiles: `src/core`, `src/stage`, `src/source`, `include/`, and
  `host/`. The check set (see `.clang-tidy`) favors correctness —
  `bugprone-*`, `clang-analyzer-*`, `misc-*`, `performance-*`,
  `portability-*` — over style, which clang-format owns, plus
  `readability-function-cognitive-complexity` (threshold 25) as the
  cognitive-complexity counterpart to the lizard cyclomatic gate. Files under
  `host/` use `host/.clang-tidy`, the same checks with a higher cognitive
  threshold (100) because test functions are long sequential verify lists.
  Both thresholds are ratchets, lowered over time as code is decomposed
  (#76). Note that clang-tidy measures cognitive complexity on macro-
  *expanded* code, so Zephyr's logging macros (`LOG_WRN` and friends expand
  to several level-checking `if`s, each with nesting penalties) inflate the
  values of service-layer functions that use them heavily — e.g.
  `zpt_uart_callback` measures ~49 while its source-level branching is
  trivial. The lizard CCN gate above is the macro-free ceiling; the
  cognitive thresholds apply where a real AST is available (host compile
  database).
- **cppcheck** (`--enable=warning,performance,portability --std=c11`) runs
  over `src/`, `include/`, and `host/` without a build. Zephyr build-system
  macros that cannot be resolved outside the west build are stubbed in
  `tooling/cppcheck/preinclude.h`. Suppressions live in
  `.cppcheck-suppressions`, a frozen baseline: new findings should be fixed,
  not suppressed.

The `src/platform/zmk` layer is only partially covered by either (cppcheck
  analyzes it with stubbed devicetree macros; clang-tidy does not see it yet).
Full coverage for that layer comes with the firmware compile database
tracked in #76. Note that both tools use version-drifting runner binaries
(the clang-tools and apt cppcheck preinstalled on the GitHub runner image);
when a tool update introduces new findings, triage them (fix or justify a
suppression) before re-freezing the baselines.

- Check: `devenv task c:tidy:check`, `devenv task c:cppcheck:check`
- CI: `c-analysis` job in `.github/workflows/check.yml`.

## Editor support (clangd)

`.clangd` points editors at the host compile database, so the host-compilable
sources get full semantic analysis plus the same clang-tidy checks that run
in CI (clangd loads the nearest `.clang-tidy` automatically). Generate the
database with `devenv task c:tidy:check` (or the commands in `.clangd`).
`src/platform/zmk` files fall back to clangd's heuristics until the firmware
compile database workflow lands (#76).

## Zephyr compliance (check_compliance.py)

The `zephyr-compliance` job in `.github/workflows/check.yml` runs the
module-safe subset of Zephyr's own `scripts/ci/check_compliance.py` over the
diff range (the PR's base..head on a pull request, `HEAD~1..HEAD` on a push
to `main`):

- **ClangFormat** — `clang-format-diff.py` over changed C lines.
- **DevicetreeBindings** — flags redundant `required: false` in changed
  `dts/bindings/*.yaml`.
- **Nits** — small style nits in changed files (redundant Zephyr sources,
  devicetree separators, …).
- **YAMLLint** — the Zephyr `.yamllint` config over changed YAML (with the
  relaxed rules it applies to `.github/` workflows).
- **GitDiffCheck** — `git diff --check` for whitespace errors and conflict
  markers.
- **TextEncoding** / **BinaryFiles** — encoding sanity and no unintended
  binary files.

Two of the script's checks are deliberately **not** run for a module repo:

- **Checkpatch** — it enforces the Linux-kernel style (tab indentation,
  80-column lines), which directly contradicts this repository's
  clang-format style (4-space, 100-column). Every formatted line would be
  flagged, so the gate is left to the `c-style` job instead.
- **The Kconfig parse family** (`Kconfig*`, `SysbuildKconfig*`) — the script
  runs `git grep` for `SB_CONFIG_*` symbols from the top-level repo; on a
  module repo that grep exits 1 (no match) and the script treats that as a
  fatal error. Kconfig and devicetree *correctness* is already gated by the
  firmware build, which configures Kconfig and compiles the devicetree for
  real boards.

Running the same subset locally requires the west workspace (`zephyr/`,
`zmk/`, …) plus a Python environment with the script's imports
(`unidiff`, `yamllint`, `junitparser`, `lxml`, `python-magic`, `west`):

    python3 zephyr/scripts/ci/check_compliance.py -c "origin/main...HEAD" \
      -m ClangFormat -m DevicetreeBindings -m Nits -m YAMLLint \
      -m GitDiffCheck -m TextEncoding -m BinaryFiles

The CI gate is diff-scoped, so it only polices files a PR touches. To audit
the *entire* tree at once (e.g. when first adopting the gate), diff against
git's empty tree instead of a commit range. `GitDiffCheck` is per-commit and
is left out of this form:

    python3 zephyr/scripts/ci/check_compliance.py \
      -c "4b825dc642cb6eb9a060e54bf8d69288fbee4904..HEAD" \
      -m ClangFormat -m DevicetreeBindings -m Nits -m YAMLLint \
      -m TextEncoding -m BinaryFiles

## Planned (tracked in #76)

- Firmware clang-tidy via `west build -t compile_commands` (PR-only job)
- BSim/ztest firmware unit tests
