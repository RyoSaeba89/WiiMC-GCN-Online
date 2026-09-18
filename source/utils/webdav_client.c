/* Read-only WebDAV (RFC 4918). XML uses the already linked Mini-XML library. */
#include "webdav_client.h"
#include <mxml.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int error_out(char *out, size_t cap, int err, const char *text)
{ snprintf(out, cap, "%s", text); errno = err; return -1; }
static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) ++s;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}
int gc_dav_config_load(gc_dav_config *config, const char *file, char *error, size_t cap)
{
    FILE *f = fopen(file, "rb");
    char *line = malloc(4096), user[256] = "", password[256] = "";
    gc_http_url *url = malloc(sizeof(*url));
    int ret = -1, lines = 0;
    memset(config, 0, sizeof(*config));
    strcpy(config->name, "WebDAV");
    if (!f) { error_out(error, cap, ENOENT, "No webdav.conf"); goto done; }
    if (!line || !url) { error_out(error, cap, ENOMEM, "Out of memory"); goto done; }
    while (fgets(line, 4096, f)) {
        if (++lines > 64 || (!strchr(line, '\n') && !feof(f))) { error_out(error, cap, EINVAL, "WebDAV configuration is too large"); goto done; }
        char *key = trim(line), *value, *dest;
        size_t size;
        if (!*key || *key == '#' || *key == ';') continue;
        value = strchr(key, '=');
        if (!value) { error_out(error, cap, EINVAL, "Expected key=value in webdav.conf"); goto done; }
        *value++ = 0;
        key = trim(key); value = trim(value);
        if (!strcmp(key, "url")) { dest = config->url; size = sizeof(config->url); }
        else if (!strcmp(key, "name")) { dest = config->name; size = sizeof(config->name); }
        else if (!strcmp(key, "username")) { dest = user; size = sizeof(user); }
        else if (!strcmp(key, "password")) { dest = password; size = sizeof(password); }
        else { error_out(error, cap, EINVAL, "Unknown key in webdav.conf"); goto done; }
        if (strlen(value) >= size) { error_out(error, cap, EINVAL, "WebDAV setting is too long"); goto done; }
        strcpy(dest, value);
    }
    if (gc_http_parse_url(config->url, url) || url->tls || strchr(url->path, '?') || strchr(config->url, '#')) {
        error_out(error, cap, EINVAL, "WebDAV requires an http:// LAN URL without query or credentials"); goto done;
    }
    size_t n = strlen(config->url);
    if (n + 1 >= sizeof(config->url)) { error_out(error, cap, EINVAL, "WebDAV URL is too long"); goto done; }
    if (config->url[n-1] != '/') strcat(config->url, "/");
    if ((user[0] || password[0]) && gc_http_basic(user, password, config->authorization, sizeof(config->authorization))) {
        error_out(error, cap, EINVAL, "Invalid WebDAV credentials"); goto done;
    }
    ret = 0;
done:
    if (f) fclose(f);
    free(line); free(url);
    memset(password, 0, sizeof(password));
    if (ret) memset(config, 0, sizeof(*config));
    return ret;
}

static int valid_path(const char *path)
{
    if (*path != '/') return 0;
    while (*path) {
        if (*path == '/' && (path[1] == '/' || (path[1] == '.' && (path[2] == 0 || path[2] == '/' ||
            (path[2] == '.' && (path[3] == 0 || path[3] == '/')))))) return 0;
        if ((unsigned char)*path < 32 || *path == '\\') return 0;
        ++path;
    }
    return 1;
}
static int make_url(const gc_dav_config *c, const char *path, char *url, size_t cap)
{
    char *encoded = malloc(GC_HTTP_URL_MAX);
    int ret = -1;
    if (!encoded) { errno = ENOMEM; return -1; }
    if (valid_path(path) && !gc_http_encode_path(path+1, encoded, GC_HTTP_URL_MAX)) {
        int n = snprintf(url, cap, "%s%s", c->url, encoded);
        if (n >= 0 && (size_t)n < cap) ret = 0;
    }
    free(encoded);
    if (ret) errno = EINVAL;
    return ret;
}

/* Prefixes have no meaning on their own. Resolve each element's namespace
 * declaration, including inherited default namespaces, to the DAV: URI. */
static int dav_node(mxml_node_t *node, const char *name)
{
    const char *tag = mxmlGetElement(node), *colon;
    char key[128];
    if (!tag) return 0;
    colon = strchr(tag, ':');
    if (strcmp(colon ? colon+1 : tag, name)) return 0;
    if (colon) {
        size_t n = colon-tag;
        if (n + 7 > sizeof(key)) return 0;
        snprintf(key, sizeof(key), "xmlns:%.*s", (int)n, tag);
    } else strcpy(key, "xmlns");
    for (mxml_node_t *p = node; p; p = mxmlGetParent(p)) {
        if (!mxmlGetElement(p)) continue;
        const char *ns = mxmlElementGetAttr(p, key);
        if (ns) return !strcmp(ns, "DAV:");
    }
    return 0;
}
static mxml_node_t *child(mxml_node_t *node, const char *name)
{
    for (mxml_node_t *p = mxmlGetFirstChild(node); p; p = mxmlGetNextSibling(p))
        if (dav_node(p, name)) return p;
    return NULL;
}
static const char *text_of(mxml_node_t *node)
{
    mxml_node_t *t = node ? mxmlGetFirstChild(node) : NULL;
    return t ? mxmlGetOpaque(t) : NULL;
}
static int success(mxml_node_t *node)
{
    const char *s = text_of(child(node, "status"));
    int status;
    return s && sscanf(s, "HTTP/1.%*d %d", &status) == 1 && status >= 200 && status < 300;
}

static unsigned name_hash(const char *name)
{
    unsigned hash = 2166136261u;
    while (*name) { hash ^= (unsigned char)*name++; hash *= 16777619u; }
    return hash;
}

static int query(const gc_dav_config *c, const char *path, int depth, gc_dav_entry **out,
    size_t *count, gc_dav_wait_fn wait, void *opaque, char *error, size_t cap)
{
    static const char body[] = "<?xml version=\"1.0\"?><D:propfind xmlns:D=\"DAV:\"><D:prop><D:resourcetype/><D:getcontentlength/></D:prop></D:propfind>";
    gc_http *h = calloc(1, sizeof(*h));
    gc_http_url *urls = calloc(2, sizeof(*urls));
    char *target = malloc(GC_HTTP_URL_MAX), *href = malloc(GC_HTTP_URL_MAX), *basepath = malloc(2048), *itempath = malloc(2048);
    char *xml = NULL;
    uint16_t *name_slots = calloc(2048, sizeof(*name_slots));
    mxml_node_t *tree = NULL;
    gc_dav_entry *entries = NULL;
    size_t used = 0, allocated = 0, entry_capacity = 0;
    int ret = -1;
    *out = NULL; *count = 0;
    if (!h || !urls || !target || !href || !basepath || !itempath || !name_slots) { error_out(error, cap, ENOMEM, "Out of memory"); goto done; }
    if (make_url(c, path, target, GC_HTTP_URL_MAX)) { error_out(error, cap, EINVAL, "Invalid WebDAV path"); goto done; }
    if (wait) wait(opaque);
    if (gc_http_open(h, target, "PROPFIND", c->authorization, body, depth, -1, 0, NULL, NULL)) {
        error_out(error, cap, h->status == 404 ? ENOENT : h->status == 401 || h->status == 403 ? EACCES : EIO, h->error); goto done;
    }
    if (h->status != 207) { error_out(error, cap, EIO, "Server does not support WebDAV PROPFIND (expected HTTP 207)"); goto done; }
    /* Use the effective URL after a trailing-slash redirect for href matching. */
    if (gc_http_parse_url(h->url, urls) || gc_http_decode_path(urls[0].path, basepath, 2048) || !valid_path(basepath)) {
        error_out(error, cap, EIO, "Invalid WebDAV response URL"); goto done;
    }
    size_t base_len = strlen(basepath);
    while (base_len > 1 && basepath[base_len-1] == '/') basepath[--base_len] = 0;
    while (1) {
        if (used + 4096 + 1 > allocated) {
            if (allocated >= 512*1024+1) { error_out(error, cap, EOVERFLOW, "WebDAV listing exceeds 512 KiB"); goto done; }
            allocated = allocated ? allocated * 2 : 8192;
            if (allocated > 512*1024+1) allocated = 512*1024+1;
            char *next = realloc(xml, allocated);
            if (!next) { error_out(error, cap, ENOMEM, "Out of memory"); goto done; }
            xml = next;
        }
        if (wait) wait(opaque);
        int n = gc_http_read(h, xml+used, allocated-used-1);
        if (n < 0) { error_out(error, cap, EIO, h->error); goto done; }
        if (!n) break;
        used += n;
    }
    xml[used] = 0;
    /* DAV needs no DTD. Reject declarations before giving XML to Mini-XML. */
    if (strstr(xml, "<!DOCTYPE") || strstr(xml, "<!ENTITY") || memchr(xml, 0, used)) {
        error_out(error, cap, EINVAL, "Unsupported WebDAV XML declaration"); goto done;
    }
    if (wait) wait(opaque);
    tree = mxmlLoadString(NULL, xml, MXML_OPAQUE_CALLBACK);
    free(xml); xml = NULL;
    if (!tree) { error_out(error, cap, EIO, "Invalid WebDAV XML"); goto done; }
    mxml_node_t *multi = tree;
    if (!dav_node(multi, "multistatus")) multi = child(tree, "multistatus");
    if (!multi) { error_out(error, cap, EIO, "Missing DAV:multistatus"); goto done; }
    for (mxml_node_t *response = mxmlGetFirstChild(multi); response; response = mxmlGetNextSibling(response)) {
        if (wait && ((*count & 31) == 0)) wait(opaque);
        if (!dav_node(response, "response")) continue;
        const char *value = text_of(child(response, "href"));
        if (!value || gc_http_resolve_url(h->url, value, href, GC_HTTP_URL_MAX) || gc_http_parse_url(href, urls+1)) continue;
        if (urls[0].tls != urls[1].tls || urls[0].port != urls[1].port || strcasecmp(urls[0].host, urls[1].host) ||
            strchr(urls[1].path, '?') || gc_http_decode_path(urls[1].path, itempath, 2048) || !valid_path(itempath)) continue;
        size_t len = strlen(itempath);
        while (len > 1 && itempath[len-1] == '/') itempath[--len] = 0;
        const char *name;
        if (!depth) { if (strcmp(itempath, basepath)) continue; name = strrchr(itempath, '/') + 1; }
        else {
            if (!strcmp(itempath, basepath)) continue;
            if (base_len == 1) name = itempath + 1;
            else {
                if (strncmp(itempath, basepath, base_len) || itempath[base_len] != '/') continue;
                name = itempath + base_len + 1;
            }
            if (!*name || strchr(name, '/')) continue;
        }
        if (strlen(name) >= sizeof(((gc_dav_entry *)0)->name)) { error_out(error, cap, ENAMETOOLONG, "WebDAV filename exceeds 255 bytes"); goto done; }
        gc_dav_entry entry = {{0}, 0, -1};
        int found_type = 0;
        for (mxml_node_t *propstat = mxmlGetFirstChild(response); propstat; propstat = mxmlGetNextSibling(propstat)) {
            if (!dav_node(propstat, "propstat") || !success(propstat)) continue;
            mxml_node_t *prop = child(propstat, "prop");
            if (!prop) continue;
            mxml_node_t *type = child(prop, "resourcetype");
            if (type) { entry.directory = child(type, "collection") != NULL; found_type = 1; }
            value = text_of(child(prop, "getcontentlength"));
            if (value) {
                char *end;
                errno = 0;
                long long size = strtoll(value, &end, 10);
                if (!errno && end != value && !*end && size >= 0) entry.size = size;
            }
        }
        if (!found_type || (!entry.directory && entry.size < 0)) continue;
        if (entry.directory) entry.size = 0;
        strcpy(entry.name, name);
        int duplicate = 0;
        unsigned slot = name_hash(name) & 2047u;
        while (name_slots[slot]) {
            if (!strcmp(entries[name_slots[slot] - 1].name, name)) { duplicate = 1; break; }
            slot = (slot + 1) & 2047u;
        }
        if (duplicate) continue;
        if (*count >= GC_DAV_MAX_ENTRIES) { error_out(error, cap, EOVERFLOW, "WebDAV folder exceeds 1024 entries"); goto done; }
        if (*count == entry_capacity) {
            size_t next_capacity = entry_capacity ? entry_capacity * 2 : 32;
            if (next_capacity > GC_DAV_MAX_ENTRIES) next_capacity = GC_DAV_MAX_ENTRIES;
            gc_dav_entry *next = realloc(entries, next_capacity * sizeof(entry));
            if (!next) { error_out(error, cap, ENOMEM, "Out of memory"); goto done; }
            entries = next;
            entry_capacity = next_capacity;
        }
        entries[*count] = entry;
        name_slots[slot] = (uint16_t)(*count + 1);
        (*count)++;
    }
    if (!depth && *count != 1) { error_out(error, cap, ENOENT, "WebDAV resource not found"); goto done; }
    *out = entries; entries = NULL; ret = 0;
done:
    gc_http_close(h); free(h); free(urls); free(target); free(href); free(basepath); free(itempath); free(xml); free(name_slots); free(entries);
    if (tree) mxmlDelete(tree);
    if (ret) *count = 0;
    return ret;
}
int gc_dav_list(const gc_dav_config *c, const char *path, gc_dav_entry **out, size_t *count, char *error, size_t cap)
{ return query(c, path, 1, out, count, NULL, NULL, error, cap); }
int gc_dav_list_wait(const gc_dav_config *c, const char *path, gc_dav_entry **out,
    size_t *count, gc_dav_wait_fn wait, void *opaque, char *error, size_t cap)
{ return query(c, path, 1, out, count, wait, opaque, error, cap); }
int gc_dav_stat(const gc_dav_config *c, const char *path, gc_dav_entry *out, char *error, size_t cap)
{
    gc_dav_entry *entries;
    size_t count;
    if (query(c, path, 0, &entries, &count, NULL, NULL, error, cap)) return -1;
    *out = entries[0]; free(entries); return 0;
}
int gc_dav_open(gc_dav_file *f, const gc_dav_config *c, const char *path, char *error, size_t cap)
{
    gc_dav_entry stat;
    memset(f, 0, sizeof(*f));
    if (gc_dav_stat(c, path, &stat, error, cap)) return -1;
    if (stat.directory) return error_out(error, cap, EISDIR, "Cannot play a WebDAV directory");
    if (make_url(c, path, f->url, sizeof(f->url))) return error_out(error, cap, EINVAL, "Invalid WebDAV path");
    f->size = stat.size;
    f->config = c;
    f->http = calloc(1, sizeof(*f->http));
    if (!f->http) return error_out(error, cap, ENOMEM, "Out of memory");
    return 0;
}
int gc_dav_read(gc_dav_file *f, void *buffer, int len)
{
    if (!f->http) { errno = EBADF; return -1; }
    if (len <= 0 || f->position >= f->size) return 0;
    if (!f->http->conn) {
        if (gc_http_open(f->http, f->url, "GET", f->config->authorization, NULL, -1, f->position, 0, NULL, NULL)) { errno = EIO; return -1; }
        if ((f->http->total >= 0 && f->http->total != f->size) ||
            (f->http->length >= 0 && f->http->length != f->size - f->position)) {
            gc_http_close(f->http); errno = EIO; return -1;
        }
    }
    if (len > f->size - f->position) len = f->size - f->position;
    int n = gc_http_read(f->http, buffer, len);
    if (n <= 0) { errno = EIO; return -1; }
    f->position += n;
    return n;
}
int64_t gc_dav_seek(gc_dav_file *f, int64_t offset, int whence)
{
    int64_t base;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = f->position;
    else if (whence == SEEK_END) base = f->size;
    else { errno = EINVAL; return -1; }
    if ((offset > 0 && base > INT64_MAX - offset) || (offset < 0 && offset < -base)) { errno = EINVAL; return -1; }
    int64_t position = base + offset;
    if (position != f->position) gc_http_close(f->http);
    f->position = position;
    return position;
}
void gc_dav_close(gc_dav_file *f)
{ gc_http_close(f->http); free(f->http); f->http = NULL; }
