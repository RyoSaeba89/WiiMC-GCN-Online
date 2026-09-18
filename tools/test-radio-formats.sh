#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/bin:$PATH"
cd "$(dirname "$0")/.."
out=tests/out/radio-formats
mkdir -p "$out/libavutil" tests/out/tmp
export TMPDIR="$PWD/tests/out/tmp" TMP="$PWD/tests/out/tmp" TEMP="$PWD/tests/out/tmp"
make -C source/mplayer codecs.conf.h > "$out/codecs.log" 2>&1
# The host runs little endian; the production codec table still uses PPC config.
printf '#define AV_HAVE_BIGENDIAN 0\n#define AV_HAVE_FAST_UNALIGNED 1\n' > "$out/libavutil/avconfig.h"
printf '/* Native host config for isolated FFmpeg helpers. */\n' > "$out/config.h"
extract() {
    awk -v pattern="$1" '$0 ~ pattern { copy=1 }
         copy { print }
         copy && /^};?$/ { exit }' "$2"
}
ff=source/mplayer/ffmpeg/libavformat
{
    extract '^const AVCodecTag ff_codec_bmp_tags' "$ff/riff.c"
    extract '^const AVCodecTag ff_codec_wav_tags' "$ff/riff.c"
    extract '^unsigned int ff_codec_get_tag' "$ff/utils.c"
    extract '^unsigned int av_codec_get_tag' "$ff/utils.c"
    extract '^unsigned int avpriv_toupper4' source/mplayer/ffmpeg/libavcodec/utils.c
    extract '^enum CodecID ff_codec_get_id' "$ff/utils.c"
    extract '^enum CodecID av_codec_get_id' "$ff/utils.c"
    extract '^const struct AVCodecTag [*]avformat_get_riff_video_tags' "$ff/utils.c"
    extract '^const struct AVCodecTag [*]avformat_get_riff_audio_tags' "$ff/utils.c"
    extract '^static int adts_aac_probe' "$ff/aacdec.c"
} > "$out/ffmpeg.inc"
awk '/^#define DEMUXER_TYPE_LAVF / || /^#define DEMUXER_TYPE_LAVF_PREFERRED /' \
    source/mplayer/libmpdemux/demuxer.h > "$out/demux-types.inc"
{
    extract '^static const char [*] const preferred_list' source/mplayer/libmpdemux/demux_lavf.c
    extract '^static int lavf_check_preferred_file' source/mplayer/libmpdemux/demux_lavf.c
} > "$out/preferred.inc"
gcc -std=gnu11 -O2 -g -ffunction-sections -fdata-sections -DGEKKO \
    -I"$out" -Isource/mplayer -Isource/mplayer/ffmpeg \
    tests/test_radio_formats.c source/mplayer/codec-cfg.c \
    source/mplayer/libmpdemux/mp_taglists.c -Wl,--gc-sections \
    -o "$out/test_radio_formats.exe"
"$out/test_radio_formats.exe"
