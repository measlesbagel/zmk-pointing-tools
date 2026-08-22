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

- `src/` and `include/` (firmware): CCN 30
- `host/` (test and runner harness): CCN 40

The thresholds are ratchets: they sit above the current worst function so
new code cannot exceed the existing peak, and they should be lowered in
follow-up work (tracked in #76) as functions are decomposed. CI pins
`lizard==1.23.0`; `lizard` is part of the default devenv package set.

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

## Planned (tracked in #76)

- clang-tidy (including cognitive complexity) and cppcheck with a frozen
  suppression baseline; clangd for editors
- Zephyr compliance checks (checkpatch, devicetree bindings, Kconfig)
- Firmware clang-tidy via `west build -t compile_commands` (PR-only job)
- BSim/ztest firmware unit tests
