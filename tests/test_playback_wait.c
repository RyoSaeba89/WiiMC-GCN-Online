/* Exercise the actual MPlayer rebuffer loop after an MP3 probe/seek has
 * discarded prefill. Audio must refill/resume without ever entering GX. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../source/utils/playback_buffer.h"

enum { OSD_PLAY = 1, VOCTRL_PAUSE, VOCTRL_RESUME, MP_CMD_PAUSE,
       MSGT_CPLAYER, MSGL_INFO };
typedef struct { int id, pausing; const char *name; } mp_cmd_t;
static struct { void (*pause)(void); void (*resume)(void); } audio;
static struct { void (*control)(int, void *); void (*check_events)(void); } video;
static struct {
    void *sh_audio, *sh_video;
    typeof(audio) *audio_out;
    typeof(video) *video_out;
    int osd_function;
} context, *mpctx = &context;
static int vo_config_count, controlledbygui;
static float stream_cache_min_percent = 50, orig_stream_cache_min_percent = 50;
static float cache_fill_status;
static const char *fileplaying = "dav1:/song.mp3", *filename = "dav1:/song.mp3";
static const char *dvd_device;
static int pauses, resumes, draws, events, sleeps, progress_updates, status;
static int stop_on_sleep, eof_on_sleep, timer_resets;
static int allow_video;

static void audio_pause(void) { ++pauses; }
static void audio_resume(void) { ++resumes; }
static void video_control(int cmd, void *arg) { (void)cmd; (void)arg; }
static void check_events(void) { assert(allow_video); ++events; }
static void DrawMPlayer(void) { assert(allow_video); ++draws; }
static void SetBufferingStatus(int value)
{
    assert(value >= 0 && value <= 100);
    status = value;
    if (value) ++progress_updates;
}
static void usec_sleep(unsigned int delay)
{
    assert(delay >= 100);
    if (!allow_video) assert(delay >= 20000);
    assert(++sleeps < 20); /* A stalled wait fails deterministically. */
    if (stop_on_sleep) controlledbygui = 2;
    else if (eof_on_sleep) cache_fill_status = -1;
    else cache_fill_status += 10;
}
#define usleep usec_sleep
static mp_cmd_t *mp_input_get_cmd(int delay, int peek, int paused)
{ (void)delay; (void)peek; (void)paused; return NULL; }
static void run_command(void *ctx, mp_cmd_t *cmd) { (void)ctx; (void)cmd; }
static void mp_cmd_free(mp_cmd_t *cmd) { (void)cmd; }
static void StartDVDMotor(void) { assert(!"unexpected DVD access"); }
static void WakeupUSB(void) { assert(!"unexpected USB access"); }
static void GetRelativeTime(void) { ++timer_resets; }
static void mp_msg(int mod, int lev, const char *fmt, ...)
{ (void)mod; (void)lev; (void)fmt; }

#include "out/low_cache_loop.inc"

static void reset_case(void)
{
    memset(&context, 0, sizeof(context));
    audio.pause = audio_pause;
    audio.resume = audio_resume;
    video.control = video_control;
    video.check_events = check_events;
    context.sh_audio = &audio;
    context.audio_out = &audio;
    controlledbygui = 1; /* GuiThread owns GX during music playback. */
    cache_fill_status = 5;
    vo_config_count = 0;
    pauses = resumes = draws = events = sleeps = progress_updates = status = 0;
    stop_on_sleep = eof_on_sleep = timer_resets = allow_video = 0;
}
static void check_finished(void)
{
    assert(pauses == 1 && resumes == 1 && timer_resets == 1);
    assert(progress_updates > 0 && status == 0);
    assert(context.osd_function == OSD_PLAY);
}
int main(void)
{
    reset_case();
    low_cache_loop();
    check_finished();
    assert(cache_fill_status >= 50 && sleeps > 1 && !draws && !events);
    puts("PASS audio refills to 50% and resumes without GX");

    reset_case();
    stream_cache_min_percent = 100.0 * dav_audio_start_bytes(16000) / DAV_AUDIO_CACHE_BYTES;
    cache_fill_status = 0;
    low_cache_loop();
    check_finished();
    assert(cache_fill_status >= stream_cache_min_percent && !draws && !events);
    stream_cache_min_percent = 50;
    puts("PASS WebDAV resumes at three seconds of 128 kbps audio without GX");

    reset_case();
    context.video_out = &video;
    vo_config_count = 1; /* A leftover output must not imply a video stream. */
    low_cache_loop();
    check_finished();
    assert(!draws && !events);
    puts("PASS audio with a stale video output leaves GX to the GUI");

    reset_case();
    stop_on_sleep = 1;
    low_cache_loop();
    check_finished();
    assert(sleeps == 1 && controlledbygui == 2 && !draws);
    puts("PASS stop/next-track exits audio rebuffering");

    reset_case();
    eof_on_sleep = 1;
    low_cache_loop();
    check_finished();
    assert(sleeps == 1 && cache_fill_status < 0 && !draws);
    puts("PASS EOF releases audio rebuffering");

    reset_case();
    context.sh_video = &video;
    context.video_out = &video;
    low_cache_loop();
    check_finished();
    assert(!draws && !events);
    puts("PASS unconfigured video output never draws");

    reset_case();
    context.sh_video = &video;
    context.video_out = &video;
    vo_config_count = allow_video = 1;
    controlledbygui = 0;
    low_cache_loop();
    check_finished();
    assert(draws > 1 && events == draws && sleeps == draws);
    puts("PASS configured video still draws during rebuffering");
    return 0;
}
