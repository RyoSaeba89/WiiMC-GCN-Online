# Changelog

## 1.1.0 - 2026-09-27

Confirmed on the maintainer's GameCube on 2026-09-27 with build
`20260923-210855`: WebDAV playback, including the automatic change to the
next song, and Web radio both work. The release binary is rebuilt from the
same source by the new GitHub Actions pipeline.

### Added

- Continuous, Shuffle, Loop and Through now work on a song started straight
  from the file browser: the folder it was started from serves as the queue
  when no playlist has been built. A playlist still takes priority.
- The audio bar's Previous, Next and play-order buttons are enabled again. An
  explicit Next advances even in Single mode.
- Optional static address: `netStaticIP`, `netStaticMask` and `netStaticGW` in
  `settings.xml` skip DHCP entirely. Leave `netStaticIP` empty to keep DHCP.

### Changed

- The startup network screen gives up after 30 seconds and offers Retry or
  Continue offline, instead of waiting for DHCP indefinitely. Settings are now
  loaded before the network starts, so local playback works without a network.
- Releases are built, tested and published by GitHub Actions from a version
  tag. Asset names are now fixed (`WiiMC-GCN-Online.zip`, `wiimc.dol`,
  `SHA256SUMS.txt`) so tools can find the newest version at
  `releases/latest/download/`; the version is in `VERSION` inside the archive.

### Fixed

- The WebDAV Buffering window remained over the second song after its
  startup prefill finished during automatic playback. Each cache prefill now
  closes its own progress window on completion or interruption.
- Continuous playback stopped after the first song started from the browser:
  MPlayer was told there was no next song and parked.
- Stopping or changing a WebDAV track now cancels its network request instead
  of waiting for transport timeouts. The WebDAV directory cache and error
  reporting are safe when the GUI and the cache thread use them together.
- The cache could stop refilling for good after reaching end of file, and its
  64-bit read/write positions could be read half-updated by the other thread.
  Both are fixed; the cache test suite, previously intermittent, now passes
  reliably.
- A reset while the TLS seed was being rewritten could leave an empty
  `tls-seed.bin` and disable HTTPS. The seed is now replaced atomically.
- Cancelling the network probe could leave its thread running unnoticed.
- Every wait for MPlayer to stop now gives up after ten seconds instead of
  freezing the menu.
- An out-of-bounds write in the USB Gecko log path, and a NULL `memset` when
  the MPlayer stack could not be allocated.

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
