/* Shared HTTP/1.1 framing for radio, playlists and the read-only DAV device.
 * All server-controlled allocations, lines and redirect chains are bounded. */
#include "http_client.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int fail(gc_http *h, const char *msg)
{ snprintf(h->error, sizeof(h->error), "%s", msg); h->failed = 1; return -1; }

static int safe_text(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    for (; *p; ++p) if (*p < 32 || *p == 127) return 0;
    return 1;
}

int gc_http_parse_url(const char *s, gc_http_url *u)
{
    const char *host, *end, *colon;
    size_t n;
    if (!s || !safe_text(s)) return -1;
    memset(u, 0, sizeof(*u));
    if (!strncasecmp(s, "http://", 7)) { host = s + 7; u->port = 80; }
    else if (!strncasecmp(s, "https://", 8)) { host = s + 8; u->port = 443; u->tls = 1; }
    else return -1;
    end = host + strcspn(host, "/?#");
    colon = memchr(host, ':', end - host);
    n = (colon ? colon : end) - host;
    if (!n || n >= sizeof(u->host)) return -1;
    memcpy(u->host, host, n);
    for (size_t i = 0; i < n; ++i)
        if (!isalnum((unsigned char)u->host[i]) && u->host[i] != '.' && u->host[i] != '-') return -1;
    if (colon) {
        char *tail;
        long port = strtol(colon + 1, &tail, 10);
        if (tail != end || tail == colon + 1 || port < 1 || port > 65535) return -1;
        u->port = (int)port;
    }
    n = strcspn(end, "#");
    if (n + (*end != '/') >= sizeof(u->path)) return -1;
    if (*end != '/') strcpy(u->path, "/");
    strncat(u->path, end, n);
    if (strchr(u->path, ' ') || strchr(u->path, '\\')) return -1;
    return 0;
}

int gc_http_resolve_url(const char *base, const char *loc, char *out, size_t cap)
{
    gc_http_url *u = malloc(sizeof(*u));
    int n = -1;
    char *slash;
    if (!u || !loc || !*loc || !safe_text(loc) || gc_http_parse_url(base, u)) goto done;
    if (strstr(loc, "://")) n = snprintf(out, cap, "%s", loc);
    else if (!strncmp(loc, "//", 2)) n = snprintf(out, cap, "%s:%s", u->tls ? "https" : "http", loc);
    else {
        if (loc[0] == '/') u->path[0] = 0;
        else if (loc[0] == '?') {
            slash = strchr(u->path, '?');
            if (slash) *slash = 0;
        } else {
            slash = strchr(u->path, '?');
            if (slash) *slash = 0;
            slash = strrchr(u->path, '/');
            if (slash) slash[1] = 0;
        }
        n = snprintf(out, cap, "%s://%s:%d%s%s", u->tls ? "https" : "http", u->host, u->port, u->path, loc);
    }
    if (n < 0 || (size_t)n >= cap || gc_http_parse_url(out, u)) n = -1;
done:
    free(u);
    return n < 0 ? -1 : 0;
}

static int raw_read(gc_http *h, void *dst, int len)
{
    if (h->begin == h->end) {
        int n = gc_net_read(h->conn, h->buffer, sizeof(h->buffer));
        if (n < 0) return fail(h, n == -ECANCELED ? "Cancelled" : "Network read failed or timed out");
        if (!n) return 0;
        h->begin = 0;
        h->end = n;
    }
    if ((size_t)len > h->end - h->begin) len = h->end - h->begin;
    memcpy(dst, h->buffer + h->begin, len);
    h->begin += len;
    return len;
}

static int line(gc_http *h, char *dst, size_t cap)
{
    size_t n = 0;
    unsigned char c;
    while (n + 1 < cap) {
        if (raw_read(h, &c, 1) != 1) return fail(h, "Truncated HTTP line");
        if (c == '\n') {
            if (!n || dst[n-1] != '\r') return fail(h, "Invalid HTTP line ending");
            dst[n-1] = 0;
            return (int)n;
        }
        if (!c) return fail(h, "Invalid HTTP header byte");
        dst[n++] = c;
    }
    return fail(h, "HTTP line is too long");
}

static int integer(const char *s, int64_t *out)
{
    char *end;
    long long n;
    if (!isdigit((unsigned char)*s)) return -1;
    errno = 0;
    n = strtoll(s, &end, 10);
    if (errno || *end || n < 0) return -1;
    *out = n;
    return 0;
}

static int headers(gc_http *h)
{
    char *buf = malloc(4096);
    int n, bytes = 0, ret = -1;
    if (!buf) return fail(h, "Out of memory");
    if (line(h, buf, 4096) < 0) goto done;
    if (sscanf(buf, "HTTP/1.%*d %d", &h->status) != 1 && sscanf(buf, "ICY %d", &h->status) != 1) {
        fail(h, "Invalid HTTP status line"); goto done;
    }
    while ((n = line(h, buf, 4096)) > 1) {
        char *v = strchr(buf, ':');
        bytes += n;
        if (bytes > 16384 || !v) { fail(h, "Invalid or oversized HTTP headers"); goto done; }
        *v++ = 0;
        while (*v == ' ' || *v == '\t') ++v;
        char *tail = v + strlen(v);
        while (tail > v && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        if (!strcasecmp(buf, "Content-Length")) {
            int64_t length;
            if (integer(v, &length) || (h->length >= 0 && h->length != length)) {
                fail(h, "Invalid Content-Length"); goto done;
            }
            h->length = length;
        } else if (!strcasecmp(buf, "Transfer-Encoding")) {
            if (strcasecmp(v, "chunked")) { fail(h, "Unsupported Transfer-Encoding"); goto done; }
            h->chunked = 1;
        } else if (!strcasecmp(buf, "Content-Encoding")) {
            if (strcasecmp(v, "identity")) { fail(h, "Compressed HTTP bodies are unsupported"); goto done; }
        } else if (!strcasecmp(buf, "Location")) {
            if (strlen(v) >= sizeof(h->location)) { fail(h, "Redirect URL is too long"); goto done; }
            strcpy(h->location, v);
        } else if (!strcasecmp(buf, "Content-Type")) snprintf(h->content_type, sizeof(h->content_type), "%s", v);
        else if (!strcasecmp(buf, "icy-name")) snprintf(h->icy_name, sizeof(h->icy_name), "%s", v);
        else if (!strcasecmp(buf, "icy-metaint")) {
            int64_t interval;
            if (integer(v, &interval) || interval < 1 || interval > INT_MAX) { fail(h, "Invalid ICY interval"); goto done; }
            h->icy_interval = interval;
        } else if (!strcasecmp(buf, "Content-Range")) {
            long long a, b, total;
            char extra;
            if (sscanf(v, "bytes %lld-%lld/%lld%c", &a, &b, &total, &extra) != 3 || a < 0 || b < a || total <= b) {
                fail(h, "Invalid Content-Range"); goto done;
            }
            h->range_start = a; h->range_end = b; h->total = total;
        }
    }
    if (n < 0) goto done;
    if (h->chunked && h->length >= 0) { fail(h, "Ambiguous HTTP body framing"); goto done; }
    h->remaining = h->length;
    ret = 0;
done:
    free(buf);
    return ret;
}

void gc_http_close(gc_http *h)
{ if (h) { gc_net_close(h->conn); h->conn = NULL; } }

static int send_all(gc_http *h, const char *p)
{
    size_t left = strlen(p);
    while (left) {
        int n = gc_net_write(h->conn, p, left);
        if (n <= 0) return fail(h, "Network write failed or timed out");
        left -= n; p += n;
    }
    return 0;
}

int gc_http_open(gc_http *h, const char *url, const char *method,
    const char *auth, const char *body, int depth, int64_t offset,
    int icy, gc_net_cancel_fn cancel, void *opaque)
{
    gc_http_url *u = calloc(2, sizeof(*u));
    char *request = malloc(8192), *next = malloc(GC_HTTP_URL_MAX);
    char current[GC_HTTP_URL_MAX];
    int ret = -1;
    memset(h, 0, sizeof(*h));
    if (!u || !request || !next) { fail(h, "Out of memory"); goto done; }
    if (strlen(url) >= sizeof(current) || gc_http_parse_url(url, u) ||
        (strcmp(method, "GET") && strcmp(method, "PROPFIND")) || (auth && !safe_text(auth))) {
        fail(h, "Invalid HTTP URL or request"); goto done;
    }
    if (u->tls && auth && *auth) { fail(h, "HTTPS credentials are unsupported"); goto done; }
    strcpy(current, url);
    for (int redirects = 0; redirects <= 5; ++redirects) {
        char extra[1536] = "", field[128];
        memset(h, 0, sizeof(*h));
        h->length = h->remaining = h->total = h->range_start = h->range_end = -1;
        strcpy(h->url, current);
        if (gc_http_parse_url(current, u)) { fail(h, "Invalid redirect URL"); break; }
        h->conn = gc_net_open(u->host, u->port, u->tls, cancel, opaque, h->error, sizeof(h->error));
        if (!h->conn) { h->failed = 1; break; }
        if (offset >= 0) { snprintf(field, sizeof(field), "Range: bytes=%lld-\r\n", (long long)offset); strcat(extra, field); }
        if (icy) strcat(extra, "Icy-MetaData: 1\r\n");
        if (depth >= 0) { snprintf(field, sizeof(field), "Depth: %d\r\nContent-Type: application/xml; charset=utf-8\r\n", depth); strcat(extra, field); }
        if (auth && *auth) {
            if (strlen(auth) > 1024) { fail(h, "Authorization is too long"); break; }
            strcat(extra, "Authorization: Basic "); strcat(extra, auth); strcat(extra, "\r\n");
        }
        if (body) { snprintf(field, sizeof(field), "Content-Length: %u\r\n", (unsigned)strlen(body)); strcat(extra, field); }
        int n = snprintf(request, 8192, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: WiiMC-GCN-Online\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n%s\r\n", method, u->path, u->host, u->port, extra);
        if (n < 0 || n >= 8192) { fail(h, "Request too large"); break; }
        if (send_all(h, request) || (body && send_all(h, body)) || headers(h)) break;
        if (h->status == 301 || h->status == 302 || h->status == 303 || h->status == 307 || h->status == 308) {
            if (redirects == 5) { fail(h, "Too many HTTP redirects"); break; }
            if (gc_http_resolve_url(current, h->location, next, GC_HTTP_URL_MAX) || gc_http_parse_url(next, u+1)) {
                fail(h, "Invalid redirect"); break;
            }
            if ((u->tls && !u[1].tls) || (auth && *auth && (u->tls != u[1].tls || u->port != u[1].port || strcasecmp(u->host, u[1].host)))) {
                fail(h, "Unsafe HTTP redirect refused"); break;
            }
            if (h->status == 303 && strcmp(method, "GET")) { fail(h, "PROPFIND redirected to GET"); break; }
            strcpy(current, next);
            gc_http_close(h);
            continue;
        }
        if (h->status < 200 || h->status >= 300) {
            snprintf(h->error, sizeof(h->error), "HTTP %d%s", h->status, h->status == 401 ? " - check WebDAV credentials" : "");
            h->failed = 1; break;
        }
        if (h->status == 206 && (offset < 0 || h->range_start != offset || h->range_end < 0 ||
            (h->length >= 0 && h->length != h->range_end - h->range_start + 1))) {
            fail(h, "Server returned an invalid byte range"); break;
        }
        if (offset > 0 && h->status != 206) { fail(h, "Server ignored the requested byte range"); break; }
        if (h->status == 204) h->eof = 1;
        ret = 0;
        break;
    }
done:
    if (ret) gc_http_close(h);
    free(u); free(request); free(next);
    return ret;
}

int gc_http_read(gc_http *h, void *buf, int len)
{
    int n;
    if (h->failed) return -1;
    if (h->eof || len <= 0) return 0;
    if (h->chunked) {
        if (!h->chunk_left) {
            char text[256], *end;
            if (h->chunk_crlf && (line(h, text, sizeof(text)) != 1)) return fail(h, "Invalid chunk boundary");
            if (line(h, text, sizeof(text)) < 0) return -1;
            if (!isxdigit((unsigned char)text[0])) return fail(h, "Invalid chunk size");
            errno = 0;
            unsigned long long size = strtoull(text, &end, 16);
            if (errno || size > INT64_MAX || (*end && *end != ';')) return fail(h, "Invalid chunk size");
            h->chunk_left = size;
            h->chunk_crlf = 1;
            if (!size) {
                int total = 0;
                do {
                    n = line(h, text, sizeof(text));
                    if (n < 0 || (total += n) > 16384) return fail(h, "Invalid HTTP trailers");
                } while (n > 1);
                h->eof = 1;
                return 0;
            }
        }
        if (len > h->chunk_left) len = h->chunk_left;
    } else if (h->remaining >= 0) {
        if (!h->remaining) { h->eof = 1; return 0; }
        if (len > h->remaining) len = h->remaining;
    }
    n = raw_read(h, buf, len);
    if (n < 0) return n;
    if (!n) {
        if (h->chunked || h->remaining > 0) return fail(h, "Truncated HTTP body");
        h->eof = 1;
    }
    if (h->chunked) h->chunk_left -= n;
    else if (h->remaining >= 0) h->remaining -= n;
    return n;
}

int gc_http_basic(const char *user, const char *password, char *out, size_t cap)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char plain[768];
    size_t n, j = 0;
    if (strchr(user, ':') || !safe_text(user) || !safe_text(password)) return -1;
    int size = snprintf(plain, sizeof(plain), "%s:%s", user, password);
    if (size < 0 || size >= (int)sizeof(plain)) return -1;
    n = size;
    if (((n + 2) / 3) * 4 + 1 > cap) return -1;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)plain[i] << 16;
        if (i+1 < n) v |= (unsigned char)plain[i+1] << 8;
        if (i+2 < n) v |= (unsigned char)plain[i+2];
        out[j++] = alphabet[v >> 18]; out[j++] = alphabet[(v >> 12) & 63];
        out[j++] = i+1 < n ? alphabet[(v >> 6) & 63] : '=';
        out[j++] = i+2 < n ? alphabet[v & 63] : '=';
    }
    out[j] = 0;
    memset(plain, 0, sizeof(plain));
    return 0;
}

int gc_http_encode_path(const char *path, char *out, size_t cap)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        int literal = isalnum(*p) && *p < 128;
        literal |= strchr("/-._~", *p) != NULL;
        if (j + (literal ? 1 : 3) >= cap) return -1;
        if (literal) out[j++] = *p;
        else { out[j++] = '%'; out[j++] = hex[*p >> 4]; out[j++] = hex[*p & 15]; }
    }
    out[j] = 0; return 0;
}
static int hexval(unsigned char c)
{ if (c >= '0' && c <= '9') return c-'0'; c = tolower(c); return c >= 'a' && c <= 'f' ? c-'a'+10 : -1; }
int gc_http_decode_path(const char *path, char *out, size_t cap)
{
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        unsigned char c = *p;
        if (c == '%') {
            if (!p[1] || !p[2] || hexval(p[1]) < 0 || hexval(p[2]) < 0) return -1;
            c = hexval(p[1]) * 16 + hexval(p[2]);
            p += 2;
            /* Encoded separators would alias a different DAV resource. */
            if (c == '/' || c == '\\') return -1;
        }
        if (c < 32 || c == 127 || c == '\\' || j + 1 >= cap) return -1;
        out[j++] = c;
    }
    out[j] = 0; return 0;
}
