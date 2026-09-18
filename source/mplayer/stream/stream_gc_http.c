/* GameCube radio transport. The shared client owns TCP, TLS and HTTP framing;
 * MPlayer receives audio bytes only, including for chunked ICY responses. */
#include "config.h"
#ifdef GEKKO
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "stream.h"
#include "mp_msg.h"
#include "../../utils/radio_stream.h"
extern char streamtitle[128], streamname[128];
extern int streamtitle_changed, streamname_changed;
extern void MPlayerNetworkError(const char *);
extern int controlledbygui;
extern volatile int stop_cache_thread;

static int cancelled(void *opaque)
{
    stream_t *s = opaque;
    return controlledbygui == 2 || (s->cache_pid && stop_cache_thread);
}
static int fill(stream_t *s, char *buf, int len)
{
    gc_radio *r = s->priv;
    int n = gc_radio_read(r, buf, len);
    if (r->title_changed) {
        snprintf(streamtitle, 128, "%s", r->title);
        streamtitle_changed = 1;
        r->title_changed = 0;
    }
    if (n <= 0) {
        s->eof = 1;
        s->error = n < 0;
        if (n < 0) MPlayerNetworkError(r->http.error);
        /* Do not enter the legacy GameCube read-zero retry/seek loop. */
        return -1;
    }
    return n;
}
static void close_http(stream_t *s)
{
    gc_radio *r = s->priv;
    gc_radio_close(r);
    free(r);
    s->priv = NULL;
}
static int open_http(stream_t *s, int mode, void *opts, int *format)
{
    (void)opts; (void)format;
    if (mode != STREAM_READ) return STREAM_UNSUPPORTED;
    gc_radio *r = calloc(1, sizeof(*r));
    if (!r) return STREAM_ERROR;
    streamtitle[0] = streamname[0] = 0;
    streamtitle_changed = streamname_changed = 1;
    if (gc_radio_open(r, s->url, cancelled, s)) {
        MPlayerNetworkError(r->http.error);
        free(r);
        return STREAM_ERROR;
    }
    snprintf(streamname, 128, "%s", r->http.icy_name);
    streamname_changed = 1;
    s->priv = r;
    s->fd = -1;
    s->type = STREAMTYPE_STREAM;
    s->flags = 0;
    s->fill_buffer = fill;
    s->close = close_http;
    s->read_chunk = 4096;
    if (!r->http.icy_interval && r->http.length > 0) s->end_pos = r->http.length;
    mp_msg(MSGT_NETWORK, MSGL_INFO, "radio: HTTP %d, %s, ICY interval %d\n", r->http.status, r->http.content_type, r->http.icy_interval);
    return STREAM_OK;
}
const stream_info_t stream_info_gc_http = {
    "GameCube HTTP/HTTPS radio", "gc_http", "WiiMC-GCN-Online", "Bounded HTTP and ICY transport",
    open_http, { "http", "https", NULL }, NULL, 0
};
#endif
