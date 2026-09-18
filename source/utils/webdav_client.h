#ifndef WIIMC_WEBDAV_CLIENT_H
#define WIIMC_WEBDAV_CLIENT_H
#include "http_client.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GC_DAV_MAX_ENTRIES 1024
typedef struct {
    char url[GC_HTTP_URL_MAX], name[80], authorization[1025];
} gc_dav_config;
typedef struct { char name[256]; int directory; int64_t size; } gc_dav_entry;
typedef struct {
    const gc_dav_config *config;
    gc_http *http;
    char url[GC_HTTP_URL_MAX];
    int64_t size, position;
    int playback_registered;
} gc_dav_file;
typedef void (*gc_dav_wait_fn)(void *opaque);
int gc_dav_config_load(gc_dav_config *, const char *file, char *error, size_t cap);
int gc_dav_list(const gc_dav_config *, const char *path, gc_dav_entry **entries, size_t *count, char *error, size_t cap);
int gc_dav_list_wait(const gc_dav_config *, const char *path, gc_dav_entry **entries,
    size_t *count, gc_dav_wait_fn wait, void *opaque, char *error, size_t cap);
int gc_dav_stat(const gc_dav_config *, const char *path, gc_dav_entry *, char *error, size_t cap);
int gc_dav_open(gc_dav_file *, const gc_dav_config *, const char *path, char *error, size_t cap);
int gc_dav_read(gc_dav_file *, void *, int);
int64_t gc_dav_seek(gc_dav_file *, int64_t offset, int whence);
void gc_dav_close(gc_dav_file *);
#ifdef __cplusplus
}
#endif
#endif
