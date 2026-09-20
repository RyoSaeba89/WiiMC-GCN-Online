# Changelog

## Unreleased

### Fixed

- Continuous, Shuffle, Loop and Through did nothing after the first song when
  it had been started from the file browser: the automatic path walked the
  playlist, which is empty until the playlist button is used, so MPlayer was
  told there was no next song and parked. The folder the song was started from
  now serves as the queue when there is no playlist, and the Next button works
  in that state too.

## 1.0.0 - 2026-09-18

First stable release of WiiMC-GCN-Online, based on WiiMC-GCN / WiiMC-SS 3.0.0.
The release uses the exact console-tested build `20260918-134405`.

### Added

- Wired DHCP networking using libogc2, with a startup screen until the console
  has an IP address, plus adapter/IP information in the credits screen.
- HTTP/HTTPS radio playback with MP3, AAC and Opus, ICY metadata, and a supplied
  station list adapted from GCRadio.
- Read-only WebDAV browsing and music playback on a trusted HTTP LAN share.
- A 512 KiB circular buffer for the current WebDAV track, with approximately
  three seconds of compressed audio prefetched after format/tag detection.
  Read-ahead continues in the background. This is not a whole-track download.
- Audio priority during directory browsing and a bounded directory cache with
  a pinned root listing.
- SD preparation, repeatable builds, host regression tests and diagnostic logs.

### Fixed

- Ogg Opus incorrectly selecting raw PCM, which produced inaudible crackling.
- Low-bitrate SomaFM AAC spending startup in MPEG video format probes.
- Audio-only WebDAV buffering entering video rendering and hanging the GUI.
- Empty-cache refill, EOF recovery, cancellation and track-change cache lifetime.
- HTTPS certificate chains needing SHA-384 support.

### Validation

The maintainer confirmed the corrected build works on the GameCube on
2026-09-18, including the reported Opus, SomaFM AAC 64 and WebDAV problems.
Automated checks cover 133 HTTP/WebDAV/ICY requests, the production cache and
audio wait loop, radio codec/demux selection, and 28 local TLS scenarios.

### Installation and scope

Use the release ZIP and follow the README. Each installation generates its own
TLS seed; private WebDAV settings and seeds are never shipped in the archive.
HTTPS verifies certificate chains, signatures and host names but ignores
calendar validity because the console clock may reset. WebDAV uses HTTP on a
trusted LAN. Track transitions are supported; sample-accurate gapless playback
is not implemented. See README for setup, controls and the security model.

The fork release number is `1.0.0`. The tested binary retains the upstream
`3.0.0` label/settings format; it has not been rebuilt merely to change a label.
