#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/bin:$PATH"
REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
mkdir -p tests/out/tmp
export TMPDIR="$REPO/tests/out/tmp" TMP="$REPO/tests/out/tmp" TEMP="$REPO/tests/out/tmp"
if [ ! -d vendor/mxml ]; then
    git clone --depth 1 --branch v3.3.1 https://github.com/michaelrsweet/mxml.git vendor/mxml
fi
if [ ! -f vendor/mxml/Makefile ]; then
    (cd vendor/mxml && CC=gcc ./configure --build=x86_64-pc-cygwin --disable-shared --disable-threads) > tests/out/mxml-configure.log
fi
make -C vendor/mxml -j4 libmxml.a > tests/out/mxml-build.log
gcc -std=gnu11 -g -O1 -Wall -Wextra -Werror -Ivendor/mxml -Isource/utils \
    tests/test_online.c source/utils/http_client.c source/utils/net_transport.c \
    source/utils/radio_stream.c source/utils/webdav_client.c \
    vendor/mxml/libmxml.a -o tests/out/test_online.exe
node tests/online-server.mjs
