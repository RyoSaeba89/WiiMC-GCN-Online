# Project memory

Last updated: 2026-09-18.

## Objective and current state

WiiMC-GCN-Online is now a GameCube music player with wired DHCP networking,
Web radio over HTTP/HTTPS, and read-only LAN WebDAV. The current source builds
successfully for GameCube and the host online suite passes 133 requests.
Build `20260918-134405` is installed on the SD, with matching SHA-256, the
previous DOL preserved and its ELF archived in `deployed/`. It combines
Opus/AAC format selection fixes with the smaller, bitrate-based WebDAV cache.
The user confirmed "ok tout fonctionne" on 2026-09-18 and requested release
1.0.0 on their personal GitHub, `RyoSaeba89/WiiMC-GCN-Online`. This accepts the
reported Opus, AAC 64 and WebDAV fixes on their console. Release the exact
tested binary; `VERSION` and the Git tag carry the fork's 1.0.0 version while
the binary retains its upstream 3.0.0 label/settings format.

No credentials belong in Git. The real WebDAV configuration is
`private/webdav.conf`; `.gitignore` also excludes every deployed
`webdav.conf` and the complete `sd-package/` directory.

## Decisions that must be preserved

1. TLS remains TLS 1.2 with required CA-chain, signature and host-name
   verification. `MBEDTLS_HAVE_TIME_DATE` is intentionally absent because the
   GameCube RTC cannot be trusted after battery loss. This matches GCRadio's
   effective configuration; do not describe it as using an old TLS protocol.
2. Boot is intentionally blocked by the English progress message until DHCP
   yields a usable IP. There is no Return button and DHCP retries indefinitely.
3. WebDAV stays HTTP-only because credentials are private LAN credentials and
   the current client explicitly refuses authenticated HTTPS.
4. Playback has priority over WebDAV metadata. On 2026-09-18 the user explicitly
   rejected the 2 MiB/50% prefill in favour of quick starts and track changes.
   Use a single 512 KiB circular cache for the current track: 16 KiB for probing,
   then about three seconds at the decoder's compressed bitrate after tag
   seeks. Keep cache-thread priority 70, the 60% metadata gate and pinned root.
5. A new directory is allowed to display slowly. Audible playback is never to
   be sacrificed to make browsing feel faster.

## Important implementation facts

- The old priority 45 was WRONG: public libogc2 LWP priorities increase with
  importance. Cache is now 70, above MPlayer 68 and GUI 60, with a 64 KiB
  stack for network/TLS calls. Prefill explicitly sleeps, supports cancellation
  and fails after 30 seconds without progress. Cache teardown waits for the
  producer before freeing its stream, including on GUI track changes.
- The actual public-radio TLS failure was missing `MBEDTLS_SHA384_C`.
  GCRadio's mbedTLS 2.x bundled SHA-384 under SHA-512; mbedTLS 3.6 does not.
  Reproduced host errors on NightRide (flags 0x804) and SomaFM (0x8), then
  verified both real HTTPS audio endpoints after enabling SHA-384, with a
  simulated 2000 RTC. Keep the independent SHA-384 certificate regression.
- `gc_tls.c` also masks only EXPIRED/FUTURE flags, as GCRadio does, and logs
  the real handshake code. Verify result 0xffffffff means verification has
  not completed, not a certificate rejection.
- TLS build scripts fingerprint configuration/recipe and the ELF depends on
  the TLS archives, preventing stale configuration from surviving a rebuild.
- The metadata gate starts only after an actual playback cache is ready;
  opening a probe/playlist/artwork file must never arm an unfulfillable wait.
- Host tests: `tools/test-cache.sh` runs the actual GEKKO cache with a pthread
  worker; `tools/test-tls.sh` runs the actual TLS client with local certificates
  and broken clocks, both with and without mbedTLS date checking compiled in.
- `source/mplayer/stream/cache2.c` previously printed a cache size but had the
  GameCube buffer allocation commented out. It now uses aligned heap memory
  and frees it at cache teardown.
- `source/mplayer/mplayer.c` treats inability to allocate the WebDAV playback
  buffer as a playback error instead of silently continuing without caching.
- `source/utils/webdav_device.c` owns the directory LRU and audio gate.
- `source/utils/webdav_client.c` invokes a wait callback before PROPFIND, each
  response-body read and periodically during extraction. Duplicate-name
  detection uses a bounded hash table rather than an O(n^2) scan.
- WebDAV hang evidence: `logs/sd-BB8E7A8F6D72-wiimc.log`, build
  `20260917-204103`. WebDAV reaches 50.2% prefill at 184.520 seconds and the
  MP3 decoder starts, but audio and GUI then hang in GX. The matching ELF
  resolves MPlayer's `8006ef50` to `DrawMPlayer` / `GX_WaitDrawDone`, and
  GUI's `80024d2c` to `Menu_Render` / `GX_DrawDone`. `low_cache_loop` was
  rendering video unconditionally while audio-only playback gives GX to the
  GUI. MP3 end-tag probing can discard prefill and trigger this wait at start.
- The 2026-09-18 fix guards rebuffer rendering with a configured video stream
  and lets audio-only playback sleep 20 ms while the producer refills. The
  paused-quit screenshot has the same video guard. Rebuffer entry/exit is
  logged. The combined fixes were accepted in build `20260918-134405`.
- Earlier failing SD run: `logs/sd-ED0F4C8CD5AD-wiimc.log`, build
  `20260918-082310`. User heard unusable Opus crackle and reported the SomaFM
  AAC 64 startup freeze; WebDAV had not yet been retested in that run. NightRide Ogg is
  identified as libopus by lavf, but MPlayer then selects `[pcm]`, s16le.
  `CODEC_ID_OPUS` had no MPlayer tag and no codecs.conf entry, so tag zero
  selected raw PCM. Add the internal `Opus` tag and `ffopus` / `libopus`
  entry as required by `source/mplayer/DOCS/tech/codecs.conf.txt`.
- SomaFM `vaporwaves-64-aac` returns HTTP 200, audio/aac at 77 seconds,
  then stays in format detection until exit at 146 seconds. ELF addresses
  `800ddb24`, `800a8614`, `80091410` resolve to cache_read,
  demux_mpg_fill_buffer and ds_fill_buffer. AAC was missing from lavf's
  preferred list while the native AAC demuxer was disabled. Accepting `aac`
  in that list avoids the long MPEG video scans of the live stream.
- The updated source contains both radio selection fixes. Restored
  host generation of codecs.conf.h (using target endian definitions); fixed
  the pre-existing three-character `flv` tag that prevented regeneration.
  The generator is a host `.exe`, independent of target EXESUF `.elf`.
  Reference docs: MPlayer `DOCS/tech/codecs.conf.txt`, FFmpeg 0.11 demuxing
  (`https://ffmpeg.org/doxygen/0.11/group__lavf__decoding.html`), and the
  Opus decoder API (`https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html`).
- WebDAV startup targets are now 48,000 bytes at 128 kbit/s and 120,000 bytes
  at 320 kbit/s, with a 16 KiB floor and 256 KiB ceiling for other formats.
  Unknown bitrate uses 120,000 bytes. `stream_cache_prefill` waits at the actual
  post-probe read position without reallocating or discarding the ring. EOF
  releases short tracks; stop/cancel and the 30-second stall deadline remain.
  The low-water threshold is one third of the startup target; recovery uses
  the same three-second target. Empty-cache recovery must wait, not resume
  simply because progress rounds to zero. Only after prefill does WebDAV
  arm its metadata gate. There is no whole-file or playlist audio preload,
  and no gapless/next-track decoder prefetch.

## Verification state

- PASS: host HTTP/WebDAV/ICY suite, 133 requests.
- PASS: actual GEKKO cache host tests: legacy 2 MiB/50% plus 512 KiB rings,
  three-second targets at 64/128/192/256/320 kbit/s after end-tag seeks,
  wrap, bounded refill with backward history, EOF/short tracks, one-second
  input gap, stop/next-track and cancellation.
- PASS: `tools/test-playback-wait.sh` compiles the actual MPlayer rebuffer
  loop and checks audio refill/resume without GX, stale/unconfigured video
  outputs, stop, EOF and configured video. The original loop fails this
  regression at its illegal audio-only `DrawMPlayer` call.
- PASS: `tools/test-radio-formats.sh` runs the production tag lookup,
  built-in codec selection, FFmpeg ADTS probe and lavf preference decision.
  Untagged Ogg Opus selects libopus, real PCM/AAC retain their decoders,
  extensionless AAC is preferred before MPEG probes, invalid data is rejected.
  Both original selections fail these regression cases. This tests format
  selection; the user subsequently accepted audible output on the console.
- PASS: 28 local TLS cases (two builds, two broken RTC values, seven
  certificate/protocol scenarios), including SHA-384 and invalid signatures.
- PASS: actual host TLS/HTTP client receives live NightRide Opus and SomaFM
  MP3 bytes with a simulated year-2000 RTC and the SD package CA bundle.
- PASS: complete devkitPPC/libogc2 GameCube build.
- PASS: linked ELF contains `mbedtls_sha384_info`, `verify_without_rtc` and
  `CacheThreadAvailable`; no linked `mbedtls_x509_time_gmtime` symbol.
- PASS, USER-REPORTED HARDWARE ACCEPTANCE: build `20260918-134405`, 2026-09-18.
  Keep `HARDWARE_TEST.md` for future regressions. Do not claim separately
  recorded timings, every adapter, or individual checklist steps from the
  user's general acceptance message.

## Safe continuation

Build with `tools/build-mbedtls.sh` after any TLS configuration change, then
`tools/build-gc.sh`. Test with `tools/test-online.sh`. Deploy only through
`tools/prepare-sd.ps1`, which preserves the previous DOL and archives logs and
the matching ELF. Never commit the private WebDAV file or paste its contents
into logs, documentation or chat output.
