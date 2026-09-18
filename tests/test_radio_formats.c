/* Run production codec lookup and AAC preference with the bundled FFmpeg
 * tag tables/probe. No substitute mapping from Opus to its decoder. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "libavformat/avformat.h"
#include "libavformat/internal.h"
#include "libavutil/intreadwrite.h"
#include "codec-cfg.h"
#include "libmpdemux/mp_taglists.h"

#include "out/radio-formats/ffmpeg.inc"

void load_builtin_codecs(void);
void mp_msg(int mod, int lev, const char *fmt, ...)
{ (void)mod; (void)lev; (void)fmt; }

/* Only lavf_check_file's I/O is adapted; the preference decision is compiled
 * directly from demux_lavf.c, fed by the actual FFmpeg ADTS probe. */
#include "out/radio-formats/demux-types.inc"
typedef struct { AVInputFormat *avif; } lavf_priv_t;
typedef struct { void *priv; AVProbeData probe; } demuxer_t;
static AVInputFormat aac_format = { .name = "aac" };
static int lavf_check_file(demuxer_t *d)
{
    if (adts_aac_probe(&d->probe) <= AVPROBE_SCORE_MAX / 4) return 0;
    ((lavf_priv_t *)d->priv)->avif = &aac_format;
    return DEMUXER_TYPE_LAVF;
}
#include "out/radio-formats/preferred.inc"

static void test_opus(void)
{
    unsigned tag = mp_codec_id2tag(CODEC_ID_OPUS, 0, 1);
    codecs_t *codec = find_audio_codec(tag, NULL, NULL, 0);
    assert(tag != 0 && codec);
    assert(!strcmp(codec->name, "ffopus"));
    assert(!strcmp(codec->drv, "ffmpeg") && !strcmp(codec->dll, "libopus"));
    assert(!find_audio_codec(tag, NULL, codec, 0)); /* No raw PCM fallback. */
    puts("PASS Ogg Opus without a container tag selects FFmpeg/libopus");

    codec = find_audio_codec(mp_codec_id2tag(CODEC_ID_PCM_S16LE, 0, 1), NULL, NULL, 0);
    assert(codec && !strcmp(codec->name, "pcm"));
    codec = find_audio_codec(mp_codec_id2tag(CODEC_ID_AAC, 0, 1), NULL, NULL, 0);
    assert(codec && !strcmp(codec->name, "ffaac"));
    puts("PASS real PCM and AAC retain their correct decoders");
}

static void test_aac(void)
{
    /* Three 186-byte ADTS frames (roughly 64 kbps, 44.1 kHz). The probe
     * validates headers, not payload, so no copyrighted recording is needed. */
    unsigned char bytes[3 * 186 + FF_INPUT_BUFFER_PADDING_SIZE] = {0};
    const unsigned char header[] = {0xff, 0xf1, 0x50, 0x80, 0x17, 0x5f, 0xfc};
    for (int i = 0; i < 3; ++i) memcpy(bytes + 186 * i, header, sizeof(header));
    lavf_priv_t priv = {0};
    demuxer_t d = { .priv = &priv,
        .probe = { .buf = bytes, .buf_size = 3 * 186,
                   .filename = "https://ice1.somafm.com/vaporwaves-64-aac" } };
    assert(lavf_check_preferred_file(&d) == DEMUXER_TYPE_LAVF_PREFERRED);
    puts("PASS extensionless 64 kbps ADTS selects lavf before MPEG video probes");
    memset(bytes, 0, sizeof(bytes));
    assert(!lavf_check_preferred_file(&d));
    puts("PASS invalid data is not accepted as AAC");
}

int main(int argc, char **argv)
{
    load_builtin_codecs();
    if (argc < 2 || !strcmp(argv[1], "opus")) test_opus();
    if (argc < 2 || !strcmp(argv[1], "aac")) test_aac();
    return 0;
}
