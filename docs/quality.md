# C code quality

C-side quality tooling for the firmware sources (`src/`, `include/`) and the
native host harness (`host/`). Web-side tooling is tracked separately. The
full plan — static analysis, complexity gates, sanitizers, Zephyr compliance,
firmware clang-tidy, and BSim/ztest — is tracked in
[issue #76](https://github.com/measlesbagel/zmk-pointing-tools/issues/76).

## CI: the local tasks, run in GitHub Actions

Every check in this document is a dev task in `devenv.nix`, and CI runs the
tasks themselves rather than re-implementing them with runner-installed
binaries:

- **`checks` job** (`.github/workflows/check.yml`) — installs nix and
  devenv 2.2 (the version required by `devenv.yaml`) and runs
  `devenv tasks run repository:check` (format, complexity, host clang-tidy,
  cppcheck, JavaScript checks/tests, host tests) plus
  `devenv tasks run host:test:asan`.
- **`zephyr-compliance` job** — fetches the Zephyr tree at the rev pinned
  in `devenv.lock` and runs the `zephyr:compliance:check` task over the PR
  range.
- **`zephyr-tests` job** — fetches the same pinned Zephyr tree into
  `zephyr/` and runs the `zephyr:tests` task (the module's ztest firmware
  unit tests on `native_posix/native/64` via twister; see the
  firmware unit tests section below).
- The **firmware build** in `build.yml` (ZMK action) is unchanged and
  remains the firmware typecheck on every PR.

The toolchain is pinned by `devenv.lock` (nixpkgs revision included), so CI
analyzes with exactly the tools of the local dev shell; bumping the lock
moves local and CI together. The firmware clang-tidy gate stays local-only
by decision (see its section).

## Formatting (clang-format)

The repository style is defined by the root `.clang-format` file
(LLVM-based, 4-space indent, 100-column limit). Formatting applies to all C
sources under `src/`, `include/`, and `host/`; vendored trees (`zephyr/`,
`zmk/`, `modules/`, `optional/`, `build/`) are never touched.

- Check: `devenv tasks run c:format:check`
  (equivalent:
  `find src include host -type f \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format --dry-run --Werror`)
- Fix: `devenv tasks run c:format`
- CI: the `checks` job in `.github/workflows/check.yml` runs the task on
  every pull request and push to `main`.

`clang-tools` is part of the default devenv package set; CI runs the same
task with the same pinned tool (see the CI section below).

## Cyclomatic complexity (lizard)

`lizard` gates function cyclomatic complexity (CCN) without a build:

- `src/` and `include/` (firmware): CCN 15
- `host/` (test and runner harness): CCN 15

The ceiling is a flat 15 for both scopes: every function in the tree was
verified against it (the former worst offenders — `coherent_stage_process`
at 26 and the replay-runner `main`s at 31/26/19 — were decomposed into
small helpers, tracked in #76). `lizard` measures source-level branching and
does not expand macros, so the values are stable and comparable between
firmware and host code. `lizard` is part of the default devenv package
set; CI runs the same task with the same toolchain.

- Check: `devenv tasks run c:complexity:check`
- CI: the `checks` job in `.github/workflows/check.yml` runs the task on
  every pull request and push to `main`.

## Sanitizers (host test build)

The host CMake project accepts `-DZPT_ENABLE_SANITIZERS=ON`, which builds all
host test targets and replay runners with AddressSanitizer and
UndefinedBehaviorSanitizer (`-fno-sanitize-recover=all`, so any undefined
behavior fails the test instead of printing a note).

- Local: `devenv tasks run host:test:asan`
- CI: the `checks` job in `.github/workflows/check.yml` runs the task
  (the full CTest suite, including the Node.js trace-replay harness)
  under sanitizers on every PR and push to `main`.

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

The `src/platform/zmk` and `src/service` layers are covered by the
firmware clang-tidy gate below (full compile-database coverage) and by
cppcheck with stubbed devicetree macros. `c:tidy:check` configures and
builds the host tree with the nix gcc and runs the pinned clang-tidy per
file via `tooling/clang-tidy/check_host_tidy.py` (any finding fails;
`run-clang-tidy` is no longer shipped by recent LLVM releases, so the
gate does not depend on it). CI runs the very same dev tasks through
devenv (`devenv tasks run`), with the toolchain pinned by
`devenv.lock`, so local and CI results cannot drift; when that lock is
bumped and a tool update introduces new findings, triage them (fix or
justify a suppression) before re-freezing the baselines.

- Check: `devenv tasks run c:tidy:check`, `devenv tasks run c:cppcheck:check`
- CI: the `checks` job in `.github/workflows/check.yml` runs both tasks.

## Firmware clang-tidy (baseline-gated)

The host compile database cannot see `src/platform/zmk` or `src/service`
(zephyr/zmk headers and devicetree macros are unresolvable natively), so
the gate builds the actual firmware and analyzes the module's sources
against that compile database:

1. `west build` for `nice_nano//zmk` + `corne_left` (the same target the
   build job ships) with `-DZMK_EXTRA_MODULES=<this repo>`. The build
   generates `compile_commands.json` at configure time (there is no
   `compile_commands` ninja target in Zephyr 4.1).
2. `tooling/clang-tidy/sanitize_compile_db.py` rewrites the GCC-based
   database for clang: drops GCC-only flags (`-specs=`,
   `-mfp16-format=`, …), adds the GCC `include-fixed` directory (the SDK's
   `<sysroot>/include` alone does not satisfy `<limits.h>`), and keeps
   only first-party `src/` and `include/` entries.
3. `tooling/clang-tidy/check_firmware_tidy.py` runs clang-tidy per file
   (parallel), filters findings to first-party paths, and gates them
   against `tooling/clang-tidy/firmware_baseline.txt` — a frozen
   `file::function::check` baseline in the spirit of
   `.cppcheck-suppressions`. The gate **fails on any new finding** and
   **warns on stale entries**, so recorded debt can only shrink: fix a
   baselined function, then delete its line. Findings outside first-party
   paths (e.g. `misc-header-include-cycle` edges inside Zephyr's own
   headers) are ignored by design.

The current baseline is 13 `readability-function-cognitive-complexity`
entries (threshold 25) in platform/service glue. Their values are inflated
by Zephyr's logging-macro expansion (see the note in the static-analysis
section above), so they are ratcheted down over time rather than
restructured for the metric.

**Version handling.** Both tidy gates are tuned against the clang-tidy
**22** generation (the dev shell pins `llvmPackages_22.clang-tools`,
falling back to the floating package once nixpkgs removes the 22
series). The check set is kept version-robust where possible:

- Macro-generated Zephyr/ZMK symbols that newer versions flag as
  `misc-use-internal-linkage` (`K_THREAD_DEFINE`, `ZMK_SUBSCRIPTION`,
  `K_MSGQ_DEFINE`, `K_SEM_DEFINE`, `RING_BUF_DECLARE`) carry documented
  NOLINTs, because their external linkage is intrinsic to the macros
  (linker-section boot-time collection and documented `extern` access
  contracts), so the gate passes on both the 21 and 22 series.
- The 22 series check-set additions that fire on this tree are
  documented exclusions: `clang-analyzer-optin.*` (both configs —
  opt-in analyzers are enabled explicitly, not via the wildcard) and
  `bugprone-unchecked-string-to-number-conversion` (host config only —
  the replay runners parse machine-generated files with count-checked
  sscanf; see `host/.clang-tidy`).
- The host gate runs under the nix toolchain, so it parses the nix
  glibc headers rather than the system's — the system-header false
  positives that newer versions report on system glibc (e.g.
  `bugprone-macro-parentheses` in cdefs.h) do not affect it.

The frozen firmware baseline is still only meaningful against the tool
generation that produced it — to bump past 22: change the pin, run
both gates on a clean tree, triage the new findings (fix, NOLINT with
a reason, or baseline), and re-freeze `firmware_baseline.txt` if the
debt changed.

**Local-only by decision.** This gate is deliberately not a CI job:
the dev task is full-fidelity (same real firmware compile database,
same scripts, same pinned tool), the repo has a single maintainer, and
the dedicated build job already typechecks the firmware on every PR. A
CI job would cost ~15-25 minutes of CI per PR (west update +
firmware build + tidy over 41 translation units) to enforce a check
that runs locally anyway. The standing rule: run the gate before
pushing any PR that touches `src/` or `include/`. If enforcement is
ever wanted (e.g. a second contributor), the cheap option is a
diff-scoped CI job that tidies only the PR-changed files — sound under
a pinned tool, since unchanged files cannot produce new findings.

- Check: `devenv shell -P firmware && devenv tasks run c:firmware:tidy`
  (builds the firmware first if `build/compile_commands.json` is
  missing; re-run `west build` or `rm -rf build` to refresh a stale one)

## Editor support (clangd)

`.clangd` points editors at the host compile database, so the host-compilable
sources get full semantic analysis plus the same clang-tidy checks that run
in CI (clangd loads the nearest `.clang-tidy` automatically). Generate the
database with `devenv tasks run c:tidy:check` (or the commands in `.clangd`).
`src/platform/zmk` and `src/service` files get real semantics from the
sanitized firmware database instead: `devenv tasks run c:firmware:tidy`
(firmware profile) writes it to `build/clang-ccdb/compile_commands.json` —
point clangd at that directory (e.g. `clangd --compile-commands-dir=
build/clang-ccdb`) to analyze the platform layer.

## Zephyr compliance (check_compliance.py)

The `zephyr-compliance` job in `.github/workflows/check.yml` runs the same
`zephyr:compliance:check` task as local over the diff range (the PR's
base..head on a pull request, `HEAD~1..HEAD` on a push to `main`). The job
fetches only the Zephyr project — at the exact rev pinned in `devenv.lock`,
the same rev the local west workspace uses — into `zephyr-tree/` (the repo
itself tracks `zephyr/module.yml`, so `zephyr/` is already occupied) and
points the task at it via `ZEPHYR_BASE` plus the `COMPLIANCE_SCRIPT` and
`COMPLIANCE_RANGE` overrides (all default to the local behavior, so an
unmodified local run is unchanged); the module-safe check subset never
touches ZMK, so no full west workspace is needed. The checks are:

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
  flagged, so the gate is left to the `checks` job instead.
- **The Kconfig parse family** (`Kconfig*`, `SysbuildKconfig*`) — the script
  runs `git grep` for `SB_CONFIG_*` symbols from the top-level repo; on a
  module repo that grep exits 1 (no match) and the script treats that as a
  fatal error. Kconfig and devicetree *correctness* is already gated by the
  firmware build, which configures Kconfig and compiles the devicetree for
  real boards.

Locally the dev task does this with the west workspace's `zephyr/` tree
and the Python environment from the default shell (which carries the
script's imports: `unidiff`, `yamllint`, `junitparser`, `lxml`,
`python-magic`, `west`):

    devenv tasks run zephyr:compliance:check          # HEAD~1..HEAD
    COMPLIANCE_RANGE=origin/main...HEAD \
      devenv tasks run zephyr:compliance:check        # a wider range

The CI gate is diff-scoped, so it only polices files a PR touches. To audit
the *entire* tree at once (e.g. when first adopting the gate), diff against
git's empty tree instead of a commit range. `GitDiffCheck` is per-commit and
is left out of this form:

    python3 zephyr/scripts/ci/check_compliance.py \
      -c "4b825dc642cb6eb9a060e54bf8d69288fbee4904..HEAD" \
      -m ClangFormat -m DevicetreeBindings -m Nits -m YAMLLint \
      -m TextEncoding -m BinaryFiles

## Firmware unit tests (ztest)

The Zephyr-coupled firmware logic — the runtime-tuning service
(`src/service/tuning.c`) and the telemetry service (`src/service/telemetry.c`)
— is unit-tested with ztest under the `native_posix/native/64` POSIX target
(`tests/unit`, suite `zpt.unit`). These are the sources the host replay
harness cannot reach (they call Zephyr APIs); the pure pipeline/stage logic
remains covered by the host harness (the `host:test` task), and BSim is not
used — `native_posix` is the fast local/CI baseline.

- **UART under test** — the telemetry service talks to a `vnd,serial`
  virtual UART (`zephyr/drivers/serial/serial_test.c`, auto-enabled when a
  node with that compatible is present): test code injects host frames into
  its RX queue with `serial_vnd_queue_in_data()` and reads service
  responses from its TX buffer. The firmware callback tolerates drivers
  without the `irq_update` UART operation (the mock returns `-ENOSYS`; the
  production CDC-ACM driver implements it), so the same code runs on both.
- **One suite, ordered by name** — ztest executes suites *and* cases in
  name-sorted linker order (not link order), and the tuning registry is a
  process-wide singleton with no unregister API. All cases therefore live
  in a single `zpt_unit` suite, and the capacity-fill case is named
  `z_capacity_fill` so it sorts last. State-target ids are allocated above
  the tuning targets and are never freed, so cases make no assumption that
  their id is the highest one.
- **Wake model** — the telemetry service thread only wakes on UART input;
  state samples queued from the test thread flush on the next host frame
  (the tests ping after submitting, mirroring production where the host
  keeps the link alive).

The test app loads the module through the `ZEPHYR_MODULES` env var (its real
Kconfig, CMake, devicetree bindings, and library path — twister's cmake
configure has no option to pass arbitrary `-D` args). It is set rather than
relying on the west workspace because Zephyr only runs its module
registration script when `WEST` or `ZEPHYR_MODULES` is set, and the `WEST`
cmake variable is `WEST-NOTFOUND` when no `.west/` workspace is found above
`ZEPHYR_BASE` — the situation in CI, where the zephyr tree is fetched
without a workspace. It runs with the host gcc
(`ZEPHYR_TOOLCHAIN_VARIANT=host`), so no Zephyr SDK is needed.

Twister runs as a plain python script
(`zephyr/scripts/twister -p native_posix/native/64 -T tests`) rather than
through west: west registers the twister command only after resolving the
manifest import chain (`config/west.yml` → `zmk/app/west.yml`), which
requires the `zmk` project checkout, while the script derives
`ZEPHYR_BASE` from its own location and has no west dependency. CI relies
on this, since the `zephyr-tests` job fetches only the zephyr tree.

- Check: `devenv tasks run zephyr:tests`
- CI: the `zephyr-tests` job in `.github/workflows/check.yml` runs the same
  task on every pull request and push to `main`.

## Planned (tracked in #76)

- BSim as an optional heavier extension of the ztest suites
