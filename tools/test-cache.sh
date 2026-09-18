#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/bin:$PATH"
cd "$(dirname "$0")/.."
mkdir -p tests/out/tmp
export TMPDIR="$PWD/tests/out/tmp" TMP="$PWD/tests/out/tmp" TEMP="$PWD/tests/out/tmp"
gcc -std=gnu11 -O2 -g -pthread -Itests/host -Isource/mplayer \
    tests/test_cache.c -o tests/out/test_cache.exe
timeout 30s tests/out/test_cache.exe
