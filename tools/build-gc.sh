#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/bin:$PATH"
REPO=$(cd "$(dirname "$0")/.." && pwd)
: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITPPC:=$DEVKITPRO/devkitPPC}"
export DEVKITPRO DEVKITPPC
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:/usr/bin:$PATH"
mkdir -p "$REPO/build/tmp"
export TMPDIR="$REPO/build/tmp" TMP="$REPO/build/tmp" TEMP="$REPO/build/tmp"
cd "$REPO"
# Timestamp-only checks miss restored/stale archives. Bind the libraries to
# both the TLS configuration and the build recipe before linking the DOL.
tls_signature=$(cat source/mbedtls_config.h tools/build-mbedtls.sh | sha256sum | cut -d ' ' -f1)
if [ ! -f vendor/prefix/lib/libmbedtls.a ] ||
   [ ! -f vendor/prefix/lib/libmbedx509.a ] ||
   [ ! -f vendor/prefix/lib/libmbedcrypto.a ] ||
   [ ! -f vendor/prefix/wiimc-tls.sha256 ] ||
   [ "$(cat vendor/prefix/wiimc-tls.sha256)" != "$tls_signature" ]; then
    bash tools/build-mbedtls.sh
fi
# Keep deployment explicit: tools/prepare-sd.ps1 builds a reviewable SD package.
make -f Makefile.gc SDCARD= "$@"
