#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/bin:$PATH"
cd "$(dirname "$0")/.."
mkdir -p tests/out/tmp
export TMPDIR="$PWD/tests/out/tmp" TMP="$PWD/tests/out/tmp" TEMP="$PWD/tests/out/tmp"
# Compile the production wait loop with observable audio/GX/input adapters.
# Keep this extraction bounded to the function, not a rewritten copy of it.
awk '/^static void low_cache_loop\(void\)$/ { copy=1 }
     copy { print }
     copy && /^}$/ { exit }' source/mplayer/mplayer.c > tests/out/low_cache_loop.inc
test -s tests/out/low_cache_loop.inc
gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror \
    tests/test_playback_wait.c -o tests/out/test_playback_wait.exe
timeout 10s tests/out/test_playback_wait.exe
