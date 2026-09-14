# WiiMC-GCN-Online — porting notes

This fork adds two things to [SuperrSonic/WiiMC-GCN](https://github.com/SuperrSonic/WiiMC-GCN):
**WebDAV** as a browsable network device, and **web radio** playback over HTTP
and HTTPS. Both need something the upstream tree does not have — a working
network stack — and the whole of this document exists because that stack is not
missing from the tree so much as **switched off** in it.

The companion project is gcradio, an internet radio player for the same console.
Its `DOC.md` is referenced throughout by section number: it holds measurements
taken on real hardware that apply here unchanged, and this document does not
repeat them.

---

## 1. Decisions, taken before any code was written

| Question | Decision |
|---|---|
| Shape of WebDAV | A **devoptab**, mounted as a device, browsable in the file browser — not a play-this-URL stream module |
| Where configuration lives | WebDAV in a **dedicated text file** on the card; radio stations in WiiMC's own **`onlinemedia.xml`** |
| TLS scope | **HTTPS for radio streams only.** WebDAV stays plain HTTP on the LAN — see §6.3 |
| Build baseline | **Nothing has ever been built.** Phase 0 exists to fix that before anything else |
| Network adapter | **Both** a Broadband Adapter DOL-015 and an ETH2GC, so the two can be compared |
| Boot device | **SD Gecko**, memory card slot A or B — see §5.1 for which slot, and why it matters |
| WebDAV server | **Caddy** (`file_server` + `webdav`) as the reference implementation |

The project is written in English throughout — code, comments, commit messages
and documentation.

---

## 2. State of the upstream tree

SuperrSonic's port turned WiiMC into a GameCube music player. Part of that was
removing the network, since the console has none by default and WiiMC's network
code is large. What matters for this fork is **how** it was removed: almost
everywhere, by commenting out rather than deleting.

### 2.1 What is switched off

| Location | State |
|---|---|
| `Makefile.gc:47` | `LIBS` has no `-lbba`, so no network symbol resolves at link time |
| `Makefile.gc:35` | `ENABLE_SMB = 0` — though `libtinysmb.a` is present in libogc2 |
| `source/networkop.cpp:47` | the body of `netcb()` is under `#if 0` |
| `source/networkop.cpp:214,233` | `ConnectShare()` and `ConnectFTP()` are hollowed out the same way |
| `source/mplayer/Makefile:192-209` | the whole `SRCS_COMMON-$(NETWORKING)` list is dead — see §2.3 |
| `source/mplayer/config.h:318` | `#undef CONFIG_NETWORKING` |
| `source/mplayer/config.h:1335-1337` | FFmpeg's `http`, `httpproxy` and `https` protocols are all `0` |
| `source/menu.h:68-71` | `MENU_SETTINGS_ONLINEMEDIA`, `_NETWORK`, `_NETWORK_SMB`, `_NETWORK_FTP` commented out of the enum |
| `source/menu.cpp:8682-8689` | the matching cases are commented out of the menu dispatcher |
| `source/wiimc.cpp:1230-1241` | every `AddMem2Area()` call is under `#if 0` |
| `source/mplayer/stream/http.c:918` | `is_icy` is hard-wired to `0`, disabling ICY metadata |

### 2.2 What survives intact

This is the part that sets the size of the job.

- **The network settings GUI.** `MenuSettingsNetwork()`, `MenuSettingsNetworkSMB()`
  and `MenuSettingsNetworkFTP()` are whole and untouched at `source/menu.cpp:6272-6860`
  — roughly 600 lines of working pages, disconnected only at the dispatcher.
- **The device model.** `DEVICE_SMB` and `DEVICE_INTERNET` are still in the enum
  at `source/wiimc.h:31-32`, *including* on the `HW_DOL` side, and
  `FindDevice()` still routes protocol URLs to `DEVICE_INTERNET`
  (`source/fileop.cpp:1082`).
- **The Online Media framework.** `browserOnlineMedia`, `ParseOnlineMedia()`
  (`source/fileop.cpp:2528`), `IsAllowedProtocol()`, `IsInternetStream()`, and
  the m3u / pls / asx / plx / ram / smil playlist parsers.
- **The GEKKO socket shims.** `source/mplayer/stream/network.h:43-53` already
  maps `socket`, `recv`, `send`, `select`, `connect`, `bind`, `setsockopt` and
  `gethostbyname` onto their `net_*` equivalents.
- **The cache.** MPlayer's cache thread is still created
  (`source/wiimc.cpp:1261`), `stream/cache2.c` is still compiled, and
  `CheckMplayerNetwork()` (`source/networkop.cpp:150`) exists as the hook
  `cache2.c` is meant to call.
- **The settings file plumbing.** `SMBSettings smbConf[MAX_SHARES]` and
  `FTPSettings ftpConf[MAX_SHARES]` are still in `struct SWiiSettings`
  (`source/settings.h`), and `libmxml` is still linked (`Makefile.gc:49`) for
  `settings.xml`.

**There is therefore no network port to write. There is a network to switch
back on, plus two things this tree has never contained.**

### 2.3 More is switched off than it looks, and it is an accident of `make`

`SRCS_COMMON-$(NETWORKING)` at `source/mplayer/Makefile:192-209` reads as a
backslash-continued list in which two entries — `stream/http.c` and
`stream/network.c` — carry a leading `#`. That is not how `make` parses it. A
`#` starts a comment that runs to the end of the **logical** line, and
backslash continuations are joined *before* comments are stripped. The first
hashed entry is `stream/asf_mmst_streaming.c`, on the second line of the list.

So everything from there down is gone, not just the two that were meant to be:
`cookies.c`, `http.c`, `network.c`, `pnm.c`, `rtp.c`, `udp.c`, `tcp.c`,
`stream_rtp.c`, `stream_udp.c`, `librtsp/` and all of `realrtsp/`. Only
`stream/stream_netstream.c`, on the first line, survives.

Confirmed against a completed build: of the objects under `stream/`, only
`cache2.o`, `open.o`, `stream.o`, `stream_bd.o`, `stream_cue.o`,
`stream_ffmpeg.o`, `stream_file.o`, `stream_mf.o`, `stream_netstream.o`,
`stream_null.o` and `url.o` exist. There is no `tcp.o`, no `udp.o`, no
`http.o`, no `network.o`.

Phase 1 therefore has to rewrite this block rather than un-comment two lines,
and it should put the comments on lines of their own. The RTSP and RealMedia
entries stay out deliberately — they were never wanted here and their absence
is the one thing the accident got right.

### 2.4 And `stream/http.c` is switched off from the inside as well

Found by a hardware run, not by reading (§11.16). Putting the file back in the
build is not enough: **six of its functions have their bodies commented out**,
each with a failing `return` left in front. The file compiles, registers
`stream_info_http1` in `auto_open_streams[]`, links with no warning and no
missing symbol — and answers "I cannot open that" to every URL.

| Function | What was left | What it should do |
|---|---|---|
| `open_s1`, `open_s2` | `return 0;//fixup_open(stream, seekable);` | `STREAM_ERROR` **is** 0, so this is an unconditional failure |
| `fixup_open` | `return 0;` before everything | pick the ICY or the plain-HTTP reader |
| `nop_streaming_start` | `return -1;` before everything | send the request, read the response, follow redirects |
| `scast_streaming_start` | the whole ICY path commented | set up `Icy-MetaInt` de-interleaving |
| `http_streaming_start` | `URL_t *url = NULL;` and, at `out:`, `stream->fd = fd;` commented | work on the stream's URL and hand the socket back |

Nothing about a build can catch this: no symbol is missing, so the link closes.
Only running it does. The git history holds one commit for this file
(`8c25c17`, the import), so it arrived in this state and there is no clean
copy to restore from — the commented-out originals are the copy, and phase 1
puts them back verbatim.

The same hand disabled `closesocket`:

```c
//#define closesocket(a) net_close(a)
#define closesocket(a) 0
```

Reasonable while nothing defined `net_close`, and quietly expensive now:
libogc2's `FD_SETSIZE` is **16** (`network.h:160`), `http.c` calls
`closesocket()` on every failed open, and the GUI retries a failed stream in a
loop. A dozen retries and the socket table is gone — including for the
resolver's own UDP socket.

---

## 3. The two real gaps

### 3.1 There is no DNS resolver on the GameCube

Verified on this machine, not assumed:

```
$ powerpc-eabi-nm --defined-only libogc2/gamecube/lib/libbba.a
  T if_config
  T net_recv
  T net_select
  T net_socket
  ...
$ powerpc-eabi-nm --defined-only libogc2/gamecube/lib/libogc.a | grep gethostbyname
  (nothing)
```

`network.h:310` declares `net_gethostbyname()` on the cube side, but **no
library defines it there** — only the Wii build does. Meanwhile
`stream/network.h:47` does `#define gethostbyname(a) net_gethostbyname(a)`. So
the moment `stream/network.c` goes back into the build, the link breaks. This
is the same wall gcradio hit; see its `DOC.md` §12.

The fix is gcradio's `source/dns.c` (267 lines): a minimum A-record query over
UDP/53 through `net_sendto`/`net_recvfrom`, three attempts of two seconds via
`net_select`, no IPv6, no search suffixes, no `resolv.conf`. It queries the
gateway — which `if_config()` hands back as an output parameter and which every
home router relays — unless a server is configured explicitly. Its cache is
flushed on connection failure rather than expired by TTL, because a console
whose clock battery has died has no reliable time.

It arrives here essentially unchanged; what changes is one `#define` in
`stream/network.h`.

### 3.2 There is no TLS anywhere in this tree

MPlayer has no notion of TLS, and the bundled FFmpeg is configured without any
protocol at all. gcradio's answer transfers directly:

- `source/tls.c` (466 lines), `source/mbedtls_config.h` (113 lines) and
  `tools/build-mbedtls.sh`, cross-building **mbedTLS 2.28 LTS** — TLS 1.2 only,
  because 2.28 predates 1.3.
- The insertion point here is a single function: `connect2Server_with_af()` in
  `source/mplayer/stream/tcp.c`. gcradio's design principle applies unchanged —
  wrap the socket in `conn_recv()` / `conn_send()` so that nothing above the
  transport learns about TLS.
- Measured cost in gcradio: **+193 KB of `.dol`** after `--gc-sections`, which
  means the library must be built with `-ffunction-sections` and the link must
  carry `--gc-sections`.
- The trust store is a PEM bundle read from the card, not baked into the binary,
  so an expired root can be replaced without a rebuild. Mozilla's list as curl
  publishes it is 186 KB and 119 roots; parsing all of them costs time at
  start-up on a 485 MHz CPU, and trimming it is a text-editor job.

See gcradio `DOC.md` §9a for the two things mbedTLS cannot get for itself on
this machine — a trustworthy date, and entropy — and §6.3 below for why the
second one decides the WebDAV design.

---

## 4. Transport arithmetic

Reproduced from gcradio `DOC.md` §10 because it constrains every decision here.

The Broadband Adapter has a **4 KiB receive buffer**, which pins lwIP's
`TCP_WND` at `2*TCP_MSS` = **2920 bytes**. The ENC28J60 behind an ETH2GC splits
its 8 KB of on-chip buffer down the middle, giving **exactly the same 4 KiB** —
so there is one limit to track, not two.

A TCP connection cannot exceed `window / RTT`:

| RTT | Ceiling |
|---|---|
| 1 ms (LAN) | ~2.9 MB/s, in practice bounded by EXI at 27 Mbit/s ≈ 3.4 MB/s |
| 25 ms | 117 KB/s |
| 73 ms | 40 KB/s — 320 kbps exactly |
| 180 ms | 16 KB/s — 128 kbps and nothing more |

**Do not raise `TCP_WND`.** Announcing a window larger than the adapter's buffer
overflows it on real hardware.

What this means for the two features:

- **Web radio** at 128–320 kbps fits comfortably out to roughly 73 ms of RTT.
- **WebDAV on the LAN** has ample headroom for MP3 and for FLAC (~1 Mbps).
  Video would not fit, but WiiMC-GCN is an audio player, so the question does
  not arise.

There is a second-order effect worth remembering when a dropout is being
diagnosed: at 2920 bytes the window is exactly two segments, and TCP's fast
retransmit needs three duplicate ACKs to arm — so it never can. Every lost
packet costs a full retransmission timeout.

---

## 5. Hardware

### 5.1 The EXI map

| Device | EXI channel | EXI device |
|---|---|---|
| Memory card slot A, SD Gecko in slot A | 0 | 0 |
| **Broadband Adapter DOL-015** (Serial Port 1) | **0** | **2** |
| Memory card slot B, SD Gecko in slot B | 1 | 0 |
| SD2SP2, **ETH2GC** (Serial Port 2) | 2 | 0 |

Two consequences:

- **An ETH2GC and an SD2SP2 cannot coexist.** Both are Serial Port 2.
- **A Broadband Adapter shares EXI channel 0 with an SD Gecko in slot A.**
  Different devices on the same channel is legal and libogc arbitrates it, but
  it is a shared bus, and **slot B (channel 1) is the isolated one.** Since this
  fork boots from an SD Gecko, **slot B is the recommended slot** — and it is
  also the one the existing code reaches first: `source/fileop.cpp:890-903`
  probes `__io_gcode`, then `__io_gcsd2`, then `__io_gcsdb`, then `__io_gcsda`.

### 5.2 Storage, and one thing this tree does not do

WiiMC-GCN mounts exactly **one** volume, as `sd1:`, from the first of those four
interfaces that answers. It does so through **libfat** (`fatMountSimple`), not
through `fatInitDefault()` and libdvm.

That is worth knowing but is out of scope here: it means no exFAT and no
partition prober, so cards above 32 GB sold pre-formatted read as empty. See
gcradio `DOC.md` §9b for what libdvm would change. **This fork does not touch
the storage layer.**

Both the configuration file and the CA bundle therefore live next to
`settings.xml` in `sd1:/apps/wiimc/`, and there is no device search order to
implement — unlike gcradio, which has `storage.h` for exactly that reason.

### 5.3 Telling the two adapters apart on screen

libogc2's `if_configex()` tries `bba_init`, then WIZnet W6100, then W5500, then
ENC28J60, and keeps the first that answers; `enc28j60if_init()` probes Serial
Port 1, then Serial Port 2, then the two card slots. **None of that is our
code**, which is why ETH2GC support costs nothing — but it also means the
program does not otherwise know which adapter it got.

gcradio reads it back with `if_indextoname(1, ...)`: every driver stamps the
netif with two letters, the chip (`en` DOL-015, `E` ENC28J60, `W` W5500,
`w` W6100) and the port (`1`, `2`, `A`, `B`). Port that here. Without it,
"no network" and "found the wrong adapter" look identical from the couch — and
this fork is explicitly meant to be tested on both.

### 5.4 What the pad does in the menus

Asked on 2026-09-13, because most buttons appear dead. They are: the menus bind
six things and nothing else.

| Button | In a menu |
|---|---|
| D-pad / stick | move the selection |
| A | select, open a folder, play a file |
| B | up one level (`upOneLevelBtn`) |
| Y | add to playlist (`trigPlus`, `menu.cpp`) |
| X | nothing — `playlistResetBtn.SetTrigger(NULL)`, deliberately disabled |
| Z | the credits screen, which also prints free memory (§11.9) |
| START | quit to the loader |

**The README's control list is for playback, not for menus** — hold X and
left/right to seek, L to the start, R to end playback, up/down to pause. None
of it does anything on the browser screens, which is what makes the pad feel
broken there.

Where the music comes from: the browser opens `WiiSettings.musicFolder`, and
there is no setting for it — `"Music Folder"` is commented out of
`MenuSettingsMusic()`, as `"Videos Folder"` is out of `MenuSettingsVideos()`. So
it is always empty, and an empty folder means the browser opens on the device
list instead: *SD* first, then the card with A, back up with B. Files go
anywhere on the card. (That list was empty until §11.10.)

There is no way to reach a network stream from this build: the transport is
still off (§2.1), web radio and WebDAV are phases 2 and 4 of §8, and
`MENU_BROWSE_ONLINEMEDIA` — the screen that would host them — is commented out
of the menu dispatcher in four places (`source/menu.cpp`).

---

## 6. Design

### 6.1 Web radio

The shortest path, because MPlayer already does this. `stream/network.c:240-241`
sends `Icy-MetaData: 1`, `stream/http.c:154` parses `StreamTitle`, and
`scast_streaming_start()` handles the de-interleaving. Three changes:

1. Add `https` to `validInternetProtocols` (`source/settings.h:247`, currently
   `{"http", "mms", ""}` — note the array is `[][5]`, so `https` at five
   characters plus its terminator needs the width widened).
2. Revert the forced `is_icy = 0` at `stream/http.c:918` to recover track
   titles.
3. Re-enable `MENU_BROWSE_ONLINEMEDIA` and its settings page.

Station lists go in `onlinemedia.xml`, whose parser and browser are already
written and merely hidden from the menu.

**The one genuine unknown:** WiiMC decodes through FFmpeg's `demux_lavf`, not
through minimp3 and libfaad as gcradio does. MP3 and AAC over a non-seekable
live stream are the well-trodden case; **Opus in Ogg on a live mount is not**,
and it needs to be proven early rather than assumed, because gcradio's
experience is that most Opus mounts are behind HTTPS and so depend on phase 3
as well.

### 6.2 WebDAV

WebDAV is HTTP with `PROPFIND` for listing and `GET` with `Range:` for partial
reads. Once phase 1 is done, the transport is free; what remains is a devoptab
on the model already present for SMB and FTP:

- `PROPFIND` with `Depth: 1`, parsed with **mxml** — already linked for
  `settings.xml`, so no new dependency.
- `GET` with `Range:` backing `read()` and `lseek()`, which is what makes
  seeking inside a track work.
- Basic authentication, in the clear, on the LAN only (§6.3).
- Registered so that `FindDevice()` and the file browser see it the way they see
  `sd1:` — which is the whole point of choosing a devoptab: `fileop.cpp`,
  the playlist parsers, the cover-art reader and the sort all keep working
  untouched.

Caddy is the reference server: its `PROPFIND` output uses the plain `D:`
namespace with no proprietary extensions, which makes it the right target to
get correct first. Nextcloud's `oc:`/`nc:` extensions and Apache's quirks are a
later conformance question, not a design one.

### 6.3 Why WebDAV stays on plain HTTP

gcradio `DOC.md` §9a is explicit about its own entropy source — the 40 MHz
timebase, the RTC, allocator addresses and scheduling jitter, mixed in
`mbedtls_hardware_poll()`:

> That is **not** a hardware RNG, and it should not be described as one. […] It
> would not be acceptable for anything carrying credentials, and nobody should
> reuse this file for something that does.

A radio stream over HTTPS sends no secret; the client authenticates the server
and that is the entire benefit. A WebDAV request sends `Authorization: Basic` on
every single call. Reusing `tls.c` for the second case is precisely what its
author told us not to do.

So: **HTTPS for radio, plain HTTP for WebDAV on a trusted LAN.** If WebDAV over
the internet becomes a requirement later, the answer is a real entropy source or
Digest authentication — a separate piece of work, not a flag.

---

## 7. Build environment

### 7.1 What is missing, and what that turned out to cost

The upstream README points at an external `portlibs_gcn.7z`. It is not needed:
every missing library is packaged in the **`libogc2-devkitpro`** pacman
repository that gcradio already requires.

Present on this machine: `ppc-freetype 2.14.3`, `ppc-libjpeg-turbo`,
`ppc-libpng`, `ppc-zlib`, `libogc2-git`, `libogc2-libdvm-git`, `libogc2-grrlib`.

Missing, and installable: **`ppc-mxml`**, **`ppc-libexif`**, **`ppc-libiconv`**.

### 7.2 What is stale in the tree

`source/mplayer/config.mak` was generated against an older libogc2 layout and
against its author's machine:

- `EXTRA_INC` points at `$(DEVKITPRO)/libogc2/include`; the current layout is
  `$(DEVKITPRO)/libogc2/gamecube/include`.
- `EXTRA_INC` also carries
  `$(DEVKITPPC)/../buildscripts/powerpc-eabi/gcc/gcc/include`, which exists on
  no standard install.
- `EXTRALIBS` points at `$(DEVKITPRO)/libogc2/lib/wii` — both the wrong layout
  and the wrong console. It is only consulted when linking an MPlayer binary,
  and this build produces `libmplayerwii.a` instead, but it should not be left
  wrong.

The README also warns that the `elf2dol` path was hard-coded and not reverted.
**No `elf2dol` reference exists anywhere in the tree**, so either it was fixed
before the last commit or the warning refers to a build step that no longer
exists. Treated as already resolved until a build says otherwise.

FFmpeg is built from inside this tree: `ffmpeg/config.mak` is a one-line
`include ../config.mak`, and FFmpeg's sources pick up MPlayer's combined
`config.h` through `-I.`. There is no separate FFmpeg `configure` step to run.

### 7.3 What actually broke, and what did not

The README's warning that "some freetype versions are incompatible" was the
risk this section was written around. **It did not materialise.**
`sub/font_load_ft.c` compiles clean against `ppc-freetype 2.14.3` — warnings
only, no errors. Freetype did cost something, but at the link rather than the
compile (§7.3.4 below).

What did break, in the order the build found it:

1. **`-I$(PORTLIBS)/include/freetype2` in `Makefile.gc:126`.** In the current
   libogc2, `PORTLIBS` is a **list** of three directories, not one
   (`gamecube_rules` sets it to `libogc2/gamecube`, `portlibs/gamecube`,
   `portlibs/ppc`). Appending `/include/freetype2` to a list yields one
   nonsense path, and `ft2build.h` is not found. The line above it already does
   the right thing with `$(foreach dir,$(LIBDIRS),...)`; this one now does too.
   Note in passing that `$(PORTLIBS_PATH)/gamecube` does not exist at all on a
   current install — harmless, since a `-I` at a missing directory costs
   nothing.

2. **`source/mplayer/config.mak` pointed at the old libogc2 layout** — §7.2.
   Three lines: `EXTRA_INC`, `CXXFLAGS` and `EXTRALIBS`. The stray
   `buildscripts` include from the author's machine went at the same time.

3. **mxml 3 made `mxml_node_t` opaque.** `source/settings.cpp:298` read
   `node->value.element.name` directly, which mxml 2 allowed. Replaced with
   `mxmlGetElement(node)`, the accessor — which exists from mxml 2.7 onward, so
   the file now compiles against either version. This is the **only** direct
   struct access in the tree; every other mxml call here
   (`mxmlDelete`, `mxmlElementGetAttr`, `mxmlElementSetAttr`, `mxmlFindElement`,
   `mxmlLoadString`, `mxmlNewElement`, `mxmlNewXML`, `mxmlSaveString`,
   `mxmlSetWrapMargin`) is public API unchanged in 3.x.

4. **`source/utils/pngu.c` had no `<string.h>`.** It uses `memcpy`, `memset`,
   `strlen` and `strcpy`, and used to get the declarations transitively through
   `png.h`. Current libpng no longer provides them, and on a modern GCC an
   implicit declaration is an error rather than a warning.

5. **freetype needs brotli and bzip2 at the link.** `ppc-freetype 2.14.3` is
   built with WOFF2 and bzip2 support, so `BrotliDecoderDecompress` and the
   three `BZ2_bzDecompress*` symbols come out undefined unless
   `-lbrotlidec -lbrotlicommon -lbz2` are on the link line. This is the same
   trap documented in gcradio's own Makefile for GRRLIB, and it fails *inside*
   freetype, which makes it read as a freetype problem rather than a missing
   library.

None of the five is specific to this fork: any attempt to build WiiMC-GCN on a
current devkitPro plus libogc2 hits all of them. That is worth knowing before
blaming anything we add later.

### 7.4 Building

From an MSYS2 shell with devkitPro's environment sourced:

```sh
pacman -S --needed ppc-mxml ppc-libexif ppc-libiconv
cd /c/Users/jacqu/Documents/WiiMC-GCN-Online
make -f Makefile.gc
```

`Makefile.gc` descends into `source/mplayer` first, so there is nothing to
build by hand beforehand. A cold build takes a few minutes and produces:

| File | Size |
|---|---|
| `source/mplayer/ffmpeg/libavcodec/libavcodec.a` | 6.4 MB |
| `source/mplayer/ffmpeg/libavformat/libavformat.a` | 2.9 MB |
| `source/mplayer/ffmpeg/libswscale/libswscale.a` | 1.3 MB |
| `source/mplayer/ffmpeg/libavutil/libavutil.a` | 0.7 MB |
| `source/mplayer/libmplayerwii.a` | 7.9 MB |
| **`wiimc.dol`** | **5.7 MB** |

That 5.7 MB is worth carrying into §9: it is loaded into the same 24 MB the
player then has to allocate out of.

**A build dirties tracked files.** Upstream committed 319 `.d` dependency files
and `source/mplayer/ffmpeg/version.h`, all of which the build regenerates — 165
of them on a full pass. They are build output that happens to be tracked, so
`.gitignore` cannot help. Reset them before committing:

```sh
git checkout -- '*.d' source/mplayer/ffmpeg/version.h
```

They are deliberately left tracked rather than removed: untracking 320 files
would bury every later diff against upstream under a deletion the fork does not
need to make.

---

## 8. Plan

Each phase ends with a criterion that can be checked on the console. A phase
that cannot be checked does not count as finished.

### Phase 0 — a reproducible baseline

Install `ppc-mxml`, `ppc-libexif`, `ppc-libiconv`; repair the stale paths in
`source/mplayer/config.mak`; build FFmpeg and `libmplayerwii.a`; build
`wiimc.dol`; boot it from the SD Gecko.

> **Done when** an unmodified `wiimc.dol` built here plays a file from the card
> on the console. Until then no regression can be attributed to anything.

**Status: done, on hardware (2026-09-14).** The five problems of §7.3 are fixed
and `make -f Makefile.gc` produces a 5.7 MB `wiimc.dol` with no errors. Getting
from that to a file playing took the log of §11 and five distinct faults
(§§11.8-11.13): the GUI thread's stack, the credits array, the SD interface
mismatch, the device thread clearing `isInserted`, and the `controlledbygui`
race. The run of §11.14 closes the phase — **fifteen files played one after the
other from the card, the browser used between each, no crash, memory flat.**
The baseline is reproducible, so a regression from here can be attributed.
**Phase 1 is open.**

### Phase 1 — switch the transport back on

`-lbba` in `Makefile.gc`; restore the body of `netcb()`; define
`CONFIG_NETWORKING`; put `stream/network.c` and `stream/http.c` back in the
MPlayer Makefile; point `gethostbyname` at the ported `dns.c`. Add the adapter
label of §5.3.

> **Done when** the console plays an `http://` stream from an Icecast on the
> LAN, and the screen names the adapter it found — on the DOL-015 and on the
> ETH2GC.

**The two halves of that criterion are tested by two different people.** Only
the DOL-015 is on this desk; the ETH2GC belongs to a friend of the author and
will be tested there, later and independently. So the phase closes in two
steps, and the DOL-015 half is the one that gates any further work.

What the ETH2GC tester should expect, so that a slow boot is not read as a
hang: `if_config()` walks the drivers in order (§5.3), and the DOL-015 probe
comes **first**. On a console with no BBA that probe has to fail before the
SPI chips are tried — measured at 5.1 s to fail on its own (§11.15) — on top
of whatever the ETH2GC then needs. The credits screen (Z) names the chip and
the port it answered on, and `sd1:/wiimc.log` holds the `net:` lines; those two
are the whole report, and they come back without the console.

**Status: the link is up in the tree, untested on hardware.** Split in two so
that an adapter problem and an MPlayer problem cannot arrive together.

*Lot 1 — bring the interface up (build `20260914-215005`).* Done:

- `Makefile.gc` gains `ENABLE_NETWORK` and links `-lbba`. It goes in `NETLIBS`,
  ahead of `-logc`, not appended like the other toggles: `libbba.a` is where
  `if_config`, `if_configex`, `if_indextoname`, `net_gethostip` and `net_init`
  live, and `libogc.a` defines none of them. Cost: the DOL goes from 5739360 to
  5830496 bytes, so the whole lwIP stack is 89 KB.
- `netcb()` has a GameCube body: `if_config()` with DHCP, three attempts a
  second apart, `wiiIP` filled, a `DebugMark` per attempt and on the result. It
  stays on its own thread because `if_config()` blocks for seconds -- longer
  when nothing answers and libogc2 walks four drivers across four ports -- and
  the menu has to stay drawn and cancellable meanwhile.
- `StartNetworkThread()` creates the thread again (it was commented out), and
  `wiimc.cpp` calls it at boot without waiting on it.
- `CheckMplayerNetwork()` tests `net_gethostip()` again instead of `if(1)`.
- `InitializeNetwork()` no longer spins on `LWP_ThreadIsSuspended(NULL)` when
  the thread was never created.
- The adapter label of §5.3, ported from gcradio's `adapter_label()`, on the
  credits screen (Z): `NET: Broadband Adapter, Serial Port 1 - 192.168.1.x`,
  or `NET: none`.

*Lot 2 — give MPlayer the transport (build `20260914-221604`).* In the tree,
untested on hardware:

- `source/utils/dns.c` and `dns.h`, gcradio's resolver (§3.1) unchanged, plus
  two things it did not need there: an `extern "C"` guard, and
  `wiimc_gethostbyname()`. MPlayer wants a `struct hostent *` back --
  `connect2Server_with_af()` reads `h_addr_list[0]` and `h_length`
  (`stream/tcp.c:163`) -- not the `u32` `dns_resolve()` hands out.
- `stream/network.h`: `#define gethostbyname(a)` points at that shim. This is
  the "one `#define`" §3.1 promised. Declared in place rather than included, so
  the MPlayer sub-make needs no extra `-I`.
- `config.h:318`: `#undef CONFIG_NETWORKING` becomes `#define`.
- `netcb()` hands the DHCP gateway to `dns_set_server()` and flushes the cache.
- The block of §2.3, rewritten one file per line with every comment on a line
  of its own.

Three things the link found that reading could not:

1. **`asf_streaming.c` and `asf_mmst_streaming.c` are not in this tree.**
   SuperrSonic dropped them. They were in the hashed part of the old list, so
   nothing had noticed. `&stream_info_asf` now sits under `!defined(GEKKO)` in
   `stream.c` beside the RTSP entries.
2. **`streamtitle`, `streamurl`, `streamname` and their three `_changed` flags
   were defined twice** -- in `stream/http.c` and again in `source/menu.cpp`,
   whose comments already said "(http.c)". They were `extern` declarations
   turned into definitions while http.c was out of the build, and `-fno-common`
   is the default now. They are `extern "C"` again. `streamname` mattered most:
   it was `static` in menu.cpp, so http.c would have raised the flag while the
   menu read its own empty buffer.
3. **Nothing in the GameCube menu reads those globals yet.** `menu.o` has no
   reference to any of them, and `--gc-sections` drops them from the ELF: the
   whole ICY display path is under `#if 0` at `menu.cpp:7690` and `:7740`. That
   is phase 2's "ICY titles restored", and it is now the only thing standing
   between a playing stream and a title on screen.

DOL: 5860992 bytes, so MPlayer's side of the transport is another 30 KB on top
of lwIP's 89 KB.

*Lot 3 — make `http.c` do something (build `20260914-223510`).* The first
hardware run of lot 2 (§11.16) showed the link, the playlist and the URL all
working and MPlayer still answering "Failed to open". §2.4 is why: six
functions in `stream/http.c` had their bodies commented out behind a failing
`return`, and `closesocket` was `#define`d to `0`. Both restored. **Nothing on
the network side of MPlayer has ever executed yet** -- the resolver,
`connect2Server` and the ICY reader are all still unproven.


### Phase 2 — web radio

`https` into `validInternetProtocols`; ICY titles restored; Online Media menu
re-enabled; stations in `onlinemedia.xml`. Prove Opus-in-Ogg on a live mount
here, or record that it does not work and why.

> **Done when** a station list on the card is browsable and playable, with track
> titles appearing in step with the audio.

### Phase 3 — TLS for radio

Port `tls.c`, `mbedtls_config.h` and `build-mbedtls.sh`; wrap the socket in
`connect2Server_with_af()`; read the CA bundle from `sd1:/apps/wiimc/`.

> **Done when** an `https://` station plays, and a failed handshake says why on
> screen rather than falling silent.

### Phase 4 — WebDAV

The devoptab of §6.2, its configuration file, and the browser integration.

> **Done when** a Caddy share is browsable on the console and a track plays from
> it with seeking working.

---

## 9. Risks

**RAM.** Every MEM2 area is disabled (`source/wiimc.cpp:1230`), so everything
goes through `malloc` on the ~22 MB MEM1 arena, and the upstream README already
warns that "mixing codecs can sometimes use up too much memory and crash". To
that we add lwIP's heap, MPlayer's stream cache, mbedTLS, and a CA bundle
parsed at start-up. It fits, but it has to be budgeted rather than assumed —
and trimming the PEM is the cheapest lever.

Note the asymmetry with gcradio, where `DOC.md` §10 can say "RAM is not a
constraint and never will be" at 1.8 MB of 24. That statement does not transfer
to this tree.

**Freetype.** §7.3. The one thing that can stop phase 0 dead.

**Opus over a live stream through `demux_lavf`.** §6.1. Unknown until tried;
worth trying before phase 3 is planned in detail, since it may be the main
reason to want TLS at all.

**Two adapters, two sets of behaviour.** The transport arithmetic is identical
(§4), but the drivers are not. Anything that works on one and not the other is a
libogc2 question, and the adapter label of §5.3 is what makes that distinction
visible at all.

---

## 10. What comes from gcradio

| File | Lines | Change expected |
|---|---|---|
| `source/dns.c` / `dns.h` | 267 / 35 | Essentially none; it is self-contained |
| `source/tls.c` / `tls.h` | 466 / 63 | The socket wrapper is re-pointed at MPlayer's `tcp.c` |
| `source/mbedtls_config.h` | 113 | None |
| `tools/build-mbedtls.sh` | — | Paths only |
| `adapter_label()` from `source/main.c` | ~30 | Rewritten against WiiMC's GUI rather than a text console |

`storage.h` is deliberately **not** ported: this tree mounts one volume as
`sd1:` and has no search order to express (§5.2).

---

## 11. The debug log

### 11.1 Why it exists

The first boot on hardware crashed and the screen showed no register dump. On
this console that is not unusual, it is the norm, because upstream left the
program with **no diagnostic channel at all**: `SaveLogToSD()` is under `#if 0`,
the stdout devoptab that fed the USB Gecko is under `#if 0`, and nobody boots
from Swiss with a Gecko attached anyway. Meanwhile most of the ways this
program can die are silent by construction:

| Death | What the screen shows |
|---|---|
| `exit()` or `main()` returning | back to the loader, nothing |
| `InitMPlayer()` failing — e.g. no `sd1:/apps/wiimc/` folder | one prompt, then `ExitRequested` and back to the loader (`source/menu.cpp`, `WiiMenu()`) |
| `InitFreeType()` failing | `return 0` from `main()`, back to the loader |
| a hang or deadlock | the last frame, frozen |
| a stack overflow | nothing, or an unrelated fault much later |
| `malloc` returning NULL | a DSI somewhere else, with the cause gone |
| a CPU exception | libogc's dump for **8 seconds**, then back to the loader |

The last row is worth knowing on its own: `__exception_setreload(8)` in
`main()` means a dump that is not photographed within eight seconds looks
exactly like every other row of this table.

### 11.2 What was built

`source/utils/debuglog.c`, switched on by `ENABLE_DEBUGLOG = 1` in
`Makefile.gc` (on by default for now). It is DKR-GC's
`platform/gc/gc_logfile.c` and `gc_crash.c` adapted to this tree; DKR-GC's
`PORTING.md` records the hardware runs that paid for each design rule, and
they are not repeated here beyond the one-line reasons in the source.

| Silent death | Instrument |
|---|---|
| CPU exception | `-Wl,--wrap=c_default_exceptionhandler`: a report written without printf into RAM, then to the card, then libogc's dump. `__exception_setreload(-1)` holds that dump until Z or RESET -- not `0`, which reloads at once (§11.7) |
| `exit`, `abort`, `assert` | wrapped at link time; the caller's address is logged and flushed before the real call |
| hang | a heartbeat thread at priority 100 every 2 s, and a dump of every thread's state and parked stack every 10 s |
| stack overflow | `LWP_CreateThread` wrapped: every stack is painted, its high-water mark reported, an overflow marked |
| out of memory | `malloc`, `calloc`, `realloc`, `memalign` wrapped: a NULL return is recorded with size and caller |
| MPlayer's own errors | stdout and stderr redirected into the log |

Plus breadcrumbs (`DebugMark`) through `main()`, the SD mount, the menu loop,
MPlayer initialisation and each file load.

One behaviour change comes with it: `usb_isgeckoalive(1)` is no longer called.
Channel 1 is memory card slot B, where §5.1 recommends the SD Gecko, and
DKR-GC's log card was ruined by Gecko traffic on exactly that slot. Nothing in
this build reads the result.

Confirmed in the linked ELF, not assumed: every call site of the nine wrapped
symbols goes to its `__wrap_` function; the single direct call left for each is
the wrapper's own `__real_` call.

**Untested off hardware.** Dolphin emulates no SD reader on the GameCube
(gcradio `DOC.md`, the Dolphin section), so the card side of this cannot run
anywhere but the console.

### 11.3 Where the files are

| File | Contents |
|---|---|
| `sd1:/wiimc.log` | this run |
| `sd1:/wiimc-prev.log` | the run before — **the one to read after a crash and a reboot** |
| `sd1:/wiimc-crash.txt` | the crash report of this run, if there was one — 24 KB, created and held open at boot |
| `sd1:/wiimc-crash-prev.txt` | the crash report of the run before |

At the root of the card that was mounted as `sd1:`, whatever the slot. If the
log says `sd: NOTHING mounted`, or neither file exists, the card was never
mounted and nothing after that point could be written.

The file is created at 256 KB at boot and overwritten in place, so it is mostly
blank lines in an editor. The body wraps under the boot header when full; the
newest line is just above `<<<<<<<< end of log >>>>>>>>`. The last 24 KB are
reserved, and a crash report, if any, is **at the very end of the file** as
well as in `wiimc-crash.txt`. A crash file that is 24 KB of blank lines and
nothing else is what "no report at all" looks like: the handler never ran.

### 11.4 Reading it

The shape of a run (the numbers are illustrative, not from a real one):

```
[    0.012] boot: log started; main thread stack 806f36c8 size 16384 sp 806f7580
[    0.310] boot: audio ok
...
[    3.004] sd: sd1: mounted via SD Gecko in slot B
[    4.871] boot: app path 'sd1:/apps/wiimc'
[    5.402] menu: 1
[    7.402] hb 1 | oom 0 | last [    5.402] menu: 1
            mem free 9120K (in heap 610K + never claimed 8510K), low 9120K, heap 4410K
            thr #0  entry 00000000 prio  64 state 00000000 stack 11204/16384 sp ... | 8001f2a4 ...
```

- **The last breadcrumb before the log stops** says how far the run got.
- **`hb` lines that keep coming while `last` stays put** is a hang with the
  scheduler alive: the thread dump shows which thread is parked where.
- **`hb` lines that stop** mean interrupts are off or the machine is dead; the
  last one written is the last moment it was alive.
- **`EXIT:`, `ABORT:`, `ASSERT:`** name the quiet deaths of §11.1 directly.
- **`OUT OF MEMORY`** lists each failing call site; `low` in the `mem` line is
  the least free memory seen, and on this 24 MB machine it is the number to
  watch (§9).
- **`STACK OVERFLOW`**, or a `stack used/size` close to its size: the 16 KB
  main thread runs the whole menu, and the GUI threads have fixed stacks too.

A `CRASH` block gives `srr0` (the faulting instruction), `lr`, `dar` (for a
DSI, the address that was refused), the breadcrumb, the faulting thread, all
32 registers, stack usage per thread, and the code addresses found on the
faulting stack. Every address resolves against **the ELF built with the
`.dol` that ran**:

```sh
powerpc-eabi-addr2line -f -C -e wiimc.elf 0x8001f2a4 0x80034ad0
```

Keep `wiimc.elf` from each build that goes onto the card; a rebuilt ELF gives
wrong answers without any warning.

### 11.5 Deployment to the card

Every `make -f Makefile.gc` ends with a `deploy` step, which on this machine
does three things when the SD card is plugged in as `F:`:

1. **Fetches the logs, then clears them off the card.** All four files of
   §11.3 are copied into `logs/`, named by their modification time, before
   anything on the card changes — and removed from the card once the copy is
   safe. The card therefore holds exactly what the last run wrote, and never an
   older run's file that could be read as this one's. The console has no clock
   battery, so those names carry its date, not ours; two runs of similar length
   land on the same name, so a file whose name is taken and whose contents
   differ is kept as `-2`, `-3`, and so on. Nothing is deleted that is not
   already in `logs/`.
2. **Writes `wiimc.dol`** to the root of the card and checks it back by MD5.
3. **Archives the ELF** as `deployed/wiimc-<build id>.elf`, keeping the last
   eight. The same build id is in the `build` line at the top of the log, so
   the ELF to hand to `addr2line` is never a guess.

A drive only counts as the card if `apps/wiimc/` already exists on it, so no
other removable drive on that letter is ever written to. With no card, the step
says so and the build still succeeds. Another letter: `make -f Makefile.gc
SDCARD=/g`; deploy alone, without building: `make -f Makefile.gc deploy`.

`logs/` and `deployed/` are ignored by git.

### 11.6 First findings (2026-09-12)

**The second hardware run wrote no `wiimc.log` at all.** So the run died
before the log file could be created — before, or during, the SD mount in
`FindAppPath()`.

**Why nothing is ever visible there.** `InitVideo()` ends in
`VIDEO_SetBlack(TRUE)`, and no framebuffer exists until `InitVideo2()`, which
runs *after* `FindAppPath()`. Anything that dies in that window — including a
CPU exception, whose libogc dump draws into the current framebuffer — shows a
black screen. The boot console (`DebugLogScreenShow()`) now fills that window
with the breadcrumbs, and gives libogc's dump a framebuffer to draw into.

**Dolphin is useful after all, for everything before the card.** It has no SD
reader, but it runs this program up to the menu, and with the boot console it
shows how far the boot got. It found two faults, both of the same kind:

1. `_rename_r` read `0x00000030`. That one was the log's own: `rename()` on
   `sd1:` with nothing mounted. **newlib's `_rename_r` and `mkdir` do not
   check `FindDevice()` for -1** — they index `devoptab_list[-1]`. Guarded in
   `DebugLogAttachCard()`.
2. `mkdir` read `0x00000034`. That one is **upstream's**, and it is a
   plausible match for the hardware crash: with no card mounted,
   `LoadSettings()` fails, `SaveSettings()` then calls `mkdir("sd1:/apps")`,
   and the `CheckMount(DEVICE_SD, 1)` that should have prevented it is
   commented out (`source/settings.cpp`). The "Could not find SD card" prompt
   below it was unreachable. The check is restored with `FindDevice("sd1:")`;
   under Dolphin the prompt now appears over the WiiMC menu.

If that is what happened on the console, the real question is not the crash
but **why `sd1:` did not mount on hardware from a card Swiss boots from**. The
boot console now names each interface as it is probed (`sd: trying ...`) and
which one mounted.

**The third hardware run faulted in the log itself** (photo of 2026-09-12,
build `20260912-220053`): DSI, `SRR0 80033ef8` = `paint_range`, called from
`paint_live_stack` in `DebugLogInit`, `DAR 806edf20`, `DSISR 02400000`, and
`r8 = 57ac57ac`, the paint pattern. `02400000` is a store **and a DABR
match**. libogc2 arms the DABR — the CPU's data address breakpoint — on a
doubleword near the bottom of every thread's stack as an overflow guard, and
`_cpu_context_switch` reloads it per thread from `context.dabr`. Painting the
live main-thread stack from the bottom wrote straight into the guard.
**Dolphin does not emulate the DABR**, which is why the same build ran there.

The paint now reads the running thread's guard from SPR 1013 and never writes
or reads that doubleword; the trampoline records each thread's guard the same
way. Worth remembering beyond this file: **on libogc2, any code that scans or
fills a thread's stack from the bottom will trip this guard.**

So the hardware has not yet run far enough to show the original crash.

**The fourth hardware run reached the menu** (build `20260912-222345`, log in
`logs/`). The SD mounted through the SD Gecko in slot B at 0.3 s; the menu was
up at 0.6 s with all ten threads created. Input did work: the main loop logged
`menu: 3` (`MENU_SETTINGS`) at 10.196 s. The machine died within 300 ms of
that — the heartbeat due at 10.5 s never came — and **no CRASH block reached
the log**. Also in that log:

- `STACK OVERFLOW` on the heartbeat thread was **false**: libogc2's
  `__lwp_thread_loadenv` writes a `0xDEADBABE` canary on the base word of every
  stack, and arms the DABR (write only, flags 6) on the base rounded up to 8.
  The heartbeat's static stack is not 8-aligned, so the scan read the canary.
  Everything below the end of that doubleword is now left alone.
- The device thread was parked in `__gcode_IsInserted`: it polls the GC Loader
  interface every two seconds on a console that has none. Not yet known to
  matter; noted.

Why no report is a guess until the next run, but a good one: the report path
opened the log with `fopen`, which allocates from the heap under newlib's
malloc lock — the first casualty of a crash in C++ GUI code. The report now
goes first to `sd1:/wiimc-crash.txt`, created at boot and **held open** by
descriptor, so the crash path needs only `lseek`/`write`/`fsync`. A report
from the previous run is kept as `wiimc-crash-prev.txt`; `deploy` fetches all
four files.

### 11.7 The fifth hardware run (2026-09-13)

Build `20260912-223308`, the one the instruments of §11.6 were added for.
`logs/20840921-101648-wiimc.log`, with `logs/20840921-101616-wiimc-crash.txt`
beside it.

The run reached the menu and stayed there: `sd1:` mounted through the SD Gecko
in slot B at 0.308 s, app path at 0.526 s, the menu up at 0.685 s with ten
threads. Then thirty-two seconds of browsing — `menu: 1`, `menu: 0` at
31.216 s, `menu: 1` again at 32.256 s — with memory flat across all fifteen
heartbeats: `mem free 12871K`, `low` equal to it, `oom 0`, not one byte of
drift. Then `menu: 3` (`MENU_SETTINGS`) at 32.696 s, and the log stops. `hb 16`
was due at 32.75 s. **The machine died less than 60 ms after entering
Settings**, against 300 ms in the fourth run, which died on the same
breadcrumb.

Two instruments were added for this run. One stayed silent, and the answer
turned out to be in what the screen did.

**The crash file was never written.** `sd1:/wiimc-crash.txt`, created and held
open by descriptor at boot precisely so that a crash would need no `malloc`,
came back 24 KB of padding — every byte `0x0a`. The fourth run's guess is
therefore wrong: the report did not die inside `fopen`'s allocation, because it
never got as far as opening anything.

**The dump did come up.** Asked what the screen shows, the user saw it: numbers
on black for a fraction of a second, then the GameCube boot animation. So an
exception *is* taken, libogc *does* print its register dump — and the console
is then reset out from under it before anyone can read it.

**What resets it is our own instrument.** `main()` calls
`__exception_setreload(0)` (`source/wiimc.cpp`), added in §11.6 to hold the
dump on screen. It does the opposite. In the linked ELF:

- `__exception_setreload(t)` is three instructions: `reload_timer = t * 50`,
  a count of 20 ms ticks.
- `c_default_exceptionhandler`, after printing, loops on the pads. It prints
  the countdown only while `reload_timer > 0`, and it *keeps looping* only
  while `reload_timer != 0`. Z or RESET break out. Reaching the bottom with
  the counter at exactly zero falls straight into `__reload()`.
- `reload_timer` lives in `.sdata` and its initial value is `ffffffff`. **The
  library's own default is -1: wait forever.**

So `0` is the single value that means *reload now*, `-1` is the one that means
*never*, and the call written to keep the dump up is the one that throws it
away on the first pass of the loop.

`__reload()` then looks for the `STUBHAXX` magic at `0x80001800` and jumps to
the loader stub if it finds one. Swiss leaves none, so it falls through to
`__SYS_DoHotReset()` — which is the boot animation the user sees, and the
reason this looked like a machine reset rather than a crash.

It also explains the one run that *was* photographed. The third run's DABR
fault happened inside `DebugLogInit()`, called at `source/wiimc.cpp:1203`,
**before** the `__exception_setreload(0)` at 1226: the counter was still the
library's -1, so that dump stayed up. Every hardware run since has crashed
after line 1226. All five deployed builds carry `li r3,0` at that call site.

Fixed by passing `-1`. The next run's dump stays until Z or RESET.

**And the empty crash file, with that in hand.** The wrapper did run — in the
linked ELF the only two branches to `c_default_exceptionhandler` are both
inside `__wrap_c_default_exceptionhandler`, and nothing else reaches it. So
the report was formatted into RAM and the card write was attempted, and the
file still came back as padding. What is left is the write itself: either it
returned an error, or it faulted, in which case the second-exception path
calls the real handler with the *first* context — which would produce exactly
the dump that appears. The card write is the one step that runs with
interrupts back on, inside libfat and EXI, from an exception context. The
next photo settles it: a `*** second exception` line cannot reach the card,
but the dump names the fault that did.

Two other things this run settles:

- **The `DEADBABE` false positive is gone.** Thread #1, the heartbeat, reports
  2836 and then 2892 bytes of its 16384 used, and no `STACK OVERFLOW`. The
  §11.6 fix is confirmed on hardware.
- **Entering Settings creates no thread.** Each menu change starts a thread at
  `80014b18` = `ThumbThread` (`source/menu.cpp:2851`) with a heap stack — #10
  at `menu: 1`, #11 at `menu: 0`, #12 at `menu: 1` — and `MenuBrowse` joins it
  on the way out (`menu.cpp:4570`). `menu: 3` starts none, so thread creation
  is not on the path to the crash.

**Marks inside `MenuSettings()`.** It runs a hundred lines
(`source/menu.cpp:7129`) between the `menu: 3` breadcrumb and its idle loop,
and not one of them was marked. Four now are: on entry, around the
`GuiOptionBrowser` constructor, after the `SuspendGui`/`Append`/`ResumeGui`
handover, and on the first pass of the loop. They split those 60 ms, and the
last one splits the menu thread from the GUI thread drawing what it has just
been handed. Each reaches the card by itself, so the last one written names
the statement — even if the crash report is lost again.

Two leads that the same run closes, so they are not chased twice:

- **Not the artwork.** The option browser's eight PNGs — `bg_entry`,
  `bg_entry_over`, the three `scrollbar` pieces, the `arrow` pairs — are all
  decoded by `GuiFileBrowser` too, and the browse menus ran for half a minute.
- **Not an array overrun.** This screen asks for eight rows where every other
  settings screen asks for six or seven, but `PAGESIZE` is 11
  (`source/libwiigui/gui.h:73`), so `optionTxt[]` and its siblings hold them.

What *is* new at `menu: 3` is the `GuiOptionBrowser` class itself: `MenuBrowse`
does not build one, so this is the first in the run.

### 11.8 The sixth hardware run (2026-09-13): the crash, named

Build `20260913-205459`, the first with `__exception_setreload(-1)`. The dump
stayed on screen and was photographed (`PXL_20260913_190746352.jpg`):

```
Exception (DSI) occurred!
GPR01 8059E9A8   GPR08 8059F200
LR 8027CEE8   SRR0 802B1320   SRR1 00009032   MSR 00001000
DAR 8059F200   DSISR 02400000
STACK DUMP: 802b1320 -> 8027cee8 -> 8020ec9c -> 80031734 -> 80031cdc
            -> 80032680 -> 8002ce84 -> 8002f3c4 -> 800138d0
CODE DUMP 802b1320: 91480000 38E80010 91480004 91480008
```

Resolved against `deployed/wiimc-20260913-205459.elf`:

| Address | Frame |
|---|---|
| `802b1320` | `memset` — the `stw r10,0(r8)` of its four-word loop |
| `8027cee8` | `cf2_arrstack_init`, freetype-2.14.3 `src/psaux/psarrst.c:66` |
| `8020ec9c` | `FT_Load_Glyph`, `src/base/ftobjs.c:1077` |
| `80031734` | `FreeTypeGX::cacheGlyphData()`, `source/utils/FreeTypeGX.cpp:337` |
| `80031cdc` | `FreeTypeGX::getWidth()`, `FreeTypeGX.cpp:597` |
| `80032680` | `FreeTypeGX::getStyleOffsetWidth()`, `FreeTypeGX.cpp:435` |
| `8002ce84` | `GuiText::Draw()`, `source/libwiigui/gui_text.cpp:547` |
| `8002f3c4` | `GuiWindow::Draw()`, `source/libwiigui/gui_window.cpp:140` |
| `800138d0` | `GuiThread()`, `source/menu.cpp:1214` |

**It is a stack overflow of the GUI thread**, and three numbers say so with no
inference required:

- `DSISR 02400000` is a store **and a DABR match** — libogc2's stack guard
  (§11.6), not a wild pointer.
- `DAR 8059F200`, and in the ELF `guistack` is at **exactly `8059f200`**,
  `0x4000` long. The faulting store lands on the guard word at the very bottom
  of the GUI thread's stack.
- `GPR01`, the stack pointer, is `8059E9A8` — **2136 bytes below `guistack`**,
  already off the end and inside `progressstack`, which ends where `guistack`
  begins.

So the thread had spent its whole 16 KB and 2 KB more *before*
`cf2_arrstack_init` got to memset its own local.

**The card agrees.** The same run's log, `logs/20840921-101738-wiimc.log`,
names thread #3 as `GuiThread` with `stack 8059f200 size 16384` — the address
in `DAR`, confirmed from the console rather than from the ELF. Its last four
lines are the §11.7 marks: `settings: entered`, `building the option browser`,
`option browser built`, `appended, the GUI is running again` — and then
nothing. The fourth is the last statement of `MenuSettings()` before its idle
loop, so the menu thread finished handing its elements over and the GUI thread
died on the first frame it drew them in. `settings: first pass of the idle
loop` never printed.

**Why FreeType, and why it is not a port bug.** `source/fonts/font.ttf` begins
with `OTTO`: it is OpenType with **CFF** outlines, so every glyph not yet in
the cache is rendered by FreeType's Adobe CFF interpreter (`psaux/cf2_*`),
which is far hungrier with the stack than the TrueType path. The 16 KB stacks
are upstream's, sized against a much older FreeType; this tree builds against
the current devkitPro (§7, commit `45a2104`), which is freetype 2.14.3. Old
code, new library.

**Which text.** The chain runs straight from `GuiWindow::Draw()` to
`GuiText::Draw()` with no browser frame between, and it ends at the thread
entry, so it is complete: the element being measured is one of `mainWindow`'s
own children. In `MenuSettings()` that is `titleTxt` — the word *Settings*.

**So it was never a Settings bug.** Any screen drawing an uncached glyph was
one glyph away from this. Settings is where it happened to land.

**The fix.** `GSTACK` and `GUITH_STACK` go from 16 KB to 64 KB
(`source/menu.cpp`). All four threads they cover draw — `GuiThread`,
`ProgressThread`, `ScreensaverThread` (`menu.cpp:500`) and `CreditsThread` —
so all four get it. That is 192 KB more `.bss` against the ~12.8 MB the run
reports free (§9). The painted-stack high-water mark in the heartbeat will
report what is really used, so the number can be tightened from a measurement
instead of a guess.

Two notes for later:

- **The guard is a good instrument.** The same DABR that produced the *false*
  `STACK OVERFLOW` of run 4 is what caught this one exactly, at the first byte
  past the end. The fault was the log's scan reading it, never the guard.
- **The crash file still comes back empty.** `wiimc-crash.txt` from this run is
  24 KB of padding again, and the log's own reserved tail is untouched too, so
  both writes on the exception path failed. The report is built in RAM before
  either, and the screen proves that much ran. What fails is the write itself,
  from an exception context, through libfat and EXI. Unresolved, and no longer
  urgent now that the dump stays up.

### 11.9 The seventh run: Settings fixed, and Z is a different bug

Build `20260913-211322`, log `logs/20840921-102014-wiimc.log`.

**The stack fix holds.** Settings opens. The §11.7 marks run all the way
through — `settings: first pass of the idle loop` at 42.999 s — and the menu
was entered and left twice more after that. The heartbeat then gives the number
the 64 KB was a guess about:

```
thr #3  entry 800137d8 prio  60 ... stack 19624/65536
```

The GUI thread really wants **19624 bytes**. The 16384 it used to have was
3240 short — which is why it died on the first uncached glyph it met with a
deep enough frame, and why nothing before Settings had tripped it. At 64 KB it
sits at 30% used, so the size can stay as it is.

**Z in a browse menu is a separate, older bug.** Z is bound in `GuiThread`
(`source/menu.cpp`) to `ResumeCreditsThread()` while `menuCurrent < 2`, that is
on the Videos and Music browsers; the README documents it as a feature — *press
Z to view the credits screen, it will also display the available memory*.
`CreditsWindow()` opened with:

```c
int numEntries = 15;
GuiText *txt[numEntries];
```

and then filled **fourteen** of them. Four further entries exist in the
function but sit under `#if 0`. Both loops at the end run to `numEntries`:

```c
for(i=0; i < numEntries; i++) alignWindow.Append(txt[i]);
...
for(i=0; i < numEntries; i++) delete txt[i];
```

So `txt[14]` was an uninitialised stack word: appended into the window,
dereferenced by the GUI thread on the next frame it drew, and finally handed to
`delete`. Undefined behaviour that survived on the build this fork came from —
the slot only has to happen to hold something harmless — and does not survive
here.

Fixed by sizing the array for every entry in the source (18) and counting the
ones actually built, so re-enabling the `#if 0` block cannot bring it back.

No register dump for this one. The log stops at `menu: 1` and `hb 23` with
nothing after, and `wiimc-crash.txt` is 24 KB of padding for the third run in a
row (§11.8). The card said where it happened; the source said what it was.

### 11.10 Why the file browser was empty

Reported on 2026-09-13: nothing opens under Music. It is not a drawing problem
and it is not the card's format — the browser had nothing to list.

`BrowserChangeFolder()` (`source/filebrowser.cpp`) builds the top-level
listing, the one that offers the devices, only when **both** hold:

- `isInserted[DEVICE_SD]` is true, and
- some `part[DEVICE_SD][i].type > 0`.

Neither did, and for the same reason. `source/fileop.cpp` declared, for the
GameCube:

```c
static DISC_INTERFACE* sd = &__io_gcode;
```

one hardcoded interface — the GC Loader — while `FindAppPath()` probes four in
turn (GC Loader, SD2SP2, SD Gecko slot B, slot A) and mounts through whichever
answers. Here that is the SD Gecko in slot B. So:

1. `AddPartition(0, DEVICE_SD, T_FAT, &devnum)` called
   `fatMount("sd1", sd, ...)` through the wrong interface, on top of an `sd1:`
   that `fatMountSimple()` had already mounted. It failed and returned
   **before** `part[].type = type`. The partition table stayed empty.
2. `devicecallback()`, the device thread, polls `sd->isInserted(sd)` every two
   seconds. On a console with no GC Loader that is false, so 200 ms after boot
   it took the card as *removed*: `UnmountPartitions(DEVICE_SD)`,
   `sd->shutdown(sd)`, `isInserted[DEVICE_SD] = false`.

What saved the run is that `UnmountPartitions()` switches on
`part[][].type`, which the first bug had left at 0 — so it unmounted nothing.
`sd1:` kept working, the log kept being written, the settings kept saving, and
only the browser was blind. Two bugs whose symptoms cancelled into one silent
one.

**Fixes.** `sd` is assigned the interface that actually mounted, in each of the
four probe branches. `FindAppPath()` fills `part[DEVICE_SD][0]` itself rather
than calling `AddPartition()`, which could only try to mount an
already-mounted `sd1:` a second time. And the insert/remove polling is now
`#ifdef HW_RVL`: on the GameCube the mounted card is the one the program booted
from and the one the log is written to, there is no hot-swap story, and polling
a memory card slot every two seconds is exactly the EXI traffic §11.2 refuses
to generate.

That also closes the loose end of §11.6 — *the device thread polls
`__gcode_IsInserted` every two seconds on a console that has none*. It was not
harmless, it was half of this.

**So adding music is: put the files on the card.** Anywhere on it. Music opens
on the device list, A enters the card, B goes back up. There is no folder to
configure and no setting to find (§5.4).

The eighth run, build `20260913-212428`, also confirms §11.9 in passing: 102
seconds, menus 0, 1, 3, 4, 5 and 6 visited repeatedly, `EXIT: exit(0)` from
START at the end, and no crash. The credits array and the GUI stack are both
settled.

### 11.11 A folder of MP3s, and what it showed

Reported on 2026-09-13, after §11.10 made the browser work: a folder of MP3s
also listed a pile of `AlbumArt…` entries *that are not on the card*, and
playing a track left a "Loading..." window up forever with the music audible
behind it.

**The AlbumArt entries are on the card.** They are Windows Media Player's
artwork — `AlbumArtSmall.jpg`, `AlbumArt_{GUID}_Large.jpg`, `Folder.jpg`,
`Thumbs.db` — and every one of them carries the FAT **hidden + system**
attributes, which is why Windows Explorer does not show them and the browser
does. `ParseDirEntries()` skips only names starting with `.` or `$`, and with
`hideExtensions` on the label loses its `.jpg`, so they read as folders.

The reason they are listed at all is that the whole extension filter in
`ParseDirEntries()` was inside `#if 0`:

```c
if(!IsAllowedExt(ext) && (!IsPlaylistExt(ext)))
    continue;
```

With it disabled, `GetExt()` was never called either — so the `IsPlaylistExt(ext)`
a few lines below read an **uninitialised** buffer, and whether a file was
flagged `TYPE_PLAYLIST` depended on stack leftovers. The block is restored
(with the `AddEntrySubs()` NULL dereference in it fixed), and
`f_entry->length`, which was assigned a `st_size` that nothing ever filled,
is now 0 — nothing reads it.

**The "Loading..." hang is not what it looked like.** Two guesses died on the
evidence in `logs/20840921-105340-wiimc.log`, both worth recording so they are
not tried again:

- *Not the embedded cover art.* Both MP3s carry an ID3 `APIC` frame, and
  FFmpeg exposes those as a video stream, which would have kept
  `mplayer.c`'s `if(!mpctx->sh_video)` from handing control back. But MPlayer's
  own output in the log says `Audio only file format detected.` and `No video -
  returning control to GUI` for the file that hung.
- *Not the file.* The first track played, from another folder, ran for 81
  seconds with the browser fully usable — and it has an `APIC` too.

What the run does show is the deadlock itself. At `hb 51`, with the music
playing:

| Thread | Where |
|---|---|
| #4 `ProgressThread` | `menu.cpp:2037`, inside `ProgressWindow`'s draw loop, **state 0 — not suspended** |
| #0 main | `CancelAction()` at `menu.cpp:2116` and `LoadMPlayerFile()` at `wiimc.cpp:848` on the stack |

`CancelAction()` sets `showProgress = 0` and `progressThreadHalt = 1` and then
waits for the progress thread to suspend; `ProgressWindow()` leaves its loop
only while both of those hold. So either something re-armed them after the
store, or the main thread is really in the *other* wait —
`while(controlledbygui != 0)` — and the progress window is simply still up
because nobody has cancelled it yet. The heartbeat's per-thread list is a
**stack scan, not an unwind** (§11.4), so it cannot separate the two: both
addresses can sit on the same stack, one of them stale.

**So this one gets an instrument instead of a fix.** The four waits that can
never end — `LoadMPlayerFile` (`controlledbygui == 0`), `LoadNewFile`
(`controlledbygui == 1`), `CancelAction` (progress thread suspends) and
`ParseDirectory` (parse thread suspends) — each write one `stuck:` line after
two seconds, naming the loop and the values it is watching, and then keep
waiting. The next run that hangs says which of the four it is in the log,
without a photograph and without guessing.

Also measured in that run, worth carrying into §9: **MPlayer's thread peaked at
511560 bytes of its 524288-byte stack** while playing an MP3. That is 12 KB of
headroom on a stack that has never been examined. It did not fail, but it is
the next stack to widen if anything odd appears during playback.

### 11.12 The "Loading..." that never went away

The §11.11 watchdogs answered on the first run that hung
(`logs/20840921-111348-wiimc.log`):

```
[    5.046] play: sd1:/music/2080 - The backup/…My Megadrive….mp3
[    9.757] play: sd1:/music/F-Zero X - OST/…01 - Endless Challenge.mp3
[   11.763] stuck: LoadMPlayerFile wants controlledbygui==0, it is 1
```

**It was never about the folder.** The first file played, the second one hung —
at 5.0 s and 9.8 s here, at 7 s and 88 s in the run before. Any second file
does it.

`controlledbygui` is the whole handshake between the menu and MPlayer, and it
carries three meanings on one `int`: 2 = *stop what you are playing*, 0 =
*MPlayer has the file*, 1 = *control is back with the GUI*. `LoadMPlayerFile()`
drove it like this:

```c
controlledbygui = 2;              // stop the previous file
while(controlledbygui == 2) …     // wait for MPlayer to acknowledge
wiiLoadFile(loadedFile, …);       // hand the new one over
while(controlledbygui != 0) …     // wait for MPlayer to take it
```

MPlayer's side, at `play_next_file` (`mplayer.c`), frees the filename, sets
`controlledbygui = 1` — **overwriting the GUI's 2, which is what releases the
first wait** — then sleeps in a loop until a filename appears, and on leaving
it sets `controlledbygui = 0`. It opens the file, finds no video stream, and at
`if(controlledbygui == 0) controlledbygui = 1;` hands control straight back.

So for an audio-only file, **0 exists only for as long as the demuxer and codec
init take**. The MPlayer output in the same log, between the `play:` line and
the `stuck:` line, shows exactly that window going by: `Audio only file format
detected.` … `Selected audio codec: [ffmp3]` … `No video - returning control to
GUI` … `Starting playback...`. The GUI polls every 100 µs and still missed it,
and then waited for a 0 that was never coming round again. The music played,
the browser stayed behind a "Loading..." window, nothing refreshed.

The first file after boot always worked because MPlayer starts up already
sitting at 0.

**The fix is a fourth value.** The GUI now writes `controlledbygui = 3`,
*handed over but not taken yet*, immediately before `wiiLoadFile()`, and waits
for it to stop being 3:

```c
controlledbygui = 3;
wiiLoadFile(loadedFile, partitionlabel);
while(controlledbygui == 3) …
```

Nothing in `mplayer.c` tests for 3 — every test there is `== 0`, `== 1` or
`== 2` — and MPlayer is parked in `play_next_file`'s sleep loop when the GUI
writes it, executing nothing but `usleep()` and an `== 2` check. So the only
thing that can clear the 3 is MPlayer picking the file up, whether it then
settles on 0 or races on to 1. A transient no longer has to be caught in the
act.

The watchdog stays, now worded for the new wait. Waiting on a value that the
other side only holds briefly is the bug; the lesson is worth more than the
patch, because the same `int` still carries four meanings across two threads
with no lock.

### 11.13 Where it stands at the end of 2026-09-13

**The handshake fix holds.** Build `20260913-221926`,
`logs/20840921-111428-wiimc.log`: four files played in a row — 4.651 s, 8.321 s,
11.363 s, 14.806 s, across two folders — with no `stuck:` line anywhere and the
browser usable between them. The §11.12 race is closed.

**A new crash, in a new place.** The log stops 28 ms after the fourth
`play:`. `hb 7` at 14.834 s is on the card, `hb 8` was due at 16.85 s and never
came. MPlayer had printed `mplayer: end film. UNINIT. err: 0` for the third
file and had not yet printed a single line for the fourth — no `Playing .`, no
`Audio only file format detected.` So the machine died **inside MPlayer's open
of the fourth file**, before the demuxer had identified it.

It is not memory: `mem free 10444K`, `low 10403K`, `oom 0`, and the figure had
not drifted across the three previous loads.

And `wiimc-crash.txt` is 24 KB of padding again — the fourth run in a row where
the exception path builds its report and fails to write it (§11.8). That is now
the most expensive open problem in this file: every crash costs a photograph.

**The prime suspect, and it is only a suspect.** The MPlayer thread's stack.
`MPLAYER_STACKSIZE` is 512 KB (`memalign` in `source/wiimc.cpp`), and the
painted high-water mark reads

```
thr #7  entry 80023bac prio  68 ... stack 511560/524288
```

— **12728 bytes of margin, and the identical figure in each of the last three
runs.** The crash falls in the deepest work that thread does. If the stack is
what gave way, the fault is a DABR hit at the bottom of `mplayerstack` with
`DSISR 02400000`, exactly the signature of §11.8, and one photograph settles it
— the dump now stays on screen (§11.7).

Two things to do first, in this order:

1. **Photograph the dump.** `SRR0`, `DAR`, `DSISR`, and the stack chain resolve
   against `deployed/wiimc-20260913-221926.elf`. That is one run and it either
   confirms the stack or names something else entirely.
2. **Raise `MPLAYER_STACKSIZE` and watch the number.** If the high-water mark
   moves with the stack, 511560 was real and the stack was short. If it stays at
   511560 with a larger stack, then the *measurement* is what is wrong — the
   paint scan is reading something it should not, as it did in §11.6 — and the
   suspect is cleared rather than fixed.

> **Answered in §11.14, without a second crash.** The crash did not reproduce —
> the same binary played fifteen files including the one that killed it — and
> the high-water mark froze at 511560 for the whole run, which is the second
> case: the measurement is what is wrong, the stack is cleared.

### 11.14 The run of 2026-09-14: the baseline holds

Same binary as §11.13 — build `20260913-221926`, nothing recompiled — run
again from the card. Log: `logs/20840921-111648-wiimc.log`.

**Fifteen files, no crash.** `play:` lines from 5.904 s to 48.402 s, across the
same two folders, with the browser walked between each one. No `stuck:`
anywhere, no register dump, no `[log buffer overran]`. The run ends on `hb 31`
at 63.096 s, in the middle of the fifteenth track, with no `main: exit
requested` — the console was switched off while it was still playing. (The
`<<<<<<<< end of log >>>>>>>>` marker is written by every flush, `debuglog.c:250`,
so it locates the newest line and says nothing about how the run ended.)

**The §11.13 crash did not reproduce, and it is not the file.** It died opening
`F-Zero X OST - 04 - Decide in the Eyes.mp3`. That file opened and played here
at 16.491 s, and the run went on through eleven more. One occurrence, no dump,
not deterministic, not file-specific: it stays on the books as an unexplained
fault, to be picked up again if it returns with a photograph.

**Memory is flat across fifteen opens.** 12659K free at `hb 1`, 10409K at
`hb 24` through `hb 31`, `low 10345K`, `oom 0`, and the figure does not drift
by more than a kilobyte between loads. No leak per file.

**The stack suspect of §11.13 is cleared — and the measurement is what is
wrong.** The MPlayer thread's painted high-water mark reads

```
hb  1   thr #7 ... stack  10696/524288   (before any file was opened)
hb  6   thr #7 ... stack 511560/524288
hb 11   thr #7 ... stack 511560/524288
hb 16   thr #7 ... stack 511560/524288
hb 21   thr #7 ... stack 511560/524288
hb 26   thr #7 ... stack 511560/524288
```

It jumps to 511560 on the first file and then does not move by a single byte
across fourteen more opens — two of those dumps were taken from a different
call chain, one of them at a different `sp`. A real high-water mark drifts;
this one does not. So 511560 is not the deepest the thread has gone — it is
where the paint scan stops finding its pattern, i.e. the topmost word that
something clobbered once, 12728 bytes from the bottom of the region. §11.6 had
the same class of bug in the same scan.

That settles §11.13's second question without spending a run on it: **this run
reached 511560 too, fifteen times over, and nothing crashed.** 511560 was never
a near-overflow, so the stack cannot be what killed the fourth file. Do not
raise `MPLAYER_STACKSIZE` on the strength of that number — fix or distrust the
scan instead. What paints `mplayerstack`, and what writes 12728 bytes above its
base before the first `play:`, is worth ten minutes when the log is next
touched; it is not worth blocking Phase 1.

**Still open, unchanged.** `wiimc-crash.txt` came back 24 KB of padding for the
fifth boot in a row, but this run never crashed, so it proves nothing new. The
write-from-an-exception-context problem of §11.8 is still there, still costing a
photograph per crash, and is still the first thing to fix the next time the
exception path is entered.

**Phase 0 is closed** (§8). Phase 1 starts with `-lbba` and `netcb()`.

### 11.15 The network comes up (2026-09-14)

Build `20260914-215005`, two boots on the card: `logs/20840921-111814-wiimc-prev.log`
then `logs/20840921-111942-wiimc.log`.

**It works, and the label earns its keep.**

```
[    0.759] net: if_config (dhcp), attempt 1
[   11.531] net: up on Broadband Adapter, Serial Port 1 -- ip 192.168.68.57 mask 255.255.252.0 gw 192.168.68.1
```

Eighteen files played afterwards, `oom 0`, log ending clean at 55.330 s.

**`if_config()` blocked for 10.77 seconds.** That is the single most useful
number in this run. It is not an error path -- that is what a successful
DOL-015 link negotiation costs. Had it been called on the boot path the screen
would have sat there for eleven seconds with nothing to say, and the obvious
conclusion would have been a hang. The heartbeat proves the thread was the
right call: `hb 1` through `hb 5` fired on time at 2.746, 4.770, 6.785, 8.800
and 10.820 s, straight through the block, and the menu drew throughout.

**lwIP brings its own thread.** `thread: #12 entry 801e1ae4 stack 8073cb88
size 32768 prio 220` appears between `hb 3` and `hb 4`, created inside
`if_config()`. Priority 220 is above everything this program runs. Worth
remembering when phase 2 starts competing for the DSP.

Our own network thread (`thr #2`) peaks at **3200 bytes of its 32768**. Ten
times more than it needs, and it can be cut if the memory is ever wanted.

**The transport costs 558 KB of RAM.** `mem free` at `hb 1` goes from 12659K on
build `20260913-221926` to 12101K here, and `arena1` lo moves from `8082b000`
to `808ba000` -- 585728 bytes, of which 91136 is the larger DOL and the rest is
lwIP's static buffers plus our 32 KB stack. Phase 2's cache sizing has that
much less to work with than §11.14 measured.

**A defect the first boot caught, which the second would have hidden.** The
cable was not up on boot one:

```
[    5.797] net: if_config failed (-1) -- cable, link LED, or no adapter
[    6.804] net: if_config (dhcp), attempt 2
[    6.818] net: up on unknown adapter -- ip 255.255.255.255 mask 255.255.255.255 gw 255.255.255.255
```

The second call returned **success in 14 ms with the broadcast address in all
three output buffers**, and the code believed it: `networkInit` went true and
`dns_set_server()` was handed 255.255.255.255. libogc2 brings the stack up
once; a second `if_config()` cannot un-fail the first, and what it writes into
those buffers after a failure is not a lease.

Fixed: `UsableAddress()` rejects `0.0.0.0` and `255.255.255.255`, the gateway
is validated separately before it reaches the resolver, and a `res >= 0` with
an unusable address stops the retry loop instead of spending the rest of it.
Without the label of §5.3 this would have read as a plain success -- "unknown
adapter" is what gave it away.

### 11.16 The first stream attempt: the link is not the problem

Build `20260914-221604`, `logs/20840921-112120-wiimc.log`. The card carries
`sd1:/music/radio-test.m3u`, six plain-`http://` MP3 stations taken from
gcradio's `gcradio.conf` — stations that project probed from the PC on
2026-08-04 and plays on this console.

**Everything up to MPlayer works.**

```
[   10.813] net: up on Broadband Adapter, Serial Port 1 -- ip 192.168.68.57 mask 255.255.252.0 gw 192.168.68.1
[   18.699] play: http://nectarine.from-de.com/necta192
```

The link comes up in 10.8 s again, the playlist parses, and the URL travels the
whole way from the browser into `wiiLoadFile`. Both lots of phase 1 do their
job. Note the `.m3u` route needs no phase 2 work: `m3u` is in
`validPlaylistExtensions` and `http` is already in `validInternetProtocols`
(`settings.h:237,247`), and `menu.cpp:4160` hands anything matching either to
`BrowserChangeFolder()`.

**And then it does not open.**

```
Playing .
Failed to open http://nectarine.from-de.com/necta192.
mplayer: end film. UNINIT. err: 0
```

on a loop, which is what "Loading..." on screen forever actually is. No
resolver line, no `connect2Server`, no server response — because none of them
ran. The cause is §2.4: `open_s1` returns `STREAM_ERROR` before doing anything,
and so does everything under it.

**What this run cost and what it bought.** Two builds passed a clean link while
the HTTP path could not have worked at any point, and nothing short of running
it would have said so. It also produced the `closesocket` finding of §2.4,
which the retry loop would have turned into a socket-table exhaustion a few
attempts later — a second bug the first one was hiding.

Restored in build `20260914-223411` (six functions) and `closesocket` in the
build after it. **The resolver, `connect2Server` and the ICY path have still
never executed once.** The next hardware run is the first real test of any of
them.
