#!/usr/bin/env bash
set -euo pipefail

build_dir="${TMPDIR:-/tmp}/zmk-pointing-tools-tests"
mkdir -p "$build_dir"

cc -std=c11 -Wall -Wextra -Werror \
  -Iinclude \
  src/axis_intent.c tests/axis_intent_test.c \
  -o "$build_dir/axis-intent-test"

"$build_dir/axis-intent-test"

cc -std=c11 -Wall -Wextra -Werror \
  -Iinclude \
  src/text_nav.c tests/text_nav_test.c \
  -o "$build_dir/text-nav-test"

"$build_dir/text-nav-test"
