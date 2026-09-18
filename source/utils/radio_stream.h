#ifndef WIIMC_RADIO_STREAM_H
#define WIIMC_RADIO_STREAM_H
#include "http_client.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
    gc_http http;
    int audio_left;
    char title[128];
    int title_changed;
} gc_radio;
int gc_radio_open(gc_radio *, const char *, gc_net_cancel_fn, void *);
int gc_radio_read(gc_radio *, void *, int);
void gc_radio_close(gc_radio *);
#ifdef __cplusplus
}
#endif
#endif
