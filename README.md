# WiiMC-GCN-Online 1.1.0

WiiMC-GCN-Online turns the GameCube WiiMC port into a network-capable music
player. It retains SD playback and adds wired networking, Web radio, HTTPS and
a read-only WebDAV music library.

The project is based on WiiMC-SS 3.0.0 and the GameCube music-player work
described in the original [video backstory](https://youtu.be/-EpUi2d2_VI).

**Latest release:** [download `WiiMC-GCN-Online.zip`](https://github.com/RyoSaeba89/WiiMC-GCN-Online/releases/latest/download/WiiMC-GCN-Online.zip)
([all releases](https://github.com/RyoSaeba89/WiiMC-GCN-Online/releases)).
Version 1.1.0 was confirmed on a real GameCube on 2026-09-27 (build
`20260923-210855` from the same source): WebDAV playback with automatic advance
to the next song, and Web radio. See [CHANGELOG.md](CHANGELOG.md) for details.
The binary keeps the upstream 3.0.0 label/settings format; the fork version is
in `VERSION`.

Releases are built and published by GitHub Actions from a `vX.Y.Z` tag. The
asset names never change (`WiiMC-GCN-Online.zip`, `wiimc.dol`,
`SHA256SUMS.txt`), so `releases/latest/download/<name>` always points at the
newest version, and `VERSION` inside the archive names it.

## Current features

- Nintendo Broadband Adapter and the ETH2GC adapters supported by libogc2
  (W6100, W5500 and ENC28J60).
- An English startup screen while DHCP runs. It gives up after 30 seconds and
  offers Retry or Continue offline.
- Optional static address (`netStaticIP`, `netStaticMask`, `netStaticGW` in
  `settings.xml`), which skips DHCP entirely.
- Web radio from `apps/wiimc/onlinemedia.xml`, including HTTP and HTTPS MP3,
  AAC and Opus streams and ICY metadata.
- Read-only WebDAV browsing and MP3 playback over a trusted LAN.
- Continuous, Shuffle, Loop and Through play orders over the folder a song was
  started from, when no playlist has been built.
- A 512 KiB circular read-ahead cache for the current WebDAV track. Playback
  starts with about three seconds of compressed audio, estimated from its
  bitrate after format/tag probing; the worker keeps filling during playback.
- Audio-first WebDAV scheduling: uncached directory work waits for at least
  60% audio fill and yields between network chunks.
- A 1 MiB in-memory LRU cache of visited WebDAV directories. The root listing
  stays pinned, so returning to a large root does not issue another PROPFIND.
- A rotating TLS seed and a Mozilla CA bundle stored on the SD card.
- SD-card debug, crash and heartbeat logs for hardware diagnosis.

## SD-card layout

Download and extract `WiiMC-GCN-Online.zip` from the release page. From
the extracted directory, use PowerShell to install it onto your SD card
(replace `F:\` with the card's drive):

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\prepare-sd.ps1 -Destination 'F:\' -CaBundle '.\apps\wiimc\ca.pem'
```

This copies the player and radio list, preserves the previous DOL, and creates
a unique TLS seed using the computer's random generator. Run it once before
using HTTPS; copying the ZIP contents alone does not create that seed.
For an existing installation, the current seed is preserved.

For manual installation on Linux/macOS, copy `wiimc.dol` and `apps/wiimc/` to
the SD root. Generate a seed only if one does not already exist (replace the
example mount path):

```sh
test -f /media/SD/apps/wiimc/tls-seed.bin || openssl rand -out /media/SD/apps/wiimc/tls-seed.bin 64
```

Boot `wiimc.dol` with your GameCube homebrew loader. Connect the wired adapter
before launch; the menu opens once DHCP has assigned an address. If no address
arrives within 30 seconds, choose Retry or Continue offline. To skip DHCP, set
`netStaticIP`, `netStaticMask` and `netStaticGW` in `apps/wiimc/settings.xml`.

The application expects:

```text
wiimc.dol
apps/wiimc/
  ca.pem
  onlinemedia.xml
  tls-seed.bin
  webdav.conf          # private, optional
  settings.xml         # created by WiiMC
```

For WebDAV, copy `apps/wiimc/webdav.conf.example` to `apps/wiimc/webdav.conf`
on the SD and set the server URL, display name and optional credentials.
The URL must include the music directory and use HTTP on your trusted LAN.
Edit `apps/wiimc/onlinemedia.xml` to change the radio list.

When working from source, `private/webdav.conf` can instead be created from
`examples/webdav.conf.example`; the preparation script installs it for you.
Private files and deployed settings are ignored by Git. Never put credentials
in source files, documentation, examples or commits.

Prepare a reviewable package:

```powershell
pwsh -File tools/prepare-sd.ps1
```

Or install directly to an inserted card:

```powershell
pwsh -File tools/prepare-sd.ps1 -Destination 'F:\'
```

The script preserves an older `wiimc.dol`, installs the radio list and private
configuration, keeps the existing random seed, and archives the matching ELF.

## Build and automated test

The GameCube build uses devkitPPC and libogc2:

```sh
bash tools/build-mbedtls.sh
bash tools/build-gc.sh
```

The host transport suite runs a local HTTP/WebDAV/ICY server and exercises URL
validation, framing, redirects, cancellation, radio metadata, DAV parsing,
large directories, byte ranges and the metadata-priority callback:

```sh
bash tools/test-online.sh
bash tools/test-cache.sh
bash tools/test-playback-wait.sh
bash tools/test-radio-formats.sh
bash tools/test-tls.sh
```

The cache suite executes the GameCube ring-buffer code with delayed input,
prefill, seeks, EOF and track changes. The TLS suite executes the production
client against local certificates with clocks set to 2000 and 2084, including
SHA-384 signatures, expired/future dates and invalid trust/name/signatures.
It also checks a build with date checking enabled to exercise the date-only
verification callback. `build-gc.sh` automatically rebuilds TLS archives when
their configuration changes.

See [HARDWARE_TEST.md](HARDWARE_TEST.md) for the exact console acceptance test
and [PORTING.md](PORTING.md) for design decisions and hardware evidence.

## Security model

- HTTPS requires a valid signature chain rooted in `ca.pem` and a certificate
  name matching the requested host.
- Certificate validity dates are deliberately not checked. A GameCube with a
  flat RTC battery has no trustworthy date; this is also GCRadio's effective
  mbedTLS configuration. An expired certificate can therefore be accepted.
- WebDAV is deliberately restricted to plain HTTP and Basic authentication.
  Use it only on a trusted LAN; credentials and media are not encrypted.
- HTTPS-to-HTTP downgrades and authenticated cross-origin redirects are
  rejected.

## Controls

- `L` / `R`: change the main menu.
- `A`: open or play the selected item.
- `B`: go back one directory.
- `Z`: show credits, detected network adapter, IP address and free memory.
- During playback, hold `X`: left/right seek, `L` seeks to the beginning, `R`
  ends playback, and up/down toggle pause.

## Remaining validation boundary

The 1.0.0 and 1.1.0 playback fixes have been accepted on the maintainer's console.
The checklist in [HARDWARE_TEST.md](HARDWARE_TEST.md) remains available for
regression testing with other adapters, servers and streams; it does not imply
every hardware combination has been tested. Track changes open and buffer the
next file; sample-accurate gapless playback is not implemented.
`wiimc.log` records cache hits, audio-priority waits, TLS verification and the
IP that releases the startup screen. Logs can include local paths and addresses.
