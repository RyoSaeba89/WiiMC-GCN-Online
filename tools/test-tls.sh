#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/bin:$PATH"
REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"
mkdir -p tests/out/tmp tests/out/tls-fixtures
export TMPDIR="$REPO/tests/out/tmp" TMP="$REPO/tests/out/tmp" TEMP="$REPO/tests/out/tmp"
fixture="$REPO/tests/out/tls-fixtures"
if [ ! -f "$fixture/future.pem" ]; then
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -keyout "$fixture/ca-key.pem" -out "$fixture/ca.pem" -days 3650 -subj '/CN=Test CA' 2>/dev/null
    openssl req -new -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -keyout "$fixture/key.pem" -out "$fixture/request.pem" -subj '/CN=localhost' 2>/dev/null
    touch "$fixture/index"
    echo 1000 > "$fixture/serial"
    cat > "$fixture/ca.conf" <<EOF
[ca]
default_ca = test
[test]
database = $fixture/index
serial = $fixture/serial
new_certs_dir = $fixture
certificate = $fixture/ca.pem
private_key = $fixture/ca-key.pem
default_md = sha256
policy = names
unique_subject = no
[names]
commonName = supplied
[server]
basicConstraints = CA:FALSE
keyUsage = digitalSignature
extendedKeyUsage = serverAuth
subjectAltName = DNS:localhost
EOF
    # OpenSSL's ca command permits fixed past/future validity for this fixture.
    openssl ca -batch -notext -config "$fixture/ca.conf" -extensions server \
        -startdate 20000101000000Z -enddate 20010101000000Z \
        -in "$fixture/request.pem" -out "$fixture/expired.pem" 2>/dev/null
    # Wrong-name cert has otherwise identical trust/signature properties.
    openssl x509 -req -in "$fixture/request.pem" -CA "$fixture/ca.pem" -CAkey "$fixture/ca-key.pem" \
        -set_serial 2000 -days 3650 -extfile <(printf 'subjectAltName=DNS:wrong.invalid\nextendedKeyUsage=serverAuth\n') \
        -out "$fixture/wrong-host.pem" 2>/dev/null
    openssl x509 -req -in "$fixture/request.pem" -signkey "$fixture/key.pem" \
        -days 3650 -out "$fixture/untrusted.pem" 2>/dev/null
    openssl ca -batch -notext -config "$fixture/ca.conf" -extensions server \
        -startdate 20990101000000Z -enddate 21000101000000Z \
        -in "$fixture/request.pem" -out "$fixture/future.pem" 2>/dev/null
fi
if [ ! -f "$fixture/sha384.pem" ]; then
    openssl x509 -req -in "$fixture/request.pem" -CA "$fixture/ca.pem" -CAkey "$fixture/ca-key.pem" \
        -sha384 -set_serial 3000 -days 3650 -out "$fixture/sha384.pem" 2>/dev/null
fi
for mode in production dates-enabled; do
    out="$REPO/tests/out/tls-$mode"
    mkdir -p "$out"
    extra=()
    # Exercise the explicit GCRadio callback as well as the no-date build.
    if [ "$mode" = dates-enabled ]; then extra=(-DMBEDTLS_HAVE_TIME_DATE); fi
    flags=(-O1 -g -I"$REPO/source" -I"$REPO/vendor/mbedtls/include" \
        -DMBEDTLS_CONFIG_FILE='<mbedtls_config.h>' "${extra[@]}")
    signature=$(cat source/mbedtls_config.h tools/test-tls.sh | sha256sum | cut -d ' ' -f1)
    if [ ! -f "$out/config.sha256" ] || [ "$(cat "$out/config.sha256")" != "$signature" ]; then
        (cd "$out" && gcc "${flags[@]}" -c "$REPO"/vendor/mbedtls/library/*.c && ar rcs libtls.a ./*.o)
        echo "$signature" > "$out/config.sha256"
    fi
    gcc "${flags[@]}" -DWANT_TLS -DWANT_DEBUGLOG -pthread -Itests/host -Isource/utils \
        tests/test_tls.c source/utils/gc_tls.c source/utils/net_transport.c \
        source/utils/http_client.c "$out/libtls.a" \
        -o "$out/test_tls.exe"
    node tests/tls-server.mjs "tests/out/tls-$mode/test_tls.exe"
done
