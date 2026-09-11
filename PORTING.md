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

**Status: builds clean, not yet booted.** The five problems of §7.3 are fixed
and `make -f Makefile.gc` produces a 5.7 MB `wiimc.dol` with no errors. The
functional half of the criterion — that it boots from an SD Gecko and plays a
file — needs the console and has not been done. **Phase 1 does not start until
it has**, because a baseline that has only ever been compiled proves nothing
about the four phases that follow.

### Phase 1 — switch the transport back on

`-lbba` in `Makefile.gc`; restore the body of `netcb()`; define
`CONFIG_NETWORKING`; put `stream/network.c` and `stream/http.c` back in the
MPlayer Makefile; point `gethostbyname` at the ported `dns.c`. Add the adapter
label of §5.3.

> **Done when** the console plays an `http://` stream from an Icecast on the
> LAN, and the screen names the adapter it found — on the DOL-015 and on the
> ETH2GC.

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
