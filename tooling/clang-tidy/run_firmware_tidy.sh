#!/usr/bin/env bash
#
# End-to-end entry point for the firmware clang-tidy gate, shared by the
# local `c:firmware:tidy` dev task and the CI `firmware-tidy` job
# (.github/workflows/firmware-tidy.yml):
#
#   1. bootstraps the repository's west workspace (config/west.yml) if it
#      is missing, preserving the tracked zephyr/module.yml descriptor;
#   2. builds the smoke firmware if no compile database exists yet;
#   3. sanitizes the GCC-based compile database for clang and runs the
#      gate — any first-party finding fails.
#
# west and the Zephyr SDK come from devenv's firmware profile
# (`devenv shell -P firmware`); python3 and clang-tidy are in the base
# environment. See docs/quality.md, "Firmware clang-tidy".
set -euo pipefail

cd "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

if [ ! -f .west/config ]; then
    echo "No west workspace; bootstrapping (west init + west update) ..."
    # A fresh checkout already contains the tracked zephyr/module.yml
    # descriptor inside the gitignored zephyr/ directory, and west refuses
    # to clone into a non-empty directory. Preserve the descriptor across
    # the clone (the same dance the zephyr-tests CI job performs); the
    # trap restores it even if west update fails.
    saved_module_yml=""
    if [ -f zephyr/module.yml ]; then
        saved_module_yml="$(mktemp -d)/module.yml"
        cp zephyr/module.yml "$saved_module_yml"
        # Failure-path restore: if west init/update aborts before zephyr/
        # exists, a bare cp would fail too and its error could obscure the
        # original west failure, so mkdir first. (On success the trap is
        # cleared below after an immediate restore.)
        trap 'mkdir -p zephyr && cp "$saved_module_yml" zephyr/module.yml' EXIT
        rm -rf zephyr
    fi
    west -q init -l config
    west update
    # Restore the descriptor now, not just on exit: the firmware build
    # below reads this module's descriptor from <repo>/zephyr/module.yml
    # (zephyr_module via -DZMK_EXTRA_MODULES) to register the module's
    # devicetree bindings — a build without it fails with "lacks
    # binding" on nodes using the module's compatibles.
    if [ -n "$saved_module_yml" ]; then
        cp "$saved_module_yml" zephyr/module.yml
        trap - EXIT
    fi
fi

build_dir="build"
if [ ! -f "$build_dir/compile_commands.json" ]; then
    echo "No firmware compile database; building first (west build) ..."
    # -DZephyr_DIR: pin find_package(Zephyr) to this workspace's tree.
    # zmk/app's find_package(Zephyr HINTS ../zephyr) does not match the
    # in-tree package's non-standard layout, so CMake falls back to the
    # user package registry (~/.cmake/packages, which zmk's CI populates
    # via `west zephyr-export`). On a machine with other Zephyr west
    # workspaces (e.g. another keyboard's config repo), a stale registry
    # entry can redirect the build to a different Zephyr tree; an
    # explicit Zephyr_DIR short-circuits that search entirely.
    west build -s zmk/app -d "$build_dir" -b nice_nano//zmk -- \
        -DZMK_CONFIG="$PWD/config" \
        -DSHIELD=corne_left \
        -DZMK_EXTRA_MODULES="$PWD" \
        -DZephyr_DIR="$PWD/zephyr/share/zephyr-package/cmake"
fi

sanitized="$build_dir/clang-ccdb"
mkdir -p "$sanitized"
python3 tooling/clang-tidy/sanitize_compile_db.py \
    "$build_dir/compile_commands.json" \
    "$sanitized/compile_commands.json" \
    "$PWD"
python3 tooling/clang-tidy/check_firmware_tidy.py \
    --db-dir "$sanitized" \
    --repo "$PWD" \
    --clang-tidy "$(command -v clang-tidy)"
