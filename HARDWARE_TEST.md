# GameCube hardware acceptance test

Use the freshly prepared SD card. Do not edit or disclose
`apps/wiimc/webdav.conf` while collecting logs.
Current installed build: `20260923-210855` (2026-09-23). On 2026-09-27 the
maintainer confirmed that WebDAV playback, including the second song and its
Buffering window, and Web radio work on the console.

## 1.0.0 acceptance result

On 2026-09-18, the maintainer confirmed that everything works after testing
build `20260918-134405` on the GameCube. This closes the reported Opus crackle,
SomaFM AAC 64 startup freeze and WebDAV playback/buffering issues. That exact
binary is the 1.0.0 release artifact. The checklist below is retained for
future regression runs; individual timings and every step were not separately
reported in the acceptance message.

## 1. Network startup gate

1. Insert the SD card, connect the wired adapter and boot `wiimc.dol`.
2. Confirm that `Initializing network, please wait...` is shown in English.
3. Press `L`, `R`, `A` and `B` while it is visible. No menu action may occur.
4. Wait for DHCP. The dialog must disappear when an IP has been obtained.
5. Press `Z`. Confirm that the detected adapter and a real LAN IP are shown.
6. Optionally, boot with the cable unplugged. After 30 seconds a prompt must
   offer Retry and Continue offline; Continue offline must open the menu with
   SD playback available.

Pass condition: the first menu cannot be used before the IP exists or the
user chooses to continue offline, and it becomes usable automatically once an
address arrives. With `netStaticIP` set, no DHCP wait occurs.

## 2. HTTPS radio and RTC-independent TLS

The 2026-09-17 fix also enables SHA-384 explicitly for mbedTLS 3.6. Both
NightRide and SomaFM were failing on the host with the old configuration,
then delivered audio with the fix and a simulated year-2000 RTC. The console
test below still checks the actual adapter, PowerPC build and SD trust store.

1. Use `L` or `R` to select **Radio**.
2. Start one HTTPS SomaFM station.
3. Let it play for at least 30 seconds.
4. Return and start one HTTPS NightRide station.
5. Let it play for at least 30 seconds and confirm that title metadata changes
   when the station sends it.

Pass condition: neither station reports a certificate error even when the
GameCube date has reset. A host-name or CA error must still be reported as a
TLS certificate rejection.

Expected log line:

```text
tls: CA and hostname verified, dates disabled (GameCube RTC), TLS 1.2
```

For a regression run, check the previously failing stations first:

1. Start **NightRide FM (Opus)**. Listen for 30 seconds: music must be clear,
   with normal timing, and the menu must remain responsive. The log must say
   `Selected audio codec: [ffopus]` and `FFmpeg libopus audio`. Selecting
   `[pcm]` for this station is a failure, even if playback starts.
2. Switch to **SomaFM Vaporwaves AAC 64**. Confirm playback starts and the
   loading screen closes. Listen for 30 seconds, then stop or change station.
   The log must reach `libavformat file format detected`,
   `Selected audio codec: [ffaac]` and `Starting playback...`.
3. Switch Opus -> AAC 64 -> MP3 -> Opus. Each stream should be audible and
   stopping/changing stations must work. Preserve the SD log after the test.

The earlier log (`sd-ED0F4C8CD5AD-wiimc.log`) did not test WebDAV. It describes
the failing pre-release build, not the subsequently accepted 1.0.0 artifact.

## 3. WebDAV playback without audible cuts

The 2026-09-18 build fixes an audio-only GX hang after prefill. On build
`20260917-204103`, the SD log reached `Starting playback...` but MPlayer
and the GUI both waited for GX draw completion. Rebuffering now leaves
rendering to the GUI when there is no video stream.

1. Select **Music**, then open the configured **WebDAV** device.
2. Wait for the large root directory to appear; this first visit populates the
   pinned RAM snapshot.
3. Enter a folder and start a 256 or 320 kbit/s MP3.
4. Confirm playback starts after about three seconds of audio data has been
   prefetched (96,000 bytes at 256 kbit/s, 120,000 at 320 kbit/s). This is audio
   duration, not a mandatory three-second wall-clock delay. The 512 KiB ring
   continues filling in the background; it belongs only to the current track.
5. While it plays, press `B` until the large root is displayed.
6. Enter three different folders, including one not previously visited, and
   return to the root after each one.
7. Repeat the root/folder cycle ten times without pausing playback.
8. Continue listening for two minutes.

Pass condition: there is no audible silence, skip or repeated fragment. A new
folder may take longer to display while audio refills; the audio must win.
Returning to a visited folder, especially the large root, should be immediate.

Expected log lines include:

```text
WebDAV audio prefill: ... bytes, bitrate ... B/s, cache 512 KiB
webdav: 512 KiB playback cache ready (...% filled)
webdav: cache hit '/' (... entries, ... ms)
webdav: metadata waited ... ms for audio cache (...%)
```

The metadata-wait line appears only when navigation had to yield to playback.

First repeat the failing track in `2080 - The backup/` (`My Megadrive`).
Confirm the music becomes audible, its time advances, and the menu remains
responsive after initial buffering. Startup prefill now follows MP3 tag
probing so end-tag seeks do not discard it. If later network demand causes
a refill, the log must contain both `cache: rebuffering audio ...` and
`cache: rebuffer wait ended ...`, followed by audible playback. Also test
pause/resume and stop while paused before proceeding with the browsing test.

## 4. Cache lifetime and next track

1. Let one short MP3 end and advance automatically, then manually switch to
   another WebDAV MP3. Check the start delay in both cases, including a
   128 kbit/s track (48,000-byte target). The player does not promise gapless
   transitions: the next track still needs opening, probing and decoding.
2. Confirm the Buffering window disappears as soon as the second track
   begins to play, even if the last visible bar was incomplete. The window
   reports progress toward the startup threshold; the 512 KiB cache continues
   filling during playback.
3. Repeat two root/folder cycles during the second track.
4. Stop playback and browse one new directory.
5. Start another track, cancel while `Buffering...` is visible, then start it
   again. The application must return to the menu and allocate a fresh cache.

Pass condition: the second track receives its own three-second prefill and
the buffering window closes when it ends. The previous audio cache is released,
cached directories remain available for the application run, and metadata
navigation is released normally after playback closes.

The cache worker should appear at public LWP priority 70 with a 64 KiB stack
in the startup thread log. If a prefill cannot advance for 30 seconds, it must
report `cache: prefill stalled for 30 seconds` and leave playback rather than
waiting indefinitely. Preserve that log if it happens.

## 5. Log collection

1. Exit or power off only after the last action has completed.
2. Put the SD card back in the PC.
3. Preserve `wiimc.log`, `wiimc-prev.log` and any `wiimc-crash.txt` before the
   next deployment. `tools/prepare-sd.ps1` archives them automatically.
4. Record the build ID from the first lines of `wiimc.log`.

Failure evidence should include the exact menu, selected file, whether the
directory was new or cached, and the approximate time visible in the log.
