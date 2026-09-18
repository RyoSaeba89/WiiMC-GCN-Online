#include "radio_stream.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

int gc_radio_open(gc_radio *r, const char *url, gc_net_cancel_fn cancel, void *opaque)
{
    memset(r, 0, sizeof(*r));
    if (gc_http_open(&r->http, url, "GET", NULL, NULL, -1, -1, 1, cancel, opaque)) return -1;
    if (r->http.status != 200) {
        snprintf(r->http.error, sizeof(r->http.error), "Unexpected radio HTTP status %d", r->http.status);
        gc_radio_close(r); return -1;
    }
    r->audio_left = r->http.icy_interval;
    return 0;
}

static int metadata(gc_radio *r)
{
    unsigned char blocks;
    if (gc_http_read(&r->http, &blocks, 1) != 1) return -1;
    if (blocks) {
        size_t len = blocks * 16, used = 0;
        char *text = malloc(len + 1);
        if (!text) return -1;
        while (used < len) {
            int n = gc_http_read(&r->http, text + used, len-used);
            if (n <= 0) { free(text); return -1; }
            used += n;
        }
        text[len] = 0;
        char *title = strstr(text, "StreamTitle='");
        if (title) {
            title += 13;
            char *end = strstr(title, "';");
            if (end) {
                *end = 0;
                snprintf(r->title, sizeof(r->title), "%s", title);
                r->title_changed = 1;
            }
        }
        free(text);
    }
    r->audio_left = r->http.icy_interval;
    return 0;
}

int gc_radio_read(gc_radio *r, void *buf, int len)
{
    if (r->http.icy_interval) {
        if (!r->audio_left && metadata(r)) {
            snprintf(r->http.error, sizeof(r->http.error), "Truncated ICY metadata");
            r->http.failed = 1;
            return -1;
        }
        if (len > r->audio_left) len = r->audio_left;
    }
    int n = gc_http_read(&r->http, buf, len);
    if (n > 0 && r->http.icy_interval) r->audio_left -= n;
    return n;
}
void gc_radio_close(gc_radio *r) { gc_http_close(&r->http); }
