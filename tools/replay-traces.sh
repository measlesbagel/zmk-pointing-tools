#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${TMPDIR:-/tmp}/zmk-pointing-tools-replay"
scroll_runner="$build_dir/scroll-replay"
text_runner="$build_dir/text-nav-replay"
mkdir -p "$build_dir"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/include" \
  "$repo_root/src/axis_intent.c" \
  "$repo_root/src/scroll.c" \
  "$repo_root/tools/scroll-replay.c" \
  -o "$scroll_runner"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$repo_root/include" \
  "$repo_root/src/text_nav.c" \
  "$repo_root/tools/text-nav-replay.c" \
  -o "$text_runner"

exec node "$repo_root/tools/replay-traces.js" \
  --scroll-runner "$scroll_runner" \
  --text-runner "$text_runner" \
  "$@"
