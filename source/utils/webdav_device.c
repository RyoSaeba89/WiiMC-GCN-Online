#include "webdav_device.h"
#include "webdav_client.h"
#include "playback_buffer.h"
#include "debuglog.h"
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static gc_dav_config config;
static char last_error[160];
static int mounted;

/* dav1: is reachable from the GUI thread (browsing, artwork, playlists) and
 * from MPlayer's cache thread at the same time. The directory cache, its byte
 * counter, its LRU clock and the last error string are plain globals: two
 * concurrent operations could leave the counter wrong, free a list another
 * caller was copying, or hand one caller's error text to the other. One lock
 * covers all of it; it is never held across a network call. */
static mutex_t dav_lock = LWP_MUTEX_NULL;

#define DAV_LOCK()   do { if(dav_lock != LWP_MUTEX_NULL) LWP_MutexLock(dav_lock); } while(0)
#define DAV_UNLOCK() do { if(dav_lock != LWP_MUTEX_NULL) LWP_MutexUnlock(dav_lock); } while(0)

/* Each operation builds its error in a local buffer and publishes it here, so
 * the string a caller reports is the one its own call produced. */
static void set_error(const char *text)
{
    DAV_LOCK();
    snprintf(last_error, sizeof(last_error), "%s", text ? text : "");
    DAV_UNLOCK();
}
typedef struct { gc_dav_entry *entries; size_t count, index; } dav_dir;

/* Directory snapshots are deliberately separate from the browser's current
 * list.  A return to a visited directory therefore needs no PROPFIND, XML
 * parse or sort while audio is playing. */
#define DAV_DIRECTORY_CACHE_LIMIT (1024 * 1024)
#define DAV_AUDIO_RESUME_PERCENT 60.0f
typedef struct dav_cache_entry {
    char *path;
    gc_dav_entry *entries;
    size_t count, bytes;
    unsigned long stamp;
    struct dav_cache_entry *next;
} dav_cache_entry;
static dav_cache_entry *directory_cache;
static size_t directory_cache_bytes;
static unsigned long directory_cache_clock;
static volatile int playback_files;
static volatile int playback_cache_ready;
extern float MPlayerCacheFillPercent(void);
extern volatile int stop_cache_thread;

/* Called only from cache_store(), which holds the lock. */
static void cache_remove(dav_cache_entry *victim)
{
    dav_cache_entry **link = &directory_cache;
    while(*link && *link != victim) link = &(*link)->next;
    if(!*link) return;
    *link = victim->next;
    directory_cache_bytes -= victim->bytes;
    free(victim->entries);
    free(victim->path);
    free(victim);
}

static int cache_copy(const char *path, gc_dav_entry **entries, size_t *count)
{
    int result = 0;

    DAV_LOCK();
    for(dav_cache_entry *item = directory_cache; item; item = item->next)
    {
        if(strcmp(item->path, path)) continue;
        gc_dav_entry *copy = NULL;
        if(item->count)
        {
            /* Copied under the lock: without it the entries could be freed
             * by another caller's eviction halfway through the memcpy. */
            copy = malloc(item->count * sizeof(*copy));
            if(!copy) { result = -1; break; }
            memcpy(copy, item->entries, item->count * sizeof(*copy));
        }
        item->stamp = ++directory_cache_clock;
        *entries = copy;
        *count = item->count;
        result = 1;
        break;
    }
    DAV_UNLOCK();
    return result;
}

static void cache_store(const char *path, const gc_dav_entry *entries, size_t count)
{
    size_t bytes = sizeof(dav_cache_entry) + strlen(path) + 1 + count * sizeof(*entries);
    if(bytes > DAV_DIRECTORY_CACHE_LIMIT) return;

    DAV_LOCK();
    while(directory_cache_bytes + bytes > DAV_DIRECTORY_CACHE_LIMIT)
    {
        dav_cache_entry *victim = NULL;
        for(dav_cache_entry *item = directory_cache; item; item = item->next)
        {
            /* The large root is the most expensive listing and the most
             * frequent return destination, so keep it for the whole run. */
            if(!strcmp(item->path, "/")) continue;
            if(!victim || item->stamp < victim->stamp) victim = item;
        }
        if(!victim) { DAV_UNLOCK(); return; }
        cache_remove(victim);
    }

    dav_cache_entry *item = calloc(1, sizeof(*item));
    if(!item) { DAV_UNLOCK(); return; }
    item->path = strdup(path);
    if(count) item->entries = malloc(count * sizeof(*entries));
    if(!item->path || (count && !item->entries))
    {
        free(item->entries); free(item->path); free(item); DAV_UNLOCK(); return;
    }
    if(count) memcpy(item->entries, entries, count * sizeof(*entries));
    item->count = count;
    item->bytes = bytes;
    item->stamp = ++directory_cache_clock;
    item->next = directory_cache;
    directory_cache = item;
    directory_cache_bytes += bytes;

    size_t total = directory_cache_bytes;
    DAV_UNLOCK();

    DebugMark("webdav: cached '%s' (%u entries, %u KiB total)", path,
        (unsigned)count, (unsigned)((total + 1023) / 1024));
}

/* Metadata is allowed onto the network only with at least 60% of the real
 * audio cache available. The callback is invoked before PROPFIND,
 * before each 4 KiB body read and periodically during XML extraction. */
static void wait_for_audio_priority(void *unused)
{
    (void)unused;
    /* open() also serves probes, playlists and artwork. Those never acquire
     * an MPlayer cache, so waiting for CacheReady from open() can deadlock.
     * Gate metadata only once an actual playback cache has been published. */
    if(!playback_files || !playback_cache_ready) return;
    u64 started = gettime();
    int waited = 0;
    float fill = -1;
    while(playback_files && playback_cache_ready && !stop_cache_thread)
    {
        fill = MPlayerCacheFillPercent();
        if(playback_cache_ready && (fill < 0 || fill >= DAV_AUDIO_RESUME_PERCENT)) break;
        waited = 1;
        usleep(10 * 1000);
    }
    if(waited)
        DebugMark("webdav: metadata waited %u ms for audio cache (%.1f%%)",
            (unsigned)ticks_to_millisecs(gettime() - started), fill);
}

/* The cancel token the transport asks before connecting and between socket
 * operations.
 *
 * It speaks only for playback. stop_cache_thread is 1 whenever no cache is
 * running at all -- which is the normal state while the user is browsing --
 * so answering on that alone would cancel every directory listing before it
 * left the console. A published playback cache being told to stop is the one
 * case that matters: that is when cache_uninit() is waiting for a worker
 * parked in a GET, which is what used to hold a track change open for as
 * long as the server felt like. */
static int playback_cancelled(void *unused)
{
    (void)unused;
    return playback_cache_ready && stop_cache_thread;
}

void WebDAVPlaybackCacheReady(void)
{
    playback_cache_ready = 1;
    DebugMark("webdav: %u KiB playback cache ready (%.1f%% filled)",
        DAV_AUDIO_CACHE_BYTES / 1024, MPlayerCacheFillPercent());
}

/* Publishes this call's own error, then reports it. */
static int failure(struct _reent *r, const char *text)
{
    set_error(text);
    r->_errno = errno ? errno : EIO;
    DebugMark("webdav: %s (errno %d)", text ? text : "", r->_errno);
    return -1;
}
static const char *local_path(const char *path)
{ return !strncmp(path, "dav1:/", 6) ? path+5 : path; }
static void fill_stat(const gc_dav_entry *e, struct stat *s)
{
    memset(s, 0, sizeof(*s));
    s->st_mode = (e->directory ? S_IFDIR | 0555 : S_IFREG | 0444);
    s->st_nlink = 1; s->st_size = e->size;
}
static int dav_open(struct _reent *r, void *file, const char *path, int flags, int mode)
{
    (void)mode;
    if ((flags & O_ACCMODE) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) { r->_errno = EROFS; return -1; }
    gc_dav_file *f = file;
    char err[160] = "";
    if (gc_dav_open(f, &config, local_path(path), err, sizeof(err))) return failure(r, err);
    f->playback_registered = 1;
    if(playback_files++ == 0) playback_cache_ready = 0;
    DebugMark("webdav: playback opened, metadata priority gate armed");
    return 0;
}
static int dav_close(struct _reent *r, void *file)
{
    (void)r;
    gc_dav_file *f = file;
    if(f->playback_registered)
    {
        f->playback_registered = 0;
        if(playback_files > 0) playback_files--;
        if(!playback_files) playback_cache_ready = 0;
        DebugMark("webdav: playback closed, metadata priority gate released");
    }
    gc_dav_close(f);
    return 0;
}
static ssize_t dav_read(struct _reent *r, void *file, char *buffer, size_t len)
{
    int n = gc_dav_read(file, buffer, len > INT_MAX ? INT_MAX : len);
    if (n < 0) {
        gc_dav_file *f = file;
        return failure(r, f->http ? f->http->error : "WebDAV read failed");
    }
    return n;
}
static off_t dav_seek(struct _reent *r, void *file, off_t offset, int whence)
{ int64_t n = gc_dav_seek(file, offset, whence); if (n < 0) failure(r, "WebDAV seek failed"); return n; }
static int dav_fstat(struct _reent *r, void *file, struct stat *s)
{ (void)r; gc_dav_file *f = file; gc_dav_entry e = {{0}, 0, f->size}; fill_stat(&e, s); return 0; }
static int dav_stat(struct _reent *r, const char *path, struct stat *s)
{
    gc_dav_entry e;
    char err[160] = "";
    if (gc_dav_stat(&config, local_path(path), &e, err, sizeof(err))) return failure(r, err);
    fill_stat(&e, s); return 0;
}
static int dav_chdir(struct _reent *r, const char *path)
{ struct stat s; if (dav_stat(r, path, &s)) return -1; if (!S_ISDIR(s.st_mode)) { r->_errno = ENOTDIR; return -1; } return 0; }
static DIR_ITER *dav_diropen(struct _reent *r, DIR_ITER *it, const char *path)
{
    dav_dir *d = it->dirStruct;
    const char *remote_path = local_path(path);
    u64 started = gettime();
    memset(d, 0, sizeof(*d));
    char err[160] = "";
    int cached = cache_copy(remote_path, &d->entries, &d->count);
    if(cached < 0) { errno = ENOMEM; failure(r, "Out of memory copying WebDAV directory cache"); return NULL; }
    if(cached)
    {
        DebugMark("webdav: cache hit '%s' (%u entries, %u ms)", remote_path,
            (unsigned)d->count, (unsigned)ticks_to_millisecs(gettime() - started));
        return it;
    }
    if (gc_dav_list_wait(&config, remote_path, &d->entries, &d->count,
            wait_for_audio_priority, NULL, err, sizeof(err))) { failure(r, err); return NULL; }
    cache_store(remote_path, d->entries, d->count);
    DebugMark("webdav: listed %u entries in %u ms", (unsigned)d->count,
        (unsigned)ticks_to_millisecs(gettime() - started));
    return it;
}
static int dav_dirreset(struct _reent *r, DIR_ITER *it) { (void)r; ((dav_dir *)it->dirStruct)->index = 0; return 0; }
static int dav_dirnext(struct _reent *r, DIR_ITER *it, char *name, struct stat *s)
{
    dav_dir *d = it->dirStruct;
    if (d->index == d->count) { r->_errno = ENOENT; return -1; }
    gc_dav_entry *e = d->entries + d->index++;
    strcpy(name, e->name); fill_stat(e, s); return 0;
}
static int dav_dirclose(struct _reent *r, DIR_ITER *it)
{ (void)r; dav_dir *d = it->dirStruct; free(d->entries); d->entries = NULL; return 0; }
static const devoptab_t dav_device = {
    .name = "dav1", .structSize = sizeof(gc_dav_file),
    .open_r = dav_open, .close_r = dav_close, .read_r = dav_read,
    .seek_r = dav_seek, .fstat_r = dav_fstat, .stat_r = dav_stat, .chdir_r = dav_chdir,
    .dirStateSize = sizeof(dav_dir), .diropen_r = dav_diropen,
    .dirreset_r = dav_dirreset, .dirnext_r = dav_dirnext, .dirclose_r = dav_dirclose,
    .lstat_r = dav_stat
};
void WebDAVInit(const char *app_path)
{
    char path[1100];
    snprintf(path, sizeof(path), "%s/webdav.conf", app_path);
    if (dav_lock == LWP_MUTEX_NULL)
        LWP_MutexInit(&dav_lock, false);

    config.cancel = playback_cancelled;
    config.cancel_opaque = NULL;

    char err[160] = "";
    if (gc_dav_config_load(&config, path, err, sizeof(err))) {
        set_error(err);
        DebugMark("webdav: %s", err); return;
    }
    if (AddDevice(&dav_device) < 0) { set_error("Cannot register dav1:"); return; }
    mounted = 1;
    set_error("");
    DebugMark("webdav: dav1: registered (read only)");
}
int WebDAVConfigured(void) { return mounted; }
const char *WebDAVName(void) { return config.name; }
const char *WebDAVError(void) { return last_error; }
