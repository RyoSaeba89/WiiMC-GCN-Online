#ifndef WIIMC_HTTP_CLIENT_H
#define WIIMC_HTTP_CLIENT_H
#include <stdint.h>
#include "net_transport.h"
#ifdef __cplusplus
extern "C" {
#endif
#define GC_HTTP_URL_MAX 2304
typedef struct {
    char host[256], path[2048];
    int port, tls;
} gc_http_url;
typedef struct gc_http {
    gc_connection *conn;
    char url[GC_HTTP_URL_MAX], error[160], location[GC_HTTP_URL_MAX];
    char content_type[128], icy_name[128];
    int status, chunked, eof, failed, icy_interval;
    int64_t length, remaining, range_start, range_end, total, chunk_left;
    unsigned char buffer[4096];
    size_t begin, end;
    int chunk_crlf;
} gc_http;
/* Credentials are explicit; URLs containing userinfo or control bytes fail.
 * Authenticated requests only follow same-origin redirects. No TLS downgrade. */
int gc_http_parse_url(const char *, gc_http_url *);
int gc_http_resolve_url(const char *base, const char *location, char *out, size_t size);
int gc_http_open(gc_http *, const char *url, const char *method,
    const char *authorization, const char *body, int depth, int64_t offset,
    int icy, gc_net_cancel_fn cancel, void *opaque);
int gc_http_read(gc_http *, void *, int);
void gc_http_close(gc_http *);
int gc_http_basic(const char *user, const char *password, char *out, size_t size);
int gc_http_encode_path(const char *path, char *out, size_t size);
int gc_http_decode_path(const char *path, char *out, size_t size);
#ifdef __cplusplus
}
#endif
#endif
