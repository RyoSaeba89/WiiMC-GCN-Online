#!/usr/bin/env bash
# Package a built wiimc.dol into the public release assets.
#
#   bash tools/package-release.sh <ca-bundle.pem> [output-dir]
#
# The asset names never change from one version to the next, so
# https://github.com/RyoSaeba89/WiiMC-GCN-Online/releases/latest/download/<name>
# always resolves to the newest release. The version travels inside the
# archive, in VERSION, and in the release tag.
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

ca=${1:?usage: package-release.sh <ca-bundle.pem> [output-dir]}
out=${2:-dist}
version=$(tr -d ' \r\n' < VERSION)
[[ $version =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Invalid VERSION '$version'" >&2; exit 1; }
[ -f wiimc.dol ] || { echo "Build wiimc.dol before packaging." >&2; exit 1; }
grep -q -- '-----BEGIN CERTIFICATE-----' "$ca" || { echo "$ca holds no certificates." >&2; exit 1; }

# The archive is an explicit allowlist, laid out like the SD card root.
# Never package a prepared SD: it holds the owner's WebDAV credentials and
# private TLS seed. Each installation generates its own seed.
files=(
    "wiimc.dol:wiimc.dol"
    "VERSION:VERSION"
    "README.md:README.md"
    "CHANGELOG.md:CHANGELOG.md"
    "HARDWARE_TEST.md:HARDWARE_TEST.md"
    "PORTING.md:PORTING.md"
    "LICENSE:LICENSE"
    "vendor/prefix/LICENSE-mbedtls:LICENSE-mbedtls"
    "$ca:apps/wiimc/ca.pem"
    "examples/onlinemedia.xml:apps/wiimc/onlinemedia.xml"
    "examples/webdav.conf.example:apps/wiimc/webdav.conf.example"
    "examples/onlinemedia.xml:examples/onlinemedia.xml"
    "examples/webdav.conf.example:examples/webdav.conf.example"
    "tools/prepare-sd.ps1:tools/prepare-sd.ps1"
)

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
for pair in "${files[@]}"; do
    src=${pair%%:*} dst=${pair#*:}
    [ -f "$src" ] || { echo "Missing release file: $src" >&2; exit 1; }
    mkdir -p "$stage/$(dirname "$dst")"
    cp "$src" "$stage/$dst"
done

# Belt and braces: nothing private may reach the archive whatever the list says.
if find "$stage" \( -name webdav.conf -o -name tls-seed.bin -o -name settings.xml \) | grep -q .; then
    echo "Refusing to package a private file." >&2; exit 1
fi

mkdir -p "$out"
out=$(cd "$out" && pwd)
rm -f "$out/WiiMC-GCN-Online.zip"
(cd "$stage" && TZ=UTC find . -exec touch -t 202601010000 {} + &&
    find . -type f | LC_ALL=C sort | sed 's|^\./||' | zip -q -X -9 "$out/WiiMC-GCN-Online.zip" -@)
cp wiimc.dol "$out/wiimc.dol"
(cd "$out" && sha256sum wiimc.dol WiiMC-GCN-Online.zip > SHA256SUMS.txt)
echo "WiiMC-GCN-Online $version"
ls -l "$out"
