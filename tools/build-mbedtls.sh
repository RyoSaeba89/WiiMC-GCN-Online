#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/bin:$PATH"
REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC="$REPO/vendor/mbedtls"
PREFIX="$REPO/vendor/prefix"
: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITPPC:=$DEVKITPRO/devkitPPC}"
export PATH="$DEVKITPPC/bin:$PATH"
mkdir -p "$REPO/vendor/tmp" "$PREFIX/include" "$PREFIX/lib"
export TMPDIR="$REPO/vendor/tmp" TMP="$REPO/vendor/tmp" TEMP="$REPO/vendor/tmp"
if [ ! -f "$SRC/library/ssl_tls.c" ]; then
    git clone --depth 1 --branch v3.6.7 --recurse-submodules --shallow-submodules \
        https://github.com/Mbed-TLS/mbedtls.git "$SRC"
fi
EXPECTED=068ff080b369adfac81509f9b57b2afabaf82dc5
[ "$(git -C "$SRC" rev-parse HEAD)" = "$EXPECTED" ] || { echo "Unexpected mbedTLS revision" >&2; exit 1; }
# The project configuration lives outside the mbedTLS source tree, so make
# cannot discover that a config-only edit invalidates every library object.
# Clean generated objects before rebuilding to make such changes effective.
make -C "$SRC/library" clean
make -C "$SRC/library" -j4 CC=powerpc-eabi-gcc AR=powerpc-eabi-ar \
    CFLAGS="-O2 -Wall -DGEKKO -mcpu=750 -meabi -mhard-float -I$REPO/source -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>' -ffunction-sections -fdata-sections" static
cp "$SRC/library/libmbedtls.a" "$SRC/library/libmbedx509.a" "$SRC/library/libmbedcrypto.a" "$PREFIX/lib/"
cp -r "$SRC/include/mbedtls" "$SRC/include/psa" "$PREFIX/include/"
cp "$SRC/LICENSE" "$PREFIX/LICENSE-mbedtls"
cat "$REPO/source/mbedtls_config.h" "$REPO/tools/build-mbedtls.sh" | sha256sum | cut -d ' ' -f1 > "$PREFIX/wiimc-tls.sha256"
