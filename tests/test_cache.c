/* Execute the real GEKKO cache implementation with a pthread-backed LWP and
 * a seekable, delayed source. No replacement implementation of the ring. */
#include "../source/mplayer/stream/cache2.c"
#include "../source/utils/playback_buffer.h"
#include <assert.h>
#include <limits.h>
#include <stdarg.h>

int getMESS, getWeird, waitReload, cntReconnect, controlledbygui;
char fileplaying[MAXPATHLEN];
static pthread_mutex_t worker_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t worker_wake = PTHREAD_COND_INITIALIZER;
static int suspended, polls, cancel_prefill, unavailable, read_delay;
static int progress_active, progress_shows, progress_finishes, cancel_on_progress;

void SuspendCacheThread(void)
{
    pthread_mutex_lock(&worker_lock);
    suspended = 1;
    while (suspended) pthread_cond_wait(&worker_wake, &worker_lock);
    pthread_mutex_unlock(&worker_lock);
}
void ResumeCacheThread(void)
{
    pthread_mutex_lock(&worker_lock);
    suspended = 0;
    pthread_cond_signal(&worker_wake);
    pthread_mutex_unlock(&worker_lock);
}
bool CacheThreadSuspended(void)
{
    int result;
    pthread_mutex_lock(&worker_lock);
    result = suspended;
    pthread_mutex_unlock(&worker_lock);
    return result;
}
bool CacheThreadAvailable(void) { return !unavailable; }
void CheckMplayerNetwork(void) { assert(!"unexpected SMB recovery"); }
void ShowProgress(const char *msg, int done, int total)
{
    (void)msg;
    assert(done >= 0 && total > 0);
    progress_active = 1;
    ++progress_shows;
    if (cancel_on_progress) cancel_prefill = 1;
}
void FinishBufferingProgress(void)
{
    assert(progress_active);
    progress_active = 0;
    ++progress_finishes;
}
void mp_msg(int mod, int lev, const char *fmt, ...)
{ (void)mod; (void)lev; (void)fmt; }
int stream_check_interrupt(int delay)
{
    /* Deliberately does NOT sleep: input backends may return immediately. */
    (void)delay;
    assert(++polls < 1000);
    return cancel_prefill;
}
static unsigned char sample(off_t pos) { return (pos * 17 + pos / 251) & 255; }
int stream_read_internal(stream_t *s, void *dst, int len)
{
    if (read_delay) usleep(read_delay);
    if (s->pos + len > s->end_pos) len = s->end_pos - s->pos;
    for (int i = 0; i < len; ++i) ((unsigned char *)dst)[i] = sample(s->pos + i);
    s->pos += len;
    return len;
}
int stream_seek_internal(stream_t *s, off_t pos) { s->pos = pos; return 1; }
void stream_reset(stream_t *s) { s->eof = 0; }
int stream_fill_buffer(stream_t *s) { (void)s; assert(0); return 0; }
int stream_seek_long(stream_t *s, off_t pos) { return stream_seek_internal(s, pos); }
static void verify_read(stream_t *s, off_t pos, int len)
{
    unsigned char data[4096];
    while (len) {
        int n = len < (int)sizeof(data) ? len : (int)sizeof(data);
        assert(stream_read(s, (char *)data, n) == n);
        for (int i = 0; i < n; ++i) assert(data[i] == sample(pos + i));
        pos += n; len -= n;
    }
}
int main(void)
{
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, mplayercachethread, NULL));
    while (!CacheThreadSuspended()) usleep(1000);
    stream_t s = {0};
    s.type = STREAMTYPE_FILE;
    s.read_chunk = 16384;
    s.end_pos = 6 * 1024 * 1024;
    read_delay = 1000;
    assert(stream_enable_cache(&s, 2 * 1024 * 1024, 1024 * 1024, 0) == 1);
    assert(progress_shows > 0 && progress_finishes == 1 && !progress_active);
    assert(MPlayerCacheFillPercent() >= 50);
    verify_read(&s, 0, 4 * 1024 * 1024); /* wrap the ring twice */
    assert(cache_stream_seek_long(&s, 12345));
    verify_read(&s, 12345, 65536);
    assert(cache_stream_seek_long(&s, s.end_pos - 4096));
    verify_read(&s, s.end_pos - 4096, 4096);
    assert(!cache_stream_fill_buffer(&s));
    assert(cache_stream_seek_long(&s, 0)); /* recover from EOF */
    verify_read(&s, 0, 4096);
    read_delay = 1000000;
    assert(cache_stream_seek_long(&s, 3 * 1024 * 1024));
    verify_read(&s, 3 * 1024 * 1024, 4096);
    read_delay = 1000; /* an 800 ms gap must not terminate the track */
    getMESS = 1;
    cache_uninit(&s); /* stop must join, even during a GUI track change */
    assert(!s.cache_data && !s.cache_pid && CacheThreadSuspended());
    getMESS = 0;
    puts("PASS 2 MiB prefill, wrap, seeks, EOF recovery and stop");

    memset(&s, 0, sizeof(s)); s.end_pos = 3000;
    polls = 0;
    assert(stream_enable_cache(&s, 2 * 1024 * 1024, 1024 * 1024, 0) == 1);
    verify_read(&s, 0, 3000);
    cache_uninit(&s);
    puts("PASS short file and second-track cache lifetime");

    const int bitrates[] = {64000, 128000, 192000, 256000, 320000};
    for (unsigned i = 0; i < sizeof(bitrates) / sizeof(bitrates[0]); ++i) {
        int target = dav_audio_start_bytes(bitrates[i] / 8);
        assert(target == bitrates[i] / 8 * 3);
        memset(&s, 0, sizeof(s));
        s.type = STREAMTYPE_FILE;
        s.read_chunk = 16384;
        s.end_pos = 6 * 1024 * 1024;
        polls = 0;
        assert(stream_enable_cache(&s, DAV_AUDIO_CACHE_BYTES, DAV_AUDIO_PROBE_BYTES, 0) == 1);
        cache_vars_t *ring = s.cache_data;
        assert(ring->buffer_size == 512 * 1024);
        /* MP3 tag detection seeks to the end, then back to audio. Do not
         * count the discarded prefix toward the actual startup target. */
        assert(cache_stream_seek_long(&s, s.end_pos - 128));
        verify_read(&s, s.end_pos - 128, 128);
        assert(cache_stream_seek_long(&s, 2048));
        assert(stream_cache_prefill(&s, target) == 1);
        assert(s.cache_data == ring);
        assert(ring->max_filepos - ring->read_filepos >= target);
        verify_read(&s, 2048, 1024 * 1024); /* wrap the smaller ring twice */
        /* An oversized refill must not wait for space retained as history. */
        assert(stream_cache_prefill(&s, DAV_AUDIO_CACHE_BYTES) == 1);
        controlledbygui = 2;
        assert(stream_cache_prefill(&s, target) == 0);
        cache_uninit(&s);
        assert(!s.cache_data && CacheThreadSuspended());
        controlledbygui = 0;
    }
    assert(dav_audio_start_bytes(0) == 120000);
    assert(dav_audio_start_bytes(-1) == 120000);
    assert(dav_audio_start_bytes(1) == DAV_AUDIO_PROBE_BYTES);
    assert(dav_audio_start_bytes(INT_MAX) == DAV_AUDIO_CACHE_BYTES / 2);
    puts("PASS 512 KiB track cache: three-second targets at 64-320 kbps, tag seeks, wrap, bounded refill and next-track stop");

    memset(&s, 0, sizeof(s)); s.end_pos = 3000;
    polls = 0;
    assert(stream_enable_cache(&s, DAV_AUDIO_CACHE_BYTES, DAV_AUDIO_PROBE_BYTES, 0) == 1);
    assert(stream_cache_prefill(&s, dav_audio_start_bytes(40000)) == 1);
    verify_read(&s, 0, 3000);
    cache_uninit(&s);
    puts("PASS track shorter than three seconds starts at EOF");

    memset(&s, 0, sizeof(s)); s.end_pos = 6 * 1024 * 1024;
    cancel_prefill = 0; cancel_on_progress = 1; polls = 0;
    read_delay = 50000;
    int finishes_before_cancel = progress_finishes;
    assert(stream_enable_cache(&s, 2 * 1024 * 1024, 1024 * 1024, 0) == 0);
    assert(progress_finishes == finishes_before_cancel + 1 && !progress_active);
    assert(!s.cache_data && !s.cache_pid && CacheThreadSuspended());
    cancel_on_progress = 0; read_delay = 1000;
    unavailable = 1;
    assert(stream_enable_cache(&s, 2 * 1024 * 1024, 1024 * 1024, 0) == -1);
    assert(!s.cache_data);
    puts("PASS cancelled prefill and unavailable worker");
    return 0;
}
