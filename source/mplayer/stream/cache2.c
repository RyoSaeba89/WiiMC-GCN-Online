/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

// Initial draft of my new cache system...
// Note it runs in 2 processes (using fork()), but doesn't require locking!!
// TODO: seeking, data consistency checking

#define READ_USLEEP_TIME 10000
#define FILL_USLEEP_TIME 50000
#define PREFILL_SLEEP_TIME 200
#define CONTROL_SLEEP_TIME 200

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#define MAXPATHLEN 1024

#undef USE_ARAM

#include "libavutil/avutil.h"
#include "osdep/shmem.h"
#include "osdep/timer.h"
#if defined(__MINGW32__)
#include <windows.h>
static void ThreadProc( void *s );
#elif defined(__OS2__)
#define INCL_DOS
#include <os2.h>
static void ThreadProc( void *s );
#elif defined(PTHREAD_CACHE)
#include <pthread.h>
static void *ThreadProc(void *s);
#elif defined(GEKKO)
#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
static void *ThreadProc(void *s);
//static unsigned char *global_buffer=NULL;
static void *cachearg = NULL;
static mutex_t cache_mutex = LWP_MUTEX_NULL;
static mutex_t cache_pos_mutex = LWP_MUTEX_NULL;
volatile int stop_cache_thread = 1;
extern void SuspendCacheThread();
extern void ResumeCacheThread();
extern bool CacheThreadSuspended();
extern bool CacheThreadAvailable();
extern int controlledbygui;
extern void CheckMplayerNetwork();
extern void ShowProgress (const char *msg, int done, int total);
extern void FinishBufferingProgress(void);
#else
#include <sys/wait.h>
#define FORKED_CACHE 1
#endif
#ifndef FORKED_CACHE
#define FORKED_CACHE 0
#endif

#include "mp_msg.h"
#include "help_mp.h"

#include "stream.h"
#include "cache2.h"
#include "mp_global.h"

#include <malloc.h>

#ifdef USE_ARAM
static void *gekko_stack = NULL;
#define GEKKO_THREAD_STACKSIZE (512 * 1024)
#define GEKKO_THREAD_PRIO 70
#endif

typedef struct {
  // constats:
  unsigned char *buffer;      // base pointer of the allocated buffer memory
  int buffer_size; // size of the allocated buffer memory
  int sector_size; // size of a single sector (2048/2324)
  int back_size;   // we should keep back_size amount of old bytes for backward seek
  int fill_limit;  // we should fill buffer only if space>=fill_limit
  int seek_limit;  // keep filling cache if distance is less that seek limit
#if FORKED_CACHE
  pid_t ppid; // parent PID to detect killed parent
#endif
  // filler's pointers:
  volatile int eof;
  volatile off_t min_filepos; // shared with the reader on the single-core GC
  volatile off_t max_filepos;
  off_t offset;      // filepos <-> bufferpos  offset value (filepos of the buffer's first byte)
  // reader's pointers:
  volatile off_t read_filepos;
  // commands/locking:
//  int seek_lock;   // 1 if we will seek/reset buffer, 2 if we are ready for cmd
//  int fifo_flag;  // 1 if we should use FIFO to notice cache about buffer reads.
  // callback
  stream_t* stream;
  volatile int control;
  volatile unsigned control_uint_arg;
  volatile double control_double_arg;
  volatile struct stream_lang_req control_lang_arg;
  volatile int control_res;
  volatile double stream_time_length;
  volatile double stream_time_pos;
#ifdef USE_ARAM
  void *arq_read_buffer;
  void *arq_fill_buffer;
#endif
} cache_vars_t;

/* The four positions above are shared between the filler thread and the
 * reader, and off_t is **64 bits** on this toolchain while the CPU is 32 --
 * verified, not assumed. Every one of them is therefore written as two
 * separate stores, and `volatile` provides neither atomicity nor ordering:
 * the other thread can read the new half of a position beside the old half
 * and compute a length that is negative, or gigabytes long. What that looks
 * like from the couch is a false EOF, a corrupted ring, or a track change
 * that hangs -- and it cannot reproduce on the x86-64 host test, where these
 * loads are single instructions.
 *
 * So every cross-thread read takes a consistent snapshot of all four, and
 * every publish goes through a helper. A dedicated mutex, not cache_mutex:
 * the sections here are a handful of loads, they must never be held across
 * a network read, and cache_stream_seek_long() publishes a position while
 * already holding cache_mutex. Lock order is cache_mutex -> cache_pos_mutex,
 * and nothing takes them the other way round. */
typedef struct {
  off_t min_filepos;
  off_t max_filepos;
  off_t read_filepos;
  off_t offset;
} cache_pos_t;

#define CACHE_POS_LOCK()   do { if(cache_pos_mutex != LWP_MUTEX_NULL) LWP_MutexLock(cache_pos_mutex); } while(0)
#define CACHE_POS_UNLOCK() do { if(cache_pos_mutex != LWP_MUTEX_NULL) LWP_MutexUnlock(cache_pos_mutex); } while(0)

static void cache_pos_get(cache_vars_t *s, cache_pos_t *p)
{
  CACHE_POS_LOCK();
  p->min_filepos  = s->min_filepos;
  p->max_filepos  = s->max_filepos;
  p->read_filepos = s->read_filepos;
  p->offset       = s->offset;
  CACHE_POS_UNLOCK();
}

/* The reader owns read_filepos. */
static void cache_pos_set_read(cache_vars_t *s, off_t pos)
{
  CACHE_POS_LOCK();
  s->read_filepos = pos;
  CACHE_POS_UNLOCK();
}

static void cache_pos_advance_read(cache_vars_t *s, int len)
{
  CACHE_POS_LOCK();
  s->read_filepos += len;
  CACHE_POS_UNLOCK();
}

/* The filler owns min_filepos, max_filepos and offset. */
static void cache_pos_set_min(cache_vars_t *s, off_t pos)
{
  CACHE_POS_LOCK();
  s->min_filepos = pos;
  CACHE_POS_UNLOCK();
}

/* Published as one step: a reader that saw the new max_filepos must also see
 * the offset that goes with it, or its wrap arithmetic lands anywhere. */
static void cache_pos_commit_fill(cache_vars_t *s, int len, int wrapped)
{
  CACHE_POS_LOCK();
  s->max_filepos += len;
  if(wrapped) s->offset += s->buffer_size;
  CACHE_POS_UNLOCK();
}

static void cache_pos_reset(cache_vars_t *s, off_t pos)
{
  CACHE_POS_LOCK();
  s->offset = s->min_filepos = s->max_filepos = s->read_filepos = pos;
  CACHE_POS_UNLOCK();
}

volatile float cache_fill_status=0;

float MPlayerCacheFillPercent(void)
{
  return cache_fill_status;
}

/* Same tearing hazard: this is max_filepos minus read_filepos, one owned by
 * each thread. */
static void cache_update_fill_status(cache_vars_t *s)
{
  cache_pos_t p;

  if(s->eof) { cache_fill_status = -1; return; }

  cache_pos_get(s, &p);
  cache_fill_status = (p.max_filepos - p.read_filepos) * 100.0 / s->buffer_size;
}

static void cache_flush(cache_vars_t *s)
{
  cache_pos_t p;

  cache_pos_get(s, &p);
  cache_pos_reset(s, p.read_filepos); // drop cache content :(
}

extern int getMESS;
extern int getWeird;
extern int waitReload;
extern int cntReconnect;

static int cache_read(cache_vars_t *s, unsigned char *buf, int size)
{
  int total=0;
  u64 last_progress = GetTimerMS();
  
  while(size>0 ){
    int pos,newb,len;
    cache_pos_t p;

	if(getMESS != 0 || controlledbygui == 2 || stop_cache_thread)
		return total;
	
  //printf("CACHE2_READ: 0x%X <= 0x%X <= 0x%X  \n",s->min_filepos,s->read_filepos,s->max_filepos);

    /* One snapshot per iteration: every test and every offset below has to
     * agree with the others, and the filler is moving all of them. */
    cache_pos_get(s, &p);

    if(p.read_filepos>=p.max_filepos || p.read_filepos<p.min_filepos){
	/* eof is only an answer once the reader has caught up with the filler.
	 * Below min_filepos the bytes are not missing, they are merely not
	 * fetched yet: the filler has to seek back and read them again. Taking
	 * that for end-of-stream is what turned a backward seek after EOF into
	 * a short read -- a track that plays once and never again. */
	if(s->eof && p.read_filepos>=p.max_filepos) break;
	if(GetTimerMS() - last_progress >= 30000) {
	    mp_msg(MSGT_CACHE, MSGL_ERR, "cache: no data for 30 seconds\n");
	    return total;
	}

	// waiting for buffer fill...
	usec_sleep(READ_USLEEP_TIME); // 10ms
	continue; // try again...
    }	
    newb=p.max_filepos-p.read_filepos; // new bytes in the buffer

//    printf("*** newb: %d bytes ***\n",newb);

    pos=p.read_filepos - p.offset;
    if(pos<0) pos+=s->buffer_size; else
    if(pos>=s->buffer_size) pos-=s->buffer_size;

    if(newb>s->buffer_size-pos) newb=s->buffer_size-pos; // handle wrap...
    if(newb>size) newb=size;

    // check:
    //if(s->read_filepos<s->min_filepos) mp_msg(MSGT_CACHE,MSGL_ERR,"Ehh. s->read_filepos<s->min_filepos !!! Report bug...\n");

    // len=write(mem,newb)
    //printf("Buffer read: %d bytes\n",newb);
    
	if(newb<=0 || pos<0 || pos+newb > s->buffer_size) // very very odd error
    {
    	//debug_str="Ehh. very very odd error !!! Report bug...\n";
    	printf("Ehh. very very odd error !!! Report bug... pos: %i  newb: %i\n",pos,newb);
    	continue;
	}
#ifdef USE_ARAM
	ARQRequest arq_request;
    ARQ_PostRequest(&arq_request, 0x1111, AR_ARAMTOMRAM, ARQ_PRIO_HI, (u32)s->buffer+pos, (u32)s->arq_read_buffer, newb);
    DCInvalidateRange(s->arq_read_buffer,newb);
    memcpy(buf,s->arq_read_buffer,newb);
#else
    memcpy(buf,&s->buffer[pos],newb);
#endif
    buf+=newb;
    len=newb;
    // ...

    cache_pos_advance_read(s, len);
    size-=len;
    total+=len;
    last_progress = GetTimerMS();

  }
  
  //reset
  getWeird = 0;
  
  cache_update_fill_status(s);
  return total;
}

static int cache_fill(cache_vars_t *s)
{
#ifdef GEKKO
  if(!s)
  {
      cache_fill_status=-1;
	  return 0;
  }

  if(s->eof)
  {
      /* EOF latches, and once it has, this early return used to make the
       * cache dead for good: no flush, no seek back, every later read
       * answering 0.
       *
       * cache_stream_seek_long() clears s->eof when the reader seeks back,
       * but that is a race it loses. The filler can already be inside a
       * stream_read_internal() issued against the *old* position; that read
       * returns 0 at the end of the file and sets eof again, a moment after
       * the seek cleared it. From then on the reader sits below min_filepos
       * with eof set, which is precisely the "out of boundaries" case the
       * code below knows how to recover from -- and never reached it.
       *
       * On hardware this is a track that never buffers after the player has
       * once run a file to its end. So: believe eof only while the reader is
       * still inside the window it describes. */
      cache_pos_t e;

      cache_pos_get(s, &e);

      if(e.read_filepos >= e.min_filepos && e.read_filepos <= e.max_filepos)
      {
          cache_fill_status=-1;
          return 0;
      }

      s->eof = 0;
      stream_reset(s->stream);
  }
#endif

  int back,back2,newb,space,len,pos;
  cache_pos_t p;
  off_t read;
  int read_chunk;
  int wraparound_copy = 0;

  /* The filler owns min_filepos, max_filepos and offset, but read_filepos is
   * the reader's and moves under it, so the whole computation below works
   * from one snapshot. */
  cache_pos_get(s, &p);
  read = p.read_filepos;

  if(read<p.min_filepos || read>p.max_filepos){
      // seek...
      //mp_msg(MSGT_CACHE,MSGL_DBG2,"Out of boundaries... seeking to 0x%"PRIX64"  \n",(int64_t)read);
      // streaming: drop cache contents only if seeking backward or too much fwd:
      if(s->stream->type!=STREAMTYPE_STREAM ||
          read<p.min_filepos || read>=p.max_filepos+s->seek_limit)
      {
    	cache_flush(s);
        if(s->stream->eof) stream_reset(s->stream);
        stream_seek_internal(s->stream,read);
        cache_pos_get(s, &p); // the flush moved all four
        //mp_msg(MSGT_CACHE,MSGL_DBG2,"Seek done. new pos: 0x%"PRIX64"  \n",(int64_t)stream_tell(s->stream));
      }
  }

  // calc number of back-bytes:
  back=read - p.min_filepos;
  if(back<0) back=0; // strange...
  if(back>s->back_size) back=s->back_size;

  // calc number of new bytes:
  newb=p.max_filepos - read;
  if(newb<0) newb=0; // strange...

  // calc free buffer space:
  space=s->buffer_size - (newb+back);

  if(space<s->fill_limit){
//    printf("Buffer is full (%d bytes free, limit: %d)\n",space,s->fill_limit);
#ifdef GEKKO
    cache_update_fill_status(s);
#endif
    return 0; // no fill...
  }
  // calc bufferpos:
  pos=p.max_filepos - p.offset;
  if(pos>=s->buffer_size) pos-=s->buffer_size; // wrap-around


//  printf("### read=0x%X  back=%d  newb=%d  space=%d  pos=%d\n",read,back,newb,space,pos);

  // try to avoid wrap-around. If not possible due to sector size
  // do an extra copy.
  if(space>s->buffer_size-pos) {
    if (s->buffer_size-pos >= s->sector_size) {
      space=s->buffer_size-pos;
    } else {
      space = s->sector_size;
      wraparound_copy = 1;
    }
  }

  // limit one-time block size
  read_chunk = s->stream->read_chunk;
  if (!read_chunk) read_chunk = 4*s->sector_size;
  space = FFMIN(space, read_chunk);

#if 1
  // back+newb+space <= buffer_size
  back2=s->buffer_size-(space+newb); // max back size
  if(p.min_filepos<(read-back2)) cache_pos_set_min(s, read-back2);
#else
  s->min_filepos=read-back; // avoid seeking-back to temp area...
#endif

#ifdef USE_ARAM
	len = stream_read_internal(s->stream, s->arq_fill_buffer, space);
#else
  if (wraparound_copy) {
      int to_copy;
      len = stream_read_internal(s->stream, s->stream->buffer, space);
      to_copy = FFMIN(len, s->buffer_size-pos);
      memcpy(s->buffer + pos, s->stream->buffer, to_copy);
      memcpy(s->buffer, s->stream->buffer + to_copy, len - to_copy);
    } else
  len = stream_read_internal(s->stream, &s->buffer[pos], space);
#endif
#ifdef GEKKO
  if(len==0) 
  {
  	if(s->stream->error>0)
	{
		s->stream->error++; //count read error
		
		if(s->stream->error>500) //num retries
		{
			printf("error > 500. eof = 1\n");
			s->eof=1;
		}
		else		
		{
		  //retry if we have cache
		  cache_update_fill_status(s);
		  if(cache_fill_status<5)
		  {	  
	  		s->eof=1;
	  		cache_fill_status=-1;  		
	  		printf("error: %i cache_fill_status<5  eof=1\n",s->stream->error);
	  	  }
	  	  else 
		  {
			extern char fileplaying[MAXPATHLEN];
			//s->eof=0;
				printf("retry read (%f): %i -> %s \n",cache_fill_status,s->stream->error,fileplaying);
				
				if(s->stream->error>3 && strncmp(fileplaying,"smb",3)==0)//only reset network in samba, maybe we can check internet streams later, samba can reconnect
					CheckMplayerNetwork();
		  }
  	    }
	}
  	else
  	{
  		cache_fill_status=-1;  	
  		printf("error. eof = 1\n");
  		s->eof=1;
  	}
  }
#ifdef USE_ARAM
else
  {
      ARQRequest arq_request;
      DCFlushRange(s->arq_fill_buffer,space);
      ARQ_PostRequest(&arq_request, 0x1111, AR_MRAMTOARAM, ARQ_PRIO_LO, (u32)s->buffer+pos, (u32)s->arq_fill_buffer, space);
  }
#else
  else s->stream->error=0;
#endif
#else
  s->eof= !len;
#endif
  /* One publish: a reader that sees the new max_filepos must see the offset
   * that belongs with it, or its wrap arithmetic points into the wrong half
   * of the ring. */
  cache_pos_commit_fill(s, len, pos+len>=s->buffer_size);
#ifdef GEKKO
    cache_update_fill_status(s);
#endif
  return len;

}

static int cache_execute_control(cache_vars_t *s) {
  double double_res;
  unsigned uint_res;
  int needs_flush = 0;
  static u64 last;
  u64 now;
#ifdef GEKKO
  if(!s || !s->stream)
	  return 0;
#endif
  int quit = s->control == -2;
  if (quit || !s->stream->control) {
    s->stream_time_length = 0;
    s->stream_time_pos = MP_NOPTS_VALUE;
    s->control_res = STREAM_UNSUPPORTED;
    s->control = -1;
    return !quit;
  }
  
  now = GetTimerMS();
  if (now - last > 99) {
    double len, pos;
    if (s->stream->control(s->stream, STREAM_CTRL_GET_TIME_LENGTH, &len) == STREAM_OK)
      s->stream_time_length = len;
    else
      s->stream_time_length = 0;
    if (s->stream->control(s->stream, STREAM_CTRL_GET_CURRENT_TIME, &pos) == STREAM_OK)
      s->stream_time_pos = pos;
    else
      s->stream_time_pos = MP_NOPTS_VALUE;
#if FORKED_CACHE
    // if parent PID changed, main process was killed -> exit
    if (s->ppid != getppid()) {
      mp_msg(MSGT_CACHE, MSGL_WARN, "Parent process disappeared, exiting cache process.\n");
      return 0;
    }
#endif
    last = now;
  }
  if (s->control == -1) return 1;
  switch (s->control) {
    case STREAM_CTRL_SEEK_TO_TIME:
      needs_flush = 1;
      double_res = s->control_double_arg;
    case STREAM_CTRL_GET_CURRENT_TIME:
    case STREAM_CTRL_GET_ASPECT_RATIO:
      s->control_res = s->stream->control(s->stream, s->control, &double_res);
      s->control_double_arg = double_res;
      break;
    case STREAM_CTRL_SEEK_TO_CHAPTER:
    case STREAM_CTRL_SET_ANGLE:
      needs_flush = 1;
      uint_res = s->control_uint_arg;
    case STREAM_CTRL_GET_NUM_CHAPTERS:
    case STREAM_CTRL_GET_CURRENT_CHAPTER:
    case STREAM_CTRL_GET_NUM_ANGLES:
    case STREAM_CTRL_GET_ANGLE:
      s->control_res = s->stream->control(s->stream, s->control, &uint_res);
      s->control_uint_arg = uint_res;
      break;
    case STREAM_CTRL_GET_LANG:
      s->control_res = s->stream->control(s->stream, s->control, (void *)&s->control_lang_arg);
      break;
    default:
      s->control_res = STREAM_UNSUPPORTED;
      break;
  }
  if (s->control_res == STREAM_OK && needs_flush) {
    cache_pos_set_read(s, s->stream->pos);
    s->eof = s->stream->eof;
    cache_flush(s);
  }
  s->control = -1;
  return 1;
}

static cache_vars_t* cache_init(int size,int sector){
  int num;
#if !defined(__MINGW32__) && !defined(PTHREAD_CACHE) && !defined(__OS2__) && !defined(GEKKO)
  cache_vars_t* s=shmem_alloc(sizeof(cache_vars_t));
#else
  cache_vars_t* s=malloc(sizeof(cache_vars_t));
#endif
  if(s==NULL) return NULL;

  memset(s,0,sizeof(cache_vars_t));
  num=size/sector;
  if(num < 16){
     num = 16;
  }//32kb min_size
  s->buffer_size=num*sector;
  s->sector_size=sector;
  s->control=-1;
  printf("s->buffer_size: %i  sector: %i\n",s->buffer_size,sector);
#if !defined(__MINGW32__) && !defined(PTHREAD_CACHE) && !defined(__OS2__) && !defined(GEKKO)
  s->buffer=shmem_alloc(s->buffer_size);
#else
#ifdef USE_ARAM
	s->buffer=(void*)(AR_GetBaseAddress()+(AR_GetInternalSize()-s->buffer_size-0x4000));
  //s->buffer=shared_alloc(s->buffer_size);

  if((u32)s->buffer < AR_GetBaseAddress()){
#else
  /* The fixed/global GameCube cache buffer used by the original port was
   * removed, but its replacement allocation was left commented out.  The
   * result was that every cache_init() failed and playback continued without
   * a cache.  Use an aligned heap block so the requested size is real. */
  s->buffer=memalign(32, s->buffer_size);
  if(s->buffer == NULL){
#endif
#endif

 // if(s->buffer == NULL){
#if !defined(__MINGW32__) && !defined(PTHREAD_CACHE) && !defined(__OS2__) && !defined(GEKKO)
    shmem_free(s,sizeof(cache_vars_t));
#else
    free(s);
#endif
    return NULL;
  }
  s->fill_limit=8*sector;
#if defined(GEKKO)
  s->back_size=s->buffer_size/4; // 1/4 back  3/4 forward
#else
  s->back_size=s->buffer_size/2;
#endif
#if FORKED_CACHE
  s->ppid = getpid();
#endif
  return s;
}

void cache_uninit(stream_t *s) {
  cache_vars_t* c = s->cache_data;
  
#if defined(GEKKO)
  if(s->cache_pid && CacheThreadAvailable())
  {
    /* Cancel network I/O before waiting. Never free the shared stream while
     * the worker is still running, even if the GUI requested another track. */
    stop_cache_thread = 1;
    while(!CacheThreadSuspended()) usleep(1000);
  }
  s->cache_pid = 0;
  cachearg = NULL;
  cache_fill_status = -1;
#ifdef USE_ARAM
  free(gekko_stack);
  gekko_stack = NULL;
#endif
#else  
  if(s->cache_pid) {
#if defined(__MINGW32__) || defined(PTHREAD_CACHE) || defined(__OS2__) || defined(GEKKO)
    cache_do_control(s, -2, NULL);
#else
    kill(s->cache_pid,SIGKILL);
    waitpid(s->cache_pid,NULL,0);
#endif
	s->cache_pid = 0;
  }
#endif //GEKKO

if(!c) return;
#if defined(GEKKO)
  if(c->stream)
    free(c->stream);

#ifdef USE_ARAM
  free(c->arq_read_buffer);
  c->arq_read_buffer = NULL;
  free(c->arq_fill_buffer);
  c->arq_fill_buffer = NULL;
#else
  free(c->buffer);
#endif

  c->buffer=NULL;
  free(s->cache_data);
  s->cache_data=NULL;
  s->cache_pid=0;

#else
#if defined(__MINGW32__) || defined(PTHREAD_CACHE) || defined(__OS2__)
  free(c->stream);
  free(c->buffer);
  c->buffer = NULL;
  free(s->cache_data);
#else
  shmem_free(c->buffer,c->buffer_size);
  c->buffer = NULL;
  shmem_free(s->cache_data,sizeof(cache_vars_t));
#endif
  s->cache_data = NULL;
#endif //GEKKO
}


static void exit_sighandler(int x){
  // close stream
  exit(0);
}

int stream_cache_prefill(stream_t *stream, int min)
{
    cache_vars_t *s = stream->cache_data;
    int result = 1;
    if (!s) return -1;
#ifdef GEKKO
    int showed_progress = 0;
#endif
    /* A second prefill can follow probing/seeking. Leave room for retained
     * history and the producer's minimum write size, or it cannot finish. */
    int forward_limit = s->buffer_size - s->back_size - s->fill_limit;
    if (min > forward_limit) min = forward_limit;

    u64 last_progress = GetTimerMS();
    cache_pos_t p;
    cache_pos_get(s, &p);
    off_t last_position = p.max_filepos;
    /* Re-snapshot every turn: the filler is publishing into these while the
     * loop reads them, and a torn max_filepos here shows up as a prefill that
     * finishes early or never. */
    for (cache_pos_get(s, &p);
         p.read_filepos < p.min_filepos || p.max_filepos - p.read_filepos < min;
         cache_pos_get(s, &p)) {
        if (getMESS || controlledbygui == 2 || stream_check_interrupt(0)) {
            result = 0;
            goto done;
        }
        if (s->eof) break; // short file: play the remaining bytes
#ifdef GEKKO
        if (cntReconnect == 0) {
            off_t available = p.max_filepos - p.read_filepos;
            ShowProgress("Buffering...", available > 0 ? (int)available : 0, min);
            showed_progress = 1;
        }
#endif
        if (p.max_filepos != last_position) {
            last_position = p.max_filepos;
            last_progress = GetTimerMS();
        } else if (GetTimerMS() - last_progress >= 30000) {
            mp_msg(MSGT_CACHE, MSGL_ERR, "cache: prefill stalled for 30 seconds\n");
            result = -1;
            goto done;
        }
        /* Yield even when input callbacks return immediately. */
        usec_sleep(20000);
    }
    if (getMESS || controlledbygui == 2) {
        result = 0;
        goto done;
    }
    cache_pos_get(s, &p);
    if (s->eof && p.max_filepos == p.read_filepos && s->stream->error)
        result = -1;
done:
#ifdef GEKKO
    if (showed_progress) FinishBufferingProgress();
#endif
    return result;
}

/**
 * \return 1 on success, 0 if the function was interrupted and -1 on error
 */
int stream_enable_cache(stream_t *stream,int size,int min,int seek_limit){
  int ss = stream->sector_size ? stream->sector_size : STREAM_BUFFER_SIZE;
  int res = -1;
  cache_vars_t* s;
#ifdef GEKKO
  cache_fill_status=-1;
  if(!CacheThreadAvailable()) return -1;
#endif
  if (stream->flags & STREAM_NON_CACHEABLE) {
    //mp_msg(MSGT_CACHE,MSGL_STATUS,"\rThis stream is non-cacheable\n");
    return 1;
  }
#ifdef GEKKO
  if(cache_mutex == LWP_MUTEX_NULL)
    LWP_MutexInit(&cache_mutex, false);	
  if(cache_pos_mutex == LWP_MUTEX_NULL)
    LWP_MutexInit(&cache_pos_mutex, false);
#endif
  s=cache_init(size,ss);
  if(s == NULL) return -1;
  stream->cache_data=s;
  s->stream=NULL; // owns a shallow copy, never the caller's stream
  s->seek_limit=seek_limit;
  stream->error=0;
  cache_pos_reset(s, stream->pos);

  //make sure that we won't wait from cache_fill
  //more data than it is allowed to fill
  if (s->seek_limit > s->buffer_size - s->fill_limit ){
     s->seek_limit = s->buffer_size - s->fill_limit;
  }
  if (min > s->buffer_size - s->fill_limit) {
     min = s->buffer_size - s->fill_limit;
  }

#if FORKED_CACHE
  s->stream=stream;
  if((stream->cache_pid=fork())){
    if ((pid_t)stream->cache_pid == -1)
      stream->cache_pid = 0;
#else
  {
    stream_t* stream2=malloc(sizeof(stream_t));
    if(!stream2) goto err_out;
    memcpy(stream2,stream,sizeof(stream_t));
    s->stream=stream2;    
#if defined(__MINGW32__)
    stream->cache_pid = _beginthread( ThreadProc, 0, s );
#elif defined(__OS2__)
    stream->cache_pid = _beginthread( ThreadProc, NULL, 256 * 1024, s );
#elif defined(GEKKO)
	stop_cache_thread = 1;
	while(!CacheThreadSuspended()) {
		usleep(50);

		//if(getMESS != 0) { //this doesn't hit, but let's assume it's useful for edgecases
		//	return -1;
		//}
	}
	cachearg = s;
	stop_cache_thread = 0;
	stream->cache_pid = 1;
	ResumeCacheThread();
	
#ifdef USE_ARAM
	s->arq_read_buffer = (void*)memalign(32, ss > STREAM_MAX_SECTOR_SIZE ? STREAM_MAX_SECTOR_SIZE : ss);
	s->arq_fill_buffer = (void*)memalign(32, stream->read_chunk ? stream->read_chunk : 4 * ss);
	gekko_stack = (void*)memalign(32, GEKKO_THREAD_STACKSIZE);
	
	memset(gekko_stack, 0x00, GEKKO_THREAD_STACKSIZE);
	LWP_CreateThread(&stream->cache_pid, ThreadProc, s, gekko_stack,
					GEKKO_THREAD_STACKSIZE, GEKKO_THREAD_PRIO);
#endif
	
#else
    {
    pthread_t tid;
    pthread_create(&tid, NULL, ThreadProc, s);
    stream->cache_pid = 1;
    }
#endif
#endif
    if (!stream->cache_pid) {
        mp_msg(MSGT_CACHE, MSGL_ERR,
               "Starting cache process/thread failed: %s.\n", strerror(errno));
        goto err_out;
    }
    // wait until cache is filled at least prefill_init %
    cache_pos_t init_pos;
    cache_pos_get(s, &init_pos);
    mp_msg(MSGT_CACHE,MSGL_V,"CACHE_PRE_INIT: %"PRId64" [%"PRId64"] %"PRId64"  pre:%d  eof:%d  \n",
	(int64_t)init_pos.min_filepos,(int64_t)init_pos.read_filepos,(int64_t)init_pos.max_filepos,min,s->eof);

    res = stream_cache_prefill(stream, min);
    if (res <= 0) goto err_out;

    mp_msg(MSGT_CACHE,MSGL_STATUS,"\n");
    return 1; // parent exits

err_out:
    cache_uninit(stream);
    return res;
  }

#if defined(__MINGW32__) || defined(PTHREAD_CACHE) || defined(__OS2__) || defined(GEKKO)
}
#if defined(PTHREAD_CACHE) || defined(GEKKO)
static void *ThreadProc( void *s ){
#else
static void ThreadProc( void *s ){
#endif
#endif

#ifdef CONFIG_GUI
  use_gui = 0; // mp_msg may not use gui stuff in forked code
#endif
// cache thread mainloop:
#ifndef GEKKO
  signal(SIGTERM,exit_sighandler); // kill
#endif  
  do {
    if(!cache_fill(s)){
	 usec_sleep(FILL_USLEEP_TIME); // idle
    }else usec_sleep(500);
#ifndef GEKKO
  } while (cache_execute_control(s));
#else
  //} while (cache_execute_control(s) && !stop_cache_thread && getMESS == 0); //not this, but let's assume due to edgecases
  } while (cache_execute_control(s) && !stop_cache_thread); //not this
#endif  
#if defined(__MINGW32__) || defined(__OS2__)
  _endthread();
#endif
#if defined(PTHREAD_CACHE) || defined(GEKKO) 
  return NULL;
#endif
  // make sure forked code never leaves this function
  exit(0);
}

#ifdef GEKKO
void *mplayercachethread(void *arg)
{
	while(1)
	{
		SuspendCacheThread();

		if(cachearg != NULL)
			ThreadProc(cachearg);
	}
	return NULL;
}
#endif

int cache_stream_fill_buffer(stream_t *s){
  int len;
  int sector_size;
  if(!s->cache_pid) return stream_fill_buffer(s);

  {
    cache_pos_t p;
    cache_pos_get((cache_vars_t*)s->cache_data, &p);
    if(s->pos!=p.read_filepos) mp_msg(MSGT_CACHE,MSGL_ERR,"!!! read_filepos differs!!! report this bug...\n");
  }
  sector_size = ((cache_vars_t*)s->cache_data)->sector_size;
  if (sector_size > STREAM_MAX_SECTOR_SIZE) {
    mp_msg(MSGT_CACHE, MSGL_ERR, "Sector size %i larger than maximum %i\n", sector_size, STREAM_MAX_SECTOR_SIZE);
    sector_size = STREAM_MAX_SECTOR_SIZE;
  }

  len=cache_read(s->cache_data,s->buffer, sector_size);
  //printf("cache_stream_fill_buffer->read -> %d\n",len);

  if(len<=0){ s->eof=1; s->buf_pos=s->buf_len=0; return 0; }
  s->eof=0;
  s->buf_pos=0;
  s->buf_len=len;
  s->pos+=len;
//  printf("[%d]",len);fflush(stdout);
  return len;

}

int cache_stream_seek_long(stream_t *stream,off_t pos){
  cache_vars_t* s;
  off_t newpos;
  if(!stream->cache_pid) return stream_seek_long(stream,pos);
  LWP_MutexLock(cache_mutex);

  s=stream->cache_data;
//  s->seek_lock=1;

//  mp_msg(MSGT_CACHE,MSGL_DBG2,"CACHE2_SEEK: 0x%"PRIX64" <= 0x%"PRIX64" (0x%"PRIX64") <= 0x%"PRIX64"  \n",s->min_filepos,pos,s->read_filepos,s->max_filepos);

  newpos=pos/s->sector_size; newpos*=s->sector_size; // align
  cache_pos_set_read(s, newpos);
  stream->pos=newpos;
  s->eof=0; // !!!!!!!

	LWP_MutexUnlock(cache_mutex);
  cache_stream_fill_buffer(stream);
	LWP_MutexLock(cache_mutex);

  pos-=newpos;
  if(pos>=0 && pos<=stream->buf_len){
    stream->buf_pos=pos; // byte position in sector
    LWP_MutexUnlock(cache_mutex);
    return 1;
  }
	LWP_MutexUnlock(cache_mutex);
//  stream->buf_pos=stream->buf_len=0;
//  return 1;

//  mp_msg(MSGT_CACHE,MSGL_V,"cache_stream_seek: WARNING! Can't seek to 0x%"PRIX64" !\n",pos+newpos);
  return 0;
}

int cache_do_control(stream_t *stream, int cmd, void *arg) {
  int pos_change = 0;
  cache_vars_t* s = stream->cache_data;
  switch (cmd) {
    case STREAM_CTRL_SEEK_TO_TIME:
      s->control_double_arg = *(double *)arg;
      s->control = cmd;
      pos_change = 1;
      break;
    case STREAM_CTRL_SEEK_TO_CHAPTER:
    case STREAM_CTRL_SET_ANGLE:
      s->control_uint_arg = *(unsigned *)arg;
      s->control = cmd;
      pos_change = 1;
      break;
    // the core might call these every frame, so cache them...
    case STREAM_CTRL_GET_TIME_LENGTH:
      *(double *)arg = s->stream_time_length;
      return s->stream_time_length ? STREAM_OK : STREAM_UNSUPPORTED;
    case STREAM_CTRL_GET_CURRENT_TIME:
      *(double *)arg = s->stream_time_pos;
      return s->stream_time_pos != MP_NOPTS_VALUE ? STREAM_OK : STREAM_UNSUPPORTED;
    case STREAM_CTRL_GET_LANG:
      s->control_lang_arg = *(struct stream_lang_req *)arg;
    case STREAM_CTRL_GET_NUM_CHAPTERS:
    case STREAM_CTRL_GET_CURRENT_CHAPTER:
    case STREAM_CTRL_GET_ASPECT_RATIO:
    case STREAM_CTRL_GET_NUM_ANGLES:
    case STREAM_CTRL_GET_ANGLE:
    case -2:
      s->control = cmd;
      break;
    default:
      return STREAM_UNSUPPORTED;
  }
  while (s->control != -1) {
	usec_sleep(CONTROL_SLEEP_TIME);
	if(getMESS != 0) //significant loop
		return 0;
  }

  if (s->control_res != STREAM_OK)
    return s->control_res;
  // We cannot do this on failure, since this would cause the
  // stream position to jump when e.g. STREAM_CTRL_SEEK_TO_TIME
  // is unsupported - but in that case we need the old value
  // to do the fallback seek.
  // This unfortunately can lead to slightly different behaviour
  // with and without cache if the protocol changes pos even
  // when an error happened.
  if (pos_change) {
    {
      cache_pos_t p;
      cache_pos_get(s, &p);
      stream->pos = p.read_filepos;
    }
    stream->eof = s->eof;
  }
  switch (cmd) {
    case STREAM_CTRL_GET_TIME_LENGTH:
    case STREAM_CTRL_GET_CURRENT_TIME:
    case STREAM_CTRL_GET_ASPECT_RATIO:
      *(double *)arg = s->control_double_arg;
      break;
    case STREAM_CTRL_GET_NUM_CHAPTERS:
    case STREAM_CTRL_GET_CURRENT_CHAPTER:
    case STREAM_CTRL_GET_NUM_ANGLES:
    case STREAM_CTRL_GET_ANGLE:
      *(unsigned *)arg = s->control_uint_arg;
      break;
    case STREAM_CTRL_GET_LANG:
      *(struct stream_lang_req *)arg = s->control_lang_arg;
      break;
  }
  return s->control_res;
}

//extern int getINFO;

int stream_read(stream_t *s,char* mem,int total)
{
  int len=total;
  if(!mem) return 0;
  while(len>0)
  {
    int x;
	
	//One bread ahead of the bread
/*	if(getINFO == 2) {
		getINFO = 0;
		return 0;
	}*/
	//if(getMESS != 0) //don't care, let's assume there's an edgecase that still fails rarely...
		//return 0;

    if(s->buf_len-s->buf_pos==0)
    {
      if(!cache_stream_fill_buffer(s)) 
	  {
	  	return total-len; // EOF or error
	  }
      x=s->buf_len-s->buf_pos;
    } 
    LWP_MutexLock(cache_mutex);
    x=s->buf_len-s->buf_pos;
    if(x>len) x=len;

    memcpy(mem,&s->buffer[s->buf_pos],x);
    s->buf_pos+=x; mem+=x; len-=x;
    LWP_MutexUnlock(cache_mutex);
  }
  return total;
}

int stream_error(stream_t *stream)
{
	if(!stream || !stream->cache_data)
		return 0;

	cache_vars_t *vars = (cache_vars_t *)stream->cache_data;
	return vars->stream->error;
}

#if 0
void refillcache(stream_t *stream,float min)
{
	cache_vars_t* s;
	int out=0;
	s=stream->cache_data;
	u64 t1;
	float old=0;
	t1 = GetTimerMS();

    while(cache_fill_status<min)
    {
		//printf("Cache fill: %5.2f%%  \n",(float)(100.0*(float)(cache_fill_status)/(float)(min)));
    	ShowProgress("Buffering...", (int)cache_fill_status, (int)min);
		if(s->eof) break; // file is smaller than prefill size
			
		if(out==0)out=stream_check_interrupt(PREFILL_SLEEP_TIME);
		else
		{ //remove others pause commands if you press pause several times
		  mp_cmd_t* cmd;
		  if((cmd = mp_input_get_cmd(PREFILL_SLEEP_TIME,0,1)) != NULL)
		  {
			  if(cmd->id==MP_CMD_PAUSE)
			  {
				  cmd = mp_input_get_cmd(0,0,0);
				  mp_cmd_free(cmd);
			  }
		  }

		}
		//printf("Cache fill: %5.2f%%  \n",cache_fill_status);
		if(cache_fill_status > 5 && out)
		{
			//printf("break Cache fill: %5.2f%%  \n",cache_fill_status);
			return ;
		}	
		
		//not needed, for security	
		if(old<cache_fill_status)t1 = GetTimerMS();
	    if(GetTimerMS()-t1>1500) return;
		old=cache_fill_status;
		usleep(50);
    }
    //printf("end Cache fill: %5.2f%%  \n",cache_fill_status);   
}
#endif
