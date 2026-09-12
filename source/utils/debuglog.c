/****************************************************************************
 * WiiMC-GCN-Online
 *
 * debuglog.c
 *
 * The log file on the SD card, and the crash instruments behind it.
 *
 * On real hardware this program has no debug channel at all. Upstream's
 * SaveLogToSD() is under #if 0, stdout goes nowhere, and a GameCube booted
 * from Swiss has no USB Gecko attached. Worse, most of the ways this program
 * can die leave nothing on the screen:
 *
 *   - exit(), or main() returning (InitFreeType failing does exactly that),
 *     goes straight back to the loader;
 *   - a hang or a deadlock freezes the last frame;
 *   - a stack overflow corrupts whatever lies below the stack and fails later,
 *     somewhere unrelated, or not visibly at all;
 *   - malloc returning NULL on a 24 MB machine is followed by a store through
 *     NULL, and by then the cause is gone.
 *
 * So everything below writes to sd1:/wiimc.log, and each silent path gets its
 * own instrument:
 *
 *   exceptions     c_default_exceptionhandler wrapped at link time
 *   exit/abort     exit, abort and __assert_func wrapped at link time
 *   hangs          a heartbeat thread at priority 100, every two seconds,
 *                  naming the last breadcrumb and every thread's state
 *   stacks         every LWP_CreateThread wrapped: the stack is painted and
 *                  its high-water mark reported
 *   memory         malloc/calloc/realloc/memalign wrapped: a NULL return is
 *                  recorded with its size and its caller
 *   MPlayer        stdout and stderr redirected into the log
 *
 * The design is DKR-GC's platform/gc/gc_logfile.c and gc_crash.c, whose
 * PORTING.md paid for the lessons in hardware runs. The ones that shape this
 * file, briefly:
 *
 *   - The file is created at full size once and then overwritten in place.
 *     Appending makes libfat rewrite the FAT on every flush, and a machine that
 *     keeps dying mid-write corrupted a card that way.
 *   - The last 24 KB are reserved for the crash report, so a full log can
 *     never crowd it out. The body wraps below it.
 *   - The crash report is formatted without printf. An exception leaves MSR[FP]
 *     clear, and newlib's vfprintf touches the FPU whatever the format string:
 *     the report would take a second exception and overwrite the first.
 *   - The report goes into RAM with interrupts off, and only the card write
 *     runs with MSR[EE] on, because libfat cannot work without interrupts.
 *   - No lock is taken once a crash has started: the faulting thread may hold
 *     it.
 *
 * Reading the file is documented in PORTING.md section 11.
 ***************************************************************************/

#ifdef WANT_DEBUGLOG

#include <gccore.h>
#include <ogc/context.h>
#include <ogc/lwp_threads.h>
#include <ogc/machine/processor.h>
#include <ogc/timesupp.h>
#include <sys/iosupport.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "debuglog.h"

/* Set by Makefile.gc, and also the name of the ELF it archives in deployed/:
 * the one to resolve this log's addresses against. */
#ifndef WIIMC_BUILD_ID
#define WIIMC_BUILD_ID		"unknown"
#endif

#define LOG_PATH			"sd1:/wiimc.log"
#define LOG_PREV_PATH		"sd1:/wiimc-prev.log"
#define CRASH_PATH			"sd1:/wiimc-crash.txt"
#define CRASH_PREV_PATH		"sd1:/wiimc-crash-prev.txt"

/* The RAM side. Everything is written here first, from any thread, and the
 * card only ever sees whole flushes of it. 32 KB covers the whole boot before
 * sd1: is mounted and several heartbeats of MPlayer chatter. */
#define LOG_BUF_SIZE		(32 * 1024)

/* The card side: created once at this size, never grown. See the header. */
#define LOG_FILE_SIZE		(256 * 1024)
#define LOG_CRASH_RESERVE	(24 * 1024)
#define LOG_BODY_SIZE		(LOG_FILE_SIZE - LOG_CRASH_RESERVE)

/* What survives of the unflushed buffer when a crash starts, so that the
 * report itself always fits in the reserve behind it. */
#define LOG_CRASH_KEEP		(6 * 1024)

#define HEARTBEAT_MS		2000
#define HEARTBEAT_PRIO		100
#define HEARTBEAT_STACK		(16 * 1024)
#define THREAD_DUMP_EVERY	5		/* beats, so every ten seconds */

#define MAX_THREADS			24
#define STACK_PAINT			0x57AC57ACu
#define OOM_SITES			8
#define CRASH_STACK_WORDS	192

static char sBuf[LOG_BUF_SIZE];
static u32 sLen;
static u32 sDropped;
static char sOut[LOG_BUF_SIZE];

static mutex_t sBufLock = LWP_MUTEX_NULL;
static mutex_t sIoLock = LWP_MUTEX_NULL;
static mutex_t sThreadLock = LWP_MUTEX_NULL;

static bool sInit;
static bool sCardReady;
static bool sAppendMode;
static u32 sOffset;
static u32 sHeaderEnd;
static u32 sCrashOffset;
static int sCrashFd = -1;

/* Set by the exception handler and never cleared. From then on there is one
 * writer left, and every lock is skipped. */
static volatile bool sCrashMode;

static u64 sBootTicks;
static char sLastMark[160];

/* The boot console: libogc's text console on a framebuffer of our own, for
 * the part of the boot that runs before sd1: is mounted and before WiiMC has
 * a framebuffer. See DebugLogScreenShow. */
static u32 *sScreenXfb;
static const devoptab_t *sScreenTab;
static volatile bool sScreen;

static void screen_write(const char *s, u32 n)
{
	if (sScreen && sScreenTab != NULL && sScreenTab->write_r != NULL)
		sScreenTab->write_r(_REENT, NULL, s, n);
}

/* The first address past the code, for telling return addresses from data on
 * a stack. The libogc linker script defines no _etext; .fini is placed right
 * after .text, so __fini is the last function in the image. */
extern void __fini(void);
#define CODE_START			0x80003100u
#define CODE_END			((u32) __fini + 0x40)

static u32 uptime_ms(void)
{
	return diff_msec(sBootTicks, gettime());
}

/* ------------------------------------------------------------------------ *
 * The buffer
 * ------------------------------------------------------------------------ */

static void buf_append(const char *s, u32 n)
{
	if (!sInit || n == 0)
		return;
	if (!sCrashMode && LWP_MutexLock(sBufLock) != 0)
		return;

	/* Full: count the loss rather than store a truncated line that would be
	 * read at face value. */
	if (sLen + n > LOG_BUF_SIZE)
		sDropped += n;
	else
	{
		memcpy(sBuf + sLen, s, n);
		sLen += n;
	}

	if (!sCrashMode)
		LWP_MutexUnlock(sBufLock);
}

/* ------------------------------------------------------------------------ *
 * The card
 * ------------------------------------------------------------------------ */

static void write_card(const char *data, u32 len, u32 dropped, bool crash)
{
	static const char kDropped[] = "\n[log buffer overran; lines lost]\n";
	static const char kEnd[] = "\n<<<<<<<< end of log >>>>>>>>\n";
	static const char kWrapped[] = "\n[log wrapped; continued after the boot header]\n";
	FILE *f;

	f = fopen(LOG_PATH, sAppendMode ? "ab" : "r+b");
	if (f == NULL && !sAppendMode)
	{
		/* The preallocated path cannot be tested off hardware, so it is never
		 * allowed to be the reason there is no log. */
		sAppendMode = true;
		f = fopen(LOG_PATH, "ab");
	}
	if (f == NULL)
	{
		/* Card pulled or filesystem gone. A failing fopen every two seconds
		 * would stall the player; the crash path still gets its attempt. */
		if (!crash)
			sCardReady = false;
		return;
	}

	if (!sAppendMode)
	{
		if (crash)
		{
			if (sCrashOffset == 0)
				sCrashOffset = LOG_BODY_SIZE;
			if (sCrashOffset + len > LOG_FILE_SIZE)
				len = LOG_FILE_SIZE - sCrashOffset;
			fseek(f, (long) sCrashOffset, SEEK_SET);
			if (len != 0)
			{
				fwrite(data, 1, len, f);
				sCrashOffset += len;
			}
			fclose(f);
			return;
		}

		if (sOffset + len + sizeof(kDropped) + sizeof(kEnd) > LOG_BODY_SIZE)
		{
			fseek(f, (long) sOffset, SEEK_SET);
			fwrite(kWrapped, 1, sizeof(kWrapped) - 1, f);
			sOffset = sHeaderEnd;
		}
		fseek(f, (long) sOffset, SEEK_SET);
	}

	if (len != 0)
	{
		fwrite(data, 1, len, f);
		sOffset += len;
	}
	if (dropped != 0)
	{
		fwrite(kDropped, 1, sizeof(kDropped) - 1, f);
		sOffset += sizeof(kDropped) - 1;
	}

	/* Written after the data and not counted: the next flush overwrites it.
	 * Once the body has wrapped, it is the only way to find the newest line. */
	if (!sAppendMode)
		fwrite(kEnd, 1, sizeof(kEnd) - 1, f);

	fclose(f); /* commits the directory entry, which fflush does not */
}

void DebugLogFlush(void)
{
	u32 len, dropped;

	if (!sCardReady || sCrashMode)
		return;

	/* Two locks so that a thread logging never waits on the card: the buffer
	 * is swapped out under the short one, and written under the long one. */
	LWP_MutexLock(sIoLock);
	LWP_MutexLock(sBufLock);
	len = sLen;
	dropped = sDropped;
	memcpy(sOut, sBuf, len);
	sLen = 0;
	sDropped = 0;
	LWP_MutexUnlock(sBufLock);

	if (len != 0 || dropped != 0)
		write_card(sOut, len, dropped, false);

	LWP_MutexUnlock(sIoLock);
}

/* ------------------------------------------------------------------------ *
 * Formatted output, for normal thread context
 * ------------------------------------------------------------------------ */

static void log_vprintf(const char *fmt, va_list ap)
{
	char line[512];
	int n = vsnprintf(line, sizeof(line), fmt, ap);

	if (n <= 0)
		return;
	if ((u32) n >= sizeof(line))
		n = sizeof(line) - 1;
	buf_append(line, (u32) n);
}

void DebugLog(const char *fmt, ...)
{
	va_list ap;

	if (!sInit || sCrashMode)
		return;
	va_start(ap, fmt);
	log_vprintf(fmt, ap);
	va_end(ap);
}

void DebugMark(const char *fmt, ...)
{
	char line[512];
	va_list ap;
	u32 ms;
	int head, n;

	if (!sInit || sCrashMode)
		return;

	ms = uptime_ms();
	head = snprintf(line, sizeof(line), "[%5u.%03u] ", (unsigned) (ms / 1000),
		(unsigned) (ms % 1000));

	va_start(ap, fmt);
	n = vsnprintf(line + head, sizeof(line) - head - 1, fmt, ap);
	va_end(ap);

	if (n < 0)
		return;
	n += head;
	if ((u32) n > sizeof(line) - 2)
		n = sizeof(line) - 2;
	if (line[n - 1] != '\n')
		line[n++] = '\n';
	line[n] = 0;

	/* Remembered for the heartbeat and for the crash report, both of which
	 * read it without a lock: always NUL-terminated, whatever the race. */
	LWP_MutexLock(sBufLock);
	strncpy(sLastMark, line, sizeof(sLastMark) - 1);
	sLastMark[sizeof(sLastMark) - 1] = 0;
	LWP_MutexUnlock(sBufLock);

	buf_append(line, (u32) n);
	screen_write(line, (u32) n);
	DebugLogFlush();
}

/* ------------------------------------------------------------------------ *
 * stdout and stderr
 *
 * MPlayer reports every failure through mp_msg, which fprintf's to one or the
 * other; "Cannot open file", "No stream found", "Failed to allocate" all used
 * to go nowhere. Nothing else in this tree installs a devoptab for them.
 * ------------------------------------------------------------------------ */

static ssize_t std_write(struct _reent *r, void *fd, const char *ptr, size_t len)
{
	(void) r;
	(void) fd;
	if (ptr != NULL && len != 0 && !sCrashMode)
		buf_append(ptr, (u32) len);
	return len;
}

static devoptab_t dotab_log_out = { .name = "stdout", .write_r = std_write };
static devoptab_t dotab_log_err = { .name = "stderr", .write_r = std_write };

/* ------------------------------------------------------------------------ *
 * Memory
 *
 * The wrappers only record. Nothing here may format or lock: they run inside
 * every allocation in the program, including the ones newlib makes for
 * vsnprintf, and a report is printed by the heartbeat instead.
 * ------------------------------------------------------------------------ */

static struct
{
	u32 caller;
	u32 size;
	u32 count;
} sOom[OOM_SITES];

static u32 sOomSites;
static u32 sOomTotal;
static u32 sOomReported;
static u32 sFreeLow = 0xFFFFFFFFu;

static void note_oom(u32 caller, u32 size)
{
	u32 i;

	sOomTotal++;
	for (i = 0; i < sOomSites; i++)
	{
		if (sOom[i].caller == caller)
		{
			sOom[i].count++;
			sOom[i].size = size;
			return;
		}
	}
	if (sOomSites < OOM_SITES)
	{
		sOom[sOomSites].caller = caller;
		sOom[sOomSites].size = size;
		sOom[sOomSites].count = 1;
		sOomSites++;
	}
}

void *__real_malloc(size_t size);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *ptr, size_t size);
void *__real_memalign(size_t align, size_t size);

void *__wrap_malloc(size_t size)
{
	void *p = __real_malloc(size);
	if (p == NULL && size != 0)
		note_oom((u32) __builtin_return_address(0), (u32) size);
	return p;
}

void *__wrap_calloc(size_t n, size_t size)
{
	void *p = __real_calloc(n, size);
	if (p == NULL && n != 0 && size != 0)
		note_oom((u32) __builtin_return_address(0), (u32) (n * size));
	return p;
}

void *__wrap_realloc(void *ptr, size_t size)
{
	void *p = __real_realloc(ptr, size);
	if (p == NULL && size != 0)
		note_oom((u32) __builtin_return_address(0), (u32) size);
	return p;
}

void *__wrap_memalign(size_t align, size_t size)
{
	void *p = __real_memalign(align, size);
	if (p == NULL && size != 0)
		note_oom((u32) __builtin_return_address(0), (u32) size);
	return p;
}

static void log_oom(void)
{
	u32 i;

	if (sOomTotal == sOomReported)
		return;
	sOomReported = sOomTotal;

	DebugLog("            OUT OF MEMORY: %u failed allocations at %u sites\n",
		(unsigned) sOomTotal, (unsigned) sOomSites);
	for (i = 0; i < sOomSites; i++)
		DebugLog("              caller %08x  last size %u  x%u\n",
			(unsigned) sOom[i].caller, (unsigned) sOom[i].size,
			(unsigned) sOom[i].count);
}

/* ------------------------------------------------------------------------ *
 * Threads and their stacks
 *
 * Every thread goes through a trampoline, which is how its lwp_cntrl is found:
 * libogc hands a thread its handle, not its control block, but inside the
 * thread _thr_executing is exactly that block. From it the heartbeat can read
 * the thread's state and saved stack pointer without any libogc internals.
 * ------------------------------------------------------------------------ */

typedef struct
{
	void *(*entry)(void *);
	void *arg;
	u32 *stack;			/* lowest address */
	u32 size;
	u32 painted;		/* bytes from the bottom known to hold STACK_PAINT */
	u32 guard;			/* libogc2's DABR stack guard, 0 until known */
	lwp_cntrl *cntrl;
	u8 prio;
	volatile u8 state;	/* 0 free, 1 created, 2 running, 3 returned */
	bool overflowReported;
} ThreadSlot;

static ThreadSlot sThreads[MAX_THREADS];

/*
 * libogc2's stack guard, and the reason this file must know about it.
 *
 * _cpu_context_switch loads the DABR -- the CPU's data address breakpoint --
 * from each thread's context, and libogc2 arms it on a doubleword near the
 * bottom of every thread's stack, so that a stack overflow raises a DSI at
 * the moment it happens. The second hardware run of this file painted the
 * main thread's live stack straight through that word and took exactly that
 * DSI: DAR 806edf20, DSISR 02400000 (a store, and a DABR match), with the
 * paint pattern still in r8. Dolphin does not emulate the DABR, which is why
 * it did not notice.
 *
 * So the guarded doubleword is never written and never read. Only the
 * running thread's guard is readable, from the SPR itself; the low two bits
 * say whether it is armed at all.
 */
static u32 current_guard(void)
{
	u32 dabr = mfspr(1013);

	return (dabr & 3) ? (dabr & ~7u) : 0;
}

/*
 * What libogc2 itself keeps at the bottom of every stack, read out of
 * __lwp_thread_loadenv rather than assumed: the word at the base holds a
 * 0xDEADBABE canary, and the DABR is armed (write, flags 6) on the doubleword
 * at the base rounded up to 8. On a stack whose base is not 8-aligned those
 * are two different places, and the first version of this file read the
 * canary as a stack that had overflowed -- the heartbeat thread's own static
 * stack, at 805ad20c, reported "16384/16384 OVERFLOW" on the fourth hardware
 * run. Everything below the end of that doubleword belongs to libogc2.
 */
static bool is_guard(const ThreadSlot *t, u32 addr)
{
	u32 reserved = (((u32) t->stack + 7) & ~7u) + 8;

	if (addr < reserved)
		return true;
	return t->guard != 0 && addr >= t->guard && addr < t->guard + 8;
}

static void paint_range(ThreadSlot *t, u32 bytes)
{
	u32 i;

	for (i = 0; i < bytes / 4; i++)
		if (!is_guard(t, (u32) t->stack + i * 4))
			t->stack[i] = STACK_PAINT;
}

/* Paint the part of a live stack that is below the current frame. Safe: an
 * interrupt taken meanwhile pushes its frame lower still and is done with it
 * before this loop resumes. */
static void paint_live_stack(ThreadSlot *t)
{
	u32 sp = (u32) __builtin_frame_address(0);
	u32 base = (u32) t->stack;

	t->guard = current_guard();
	if (t->stack == NULL || sp <= base + 512 || sp > base + t->size)
		return;
	t->painted = ((sp - 512) - base) & ~3u;
	paint_range(t, t->painted);
}

static u32 stack_unused(const ThreadSlot *t)
{
	u32 i, n = t->painted / 4;

	for (i = 0; i < n; i++)
	{
		if (is_guard(t, (u32) t->stack + i * 4))
			continue;
		if (t->stack[i] != STACK_PAINT)
			break;
	}
	return i * 4;
}

static void *thread_trampoline(void *arg)
{
	ThreadSlot *t = (ThreadSlot *) arg;
	void *ret;

	t->cntrl = _thr_executing;
	t->guard = current_guard();
	if (t->stack == NULL && t->cntrl != NULL)
	{
		/* libogc allocated this stack itself. */
		t->stack = (u32 *) t->cntrl->stack;
		t->size = t->cntrl->stack_size;
		paint_live_stack(t);
	}
	t->state = 2;
	ret = t->entry(t->arg);
	t->state = 3;
	return ret;
}

static ThreadSlot *claim_slot(void *stackbase)
{
	ThreadSlot *t = NULL;
	u32 i;

	LWP_MutexLock(sThreadLock);
	/* The GUI threads are recreated on the same static stacks every time the
	 * menu comes back: reuse their slot rather than filling the table. */
	for (i = 1; i < MAX_THREADS && t == NULL; i++)
		if (stackbase != NULL && (void *) sThreads[i].stack == stackbase &&
			sThreads[i].state == 3)
			t = &sThreads[i];
	for (i = 1; i < MAX_THREADS && t == NULL; i++)
		if (sThreads[i].state == 0)
			t = &sThreads[i];
	if (t != NULL)
		t->state = 1;
	LWP_MutexUnlock(sThreadLock);
	return t;
}

s32 __real_LWP_CreateThread(lwp_t *thethread, void *(*entry)(void *), void *arg,
	void *stackbase, u32 stack_size, u8 prio);

s32 __wrap_LWP_CreateThread(lwp_t *thethread, void *(*entry)(void *), void *arg,
	void *stackbase, u32 stack_size, u8 prio)
{
	ThreadSlot *t;
	s32 ret;

	if (!sInit || (t = claim_slot(stackbase)) == NULL)
		return __real_LWP_CreateThread(thethread, entry, arg, stackbase, stack_size, prio);

	t->entry = entry;
	t->arg = arg;
	t->stack = (u32 *) stackbase;
	t->size = stack_size;
	t->painted = 0;
	t->cntrl = NULL;
	t->prio = prio;
	t->overflowReported = false;

	/* A stack we were handed and nobody runs on yet: paint all of it. The
	 * armed guard here is the *calling* thread's, and skipping it costs
	 * nothing; the new thread's own guard is recorded by the trampoline. */
	t->guard = current_guard();
	if (stackbase != NULL && stack_size >= 64)
	{
		t->painted = stack_size & ~3u;
		paint_range(t, t->painted);
	}

	ret = __real_LWP_CreateThread(thethread, thread_trampoline, t, stackbase, stack_size, prio);
	if (ret < 0)
	{
		t->state = 0;
		DebugLog("thread: create FAILED (%d), entry %08x prio %u\n", (int) ret,
			(unsigned) (u32) entry, (unsigned) prio);
	}
	else
		DebugLog("thread: #%u entry %08x stack %08x size %u prio %u\n",
			(unsigned) (t - sThreads), (unsigned) (u32) entry,
			(unsigned) (u32) stackbase, (unsigned) stack_size, (unsigned) prio);
	return ret;
}

static void check_stacks(void)
{
	u32 i;

	for (i = 0; i < MAX_THREADS; i++)
	{
		ThreadSlot *t = &sThreads[i];

		if (t->state == 0 || t->overflowReported || t->painted < 4)
			continue;
		if (stack_unused(t) == 0)
		{
			t->overflowReported = true;
			DebugMark("STACK OVERFLOW: thread #%u entry %08x, %u-byte stack %08x",
				(unsigned) i, (unsigned) (u32) t->entry, (unsigned) t->size,
				(unsigned) (u32) t->stack);
		}
	}
}

static bool is_code(u32 w)
{
	return w >= CODE_START && w < CODE_END && (w & 3) == 0;
}

static void dump_threads(void)
{
	u32 i, j, k;

	for (i = 0; i < MAX_THREADS; i++)
	{
		ThreadSlot *t = &sThreads[i];
		char code[80];
		u32 sp = 0, lr = 0, state = 0xFFFFFFFFu;
		int len = 0;

		if (t->state == 0 || t->state == 3)
			continue;
		if (t->cntrl != NULL && t->cntrl != _thr_executing)
		{
			sp = t->cntrl->context.gpr[1];
			lr = t->cntrl->context.lr;
			state = t->cntrl->cur_state;
		}

		/* Where the thread is parked: the first few code addresses above its
		 * saved stack pointer. addr2line turns them into a call chain. */
		code[0] = 0;
		if (sp != 0 && t->stack != NULL && sp >= (u32) t->stack &&
			sp < (u32) t->stack + t->size)
		{
			for (j = 0, k = 0; j < 96 && k < 5; j++)
			{
				u32 addr = sp + j * 4;
				u32 w;

				if (addr >= (u32) t->stack + t->size)
					break;
				w = *(u32 *) addr;
				if (is_code(w))
				{
					len += snprintf(code + len, sizeof(code) - len, " %08x", (unsigned) w);
					k++;
				}
			}
		}

		DebugLog("            thr #%-2u entry %08x prio %3u state %08x stack %u/%u%s sp %08x lr %08x |%s\n",
			(unsigned) i, (unsigned) (u32) t->entry, (unsigned) t->prio,
			(unsigned) state,
			(unsigned) (t->painted ? t->size - stack_unused(t) : 0), (unsigned) t->size,
			t->overflowReported ? " OVERFLOW" : "",
			(unsigned) sp, (unsigned) lr, code);
	}
}

/* ------------------------------------------------------------------------ *
 * The heartbeat
 *
 * What makes a hang readable. A thread spinning at priority 60 or 70 cannot
 * starve this one; if the beats stop, interrupts are off or the scheduler is
 * dead, and that is itself the finding. If they continue while "last" does not
 * move, the dump names which thread is stuck and where.
 * ------------------------------------------------------------------------ */

static u8 sHeartbeatStack[HEARTBEAT_STACK];
static lwp_t sHeartbeatThread = LWP_THREAD_NULL;

static void heartbeat(u32 beat)
{
	char last[sizeof(sLastMark)];
	struct mallinfo mi;
	u32 ms = uptime_ms();
	u32 top = (u32) SYS_GetArena1Hi() - (u32) SYS_GetArena1Lo();
	u32 freeTotal;
	char *nl;

	LWP_MutexLock(sBufLock);
	memcpy(last, sLastMark, sizeof(last));
	LWP_MutexUnlock(sBufLock);
	nl = strchr(last, '\n');
	if (nl != NULL)
		*nl = 0;

	DebugLog("[%5u.%03u] hb %u | oom %u | last %s\n", (unsigned) (ms / 1000),
		(unsigned) (ms % 1000), (unsigned) beat, (unsigned) sOomTotal, last);
	log_oom();
	check_stacks();

	/* Flushed before mallinfo, which takes newlib's malloc lock: if a dead
	 * thread holds it, this beat still reaches the card and the next one never
	 * comes -- which says exactly that. */
	DebugLogFlush();

	mi = mallinfo();
	freeTotal = (u32) mi.fordblks + top;
	if (freeTotal < sFreeLow)
		sFreeLow = freeTotal;
	DebugLog("            mem free %uK (in heap %uK + never claimed %uK), low %uK, heap %uK\n",
		(unsigned) (freeTotal / 1024), (unsigned) (mi.fordblks / 1024),
		(unsigned) (top / 1024), (unsigned) (sFreeLow / 1024),
		(unsigned) (mi.arena / 1024));

	if (beat % THREAD_DUMP_EVERY == 1)
		dump_threads();
}

static void *heartbeat_thread(void *arg)
{
	u32 beat = 0;

	(void) arg;
	while (!sCrashMode)
	{
		usleep(HEARTBEAT_MS * 1000);
		if (sCrashMode)
			break;
		heartbeat(++beat);
	}
	return NULL;
}

/* ------------------------------------------------------------------------ *
 * Setup
 * ------------------------------------------------------------------------ */

void DebugLogInit(void)
{
	ThreadSlot *m = &sThreads[0];

	if (sInit)
		return;

	sBootTicks = gettime();
	LWP_MutexInit(&sBufLock, false);
	LWP_MutexInit(&sIoLock, false);
	LWP_MutexInit(&sThreadLock, false);
	sInit = true;

	/* The screen comes first, before anything that can fail. CON_Init
	 * installs the console as stdout; its devoptab is kept for the marks,
	 * and stdout is then taken over by the log. */
	{
		GXRModeObj *rmode;

		VIDEO_Init();
		rmode = VIDEO_GetPreferredMode(NULL);
		sScreenXfb = (u32 *) MEM_K0_TO_K1(SYS_AllocateFramebuffer(rmode));
		if (sScreenXfb != NULL)
		{
			VIDEO_Configure(rmode);
			VIDEO_ClearFrameBuffer(rmode, sScreenXfb, COLOR_BLACK);
			CON_Init(sScreenXfb, 24, 32, rmode->fbWidth, rmode->xfbHeight,
				rmode->fbWidth * VI_DISPLAY_PIX_SZ);
			sScreenTab = devoptab_list[STD_OUT];
			VIDEO_SetNextFramebuffer(sScreenXfb);
			VIDEO_SetBlack(FALSE);
			VIDEO_Flush();
			VIDEO_WaitVSync();
			sScreen = true;
			{
				static const char kTitle[] =
					"WiiMC-GCN-Online boot trace, build " WIIMC_BUILD_ID "\n\n";
				screen_write(kTitle, sizeof(kTitle) - 1);
			}
		}
	}

	devoptab_list[STD_OUT] = &dotab_log_out;
	devoptab_list[STD_ERR] = &dotab_log_err;
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	/* The main thread runs the whole menu, not just the boot, and it did not
	 * come through LWP_CreateThread. Slot 0 is its own. */
	m->cntrl = _thr_executing;
	m->entry = NULL;
	m->prio = m->cntrl ? m->cntrl->cur_prio : 0;
	m->stack = m->cntrl ? (u32 *) m->cntrl->stack : NULL;
	m->size = m->cntrl ? m->cntrl->stack_size : 0;
	m->state = 2;
	paint_live_stack(m);

	DebugMark("boot: log started; main thread stack %08x size %u sp %08x",
		(unsigned) (u32) m->stack, (unsigned) m->size,
		(unsigned) (u32) __builtin_frame_address(0));
}

/*
 * The crash file, and why it is a second file held open all session.
 *
 * The fourth hardware run entered Settings, died within 300 ms, and left no
 * CRASH block anywhere in wiimc.log. The report path opened the log with
 * fopen, and fopen allocates its FILE -- from the heap, under newlib's malloc
 * lock -- which is precisely what a crash in C++ GUI code is likely to have
 * corrupted. A descriptor opened here, at boot, needs neither at crash time:
 * lseek and write go straight to the filesystem driver.
 *
 * A file of its own rather than a second handle on the log, so that the
 * filesystem's open-file locking can never refuse the log's own flushes.
 *
 * A report left by the previous run is kept as wiimc-crash-prev.txt, since the
 * boot that reads it is usually a later one.
 */
static void open_crash_file(void)
{
	static char pad[4096];
	char head[64];
	u32 written = 0;
	int fd, i, n;
	bool used = false;

	fd = open(CRASH_PATH, O_RDONLY);
	if (fd >= 0)
	{
		n = read(fd, head, sizeof(head));
		close(fd);
		for (i = 0; i < n; i++)
			if (head[i] != '\n')
				used = true;
	}
	remove(used ? CRASH_PREV_PATH : CRASH_PATH);
	if (used)
		rename(CRASH_PATH, CRASH_PREV_PATH);

	sCrashFd = open(CRASH_PATH, O_RDWR | O_CREAT | O_TRUNC, 0666);
	if (sCrashFd < 0)
	{
		DebugLog("log: cannot create " CRASH_PATH "\n");
		return;
	}
	memset(pad, '\n', sizeof(pad));
	while (written < LOG_CRASH_RESERVE && write(sCrashFd, pad, sizeof(pad)) == sizeof(pad))
		written += sizeof(pad);
	fsync(sCrashFd);
	DebugLog("log: crash file " CRASH_PATH " held open (%u KB)%s\n",
		(unsigned) (written / 1024), used ? ", previous report kept as " CRASH_PREV_PATH : "");
}

void DebugLogScreenShow(void)
{
	if (!sScreen)
		return;
	/* InitVideo() reran VIDEO_Init and blanked the display. Same mode family,
	 * same framebuffer size: point the display back at the console. */
	VIDEO_SetNextFramebuffer(sScreenXfb);
	VIDEO_SetBlack(FALSE);
	VIDEO_Flush();
	VIDEO_WaitVSync();
}

void DebugLogScreenEnd(void)
{
	if (!sScreen)
		return;
	/* InitVideo2() has set WiiMC's own framebuffer. The console's is no
	 * longer displayed, and only the marks ever wrote to it. */
	sScreen = false;
	free(MEM_K1_TO_K0(sScreenXfb));
	sScreenXfb = NULL;
}

void DebugLogAttachCard(void)
{
	static char pad[4096];
	char header[512];
	u32 written = 0;
	FILE *f;
	int n;

	if (!sInit || sCardReady)
		return;

	/* Not "try and see": with no sd1: device, newlib's _rename_r finds -1 for
	 * both paths, takes them as the same device and reads devoptab_list[-1].
	 * Dolphin, which has no SD reader, faulted at 0x30 on exactly that. */
	if (FindDevice("sd1:") < 0)
	{
		DebugMark("log: sd1: is not mounted, no log file this run");
		return;
	}

	/* The previous run is kept: the one that crashed is usually the one before
	 * the boot that is reading this. */
	remove(LOG_PREV_PATH);
	rename(LOG_PATH, LOG_PREV_PATH);

	f = fopen(LOG_PATH, "wb");
	if (f == NULL)
	{
		DebugMark("log: cannot create " LOG_PATH);
		return;
	}
	memset(pad, '\n', sizeof(pad));
	while (written < LOG_FILE_SIZE && fwrite(pad, 1, sizeof(pad), f) == sizeof(pad))
		written += sizeof(pad);
	fclose(f);

	sAppendMode = (written < LOG_FILE_SIZE);
	sOffset = 0;
	sCrashOffset = 0;
	sCardReady = true;

	open_crash_file();

	n = snprintf(header, sizeof(header),
		"=== WiiMC-GCN-Online debug log ===\n"
		"build   " WIIMC_BUILD_ID " (ELF: deployed/wiimc-" WIIMC_BUILD_ID ".elf)\n"
		"file    " LOG_PATH ", %u KB: %u KB body that wraps, last %u KB reserved for the crash report%s\n"
		"arena1  %08x-%08x\n"
		"code    %08x-%08x (stack words in this range are return addresses)\n"
		"resolve powerpc-eabi-addr2line -f -C -e wiimc.elf <address>\n\n",
		(unsigned) (LOG_FILE_SIZE / 1024), (unsigned) (LOG_BODY_SIZE / 1024),
		(unsigned) (LOG_CRASH_RESERVE / 1024),
		sAppendMode ? " -- FALLBACK: preallocation failed, appending" : "",
		(unsigned) (u32) SYS_GetArena1Lo(), (unsigned) (u32) SYS_GetArena1Hi(),
		(unsigned) CODE_START, (unsigned) CODE_END);

	LWP_MutexLock(sIoLock);
	write_card(header, (u32) n, 0, false);
	LWP_MutexUnlock(sIoLock);

	/* Everything logged since boot, then the header ends here: a wrap must not
	 * overwrite the lines that identify the run. */
	DebugLogFlush();
	sHeaderEnd = sOffset;

	LWP_CreateThread(&sHeartbeatThread, heartbeat_thread, NULL, sHeartbeatStack,
		HEARTBEAT_STACK, HEARTBEAT_PRIO);
}

/* ------------------------------------------------------------------------ *
 * exit, abort, assert
 *
 * The quiet deaths. libogc2's exit goes back to the loader, so from the couch
 * this looks exactly like a crash -- and returning from main() is an exit.
 * ------------------------------------------------------------------------ */

void __real_exit(int code) __attribute__((noreturn));
void __real_abort(void) __attribute__((noreturn));
void __real___assert_func(const char *file, int line, const char *func,
	const char *expr) __attribute__((noreturn));

/* While the boot console is the only record -- no card yet -- a quiet death
 * would take the screen with it straight back to the loader. Hold it. */
static void hold_screen(void)
{
	if (sScreen && !sCardReady)
	{
		static const char kHold[] = "\nno log file yet: holding this screen for 30 s\n";
		screen_write(kHold, sizeof(kHold) - 1);
		sleep(30);
	}
}

void __wrap_exit(int code)
{
	DebugMark("EXIT: exit(%d) called from %08x (main returning also lands here)", code,
		(unsigned) (u32) __builtin_return_address(0));
	log_oom();
	DebugLogFlush();
	hold_screen();
	__real_exit(code);
}

void __wrap_abort(void)
{
	DebugMark("ABORT: abort() called from %08x (C++ std::terminate lands here too)",
		(unsigned) (u32) __builtin_return_address(0));
	log_oom();
	DebugLogFlush();
	hold_screen();
	__real_abort();
}

void __wrap___assert_func(const char *file, int line, const char *func, const char *expr)
{
	DebugMark("ASSERT: %s:%d %s(): %s", file ? file : "?", line, func ? func : "?",
		expr ? expr : "?");
	log_oom();
	DebugLogFlush();
	hold_screen();
	__real___assert_func(file, line, func, expr);
}

/* ------------------------------------------------------------------------ *
 * The exception handler
 *
 * libogc's vector stub ends in `mtsrr0; rfi` into default_exceptionhandler,
 * an assembly routine that saves the whole frame_context and then calls
 * c_default_exceptionhandler(frame_context *) -- an ordinary C call, from
 * exception_handler.o, to a symbol defined in exception.o. -Wl,--wrap catches
 * exactly that call. Nothing is patched at run time, and libogc's own register
 * dump still runs afterwards.
 *
 * Everything from here down formats by hand. See the header for why.
 * ------------------------------------------------------------------------ */

static void crash_puts(const char *s)
{
	u32 n = 0;

	while (s[n] != 0)
		n++;
	buf_append(s, n);
}

static void crash_hex(u32 v)
{
	static const char kDigits[] = "0123456789abcdef";
	char out[8];
	int i;

	for (i = 7; i >= 0; i--)
	{
		out[i] = kDigits[v & 0xF];
		v >>= 4;
	}
	buf_append(out, 8);
}

static void crash_dec(u32 v)
{
	char out[12];
	int i = sizeof(out);

	do
	{
		out[--i] = (char) ('0' + v % 10);
		v /= 10;
	} while (v != 0);
	buf_append(out + i, (u32) (sizeof(out) - i));
}

static const char *exception_name(u32 n)
{
	switch (n)
	{
		case EX_SYS_RESET:	return "system reset";
		case EX_MACH_CHECK:	return "machine check";
		case EX_DSI:		return "DSI (bad data address)";
		case EX_ISI:		return "ISI (bad instruction address)";
		case EX_INT:		return "external interrupt";
		case EX_ALIGN:		return "alignment";
		case EX_PRG:		return "program (illegal instruction or trap)";
		case EX_FP:			return "floating point unavailable";
		case EX_DEC:		return "decrementer";
		case EX_SYS_CALL:	return "system call";
		case EX_TRACE:		return "trace";
		case EX_PERF:		return "performance monitor";
		case EX_IABR:		return "instruction breakpoint";
		case EX_THERM:		return "thermal";
		default:			return "unknown";
	}
}

/* Keep only the tail of what was still waiting to be flushed, so the report
 * behind it fits in the reserve. */
static void crash_trim_buffer(void)
{
	static const char kCut[] = "[...]\n";
	u32 i, from;

	if (sLen <= LOG_CRASH_KEEP)
		return;
	from = sLen - LOG_CRASH_KEEP;
	for (i = 0; i < sizeof(kCut) - 1; i++)
		sBuf[i] = kCut[i];
	for (i = 0; i < LOG_CRASH_KEEP; i++)
		sBuf[sizeof(kCut) - 1 + i] = sBuf[from + i];
	sLen = LOG_CRASH_KEEP + sizeof(kCut) - 1;
}

static void crash_report(const frame_context *ctx, u32 dsisr, u32 dar)
{
	u32 sp = ctx->gpr[1];
	u32 i;

	crash_puts("\n=================== CRASH ===================\n");
	crash_puts("exception ");
	crash_dec(ctx->nExcept);
	crash_puts(", ");
	crash_puts(exception_name(ctx->nExcept));
	crash_puts("\nsrr0     ");
	crash_hex(ctx->srr0);
	crash_puts("   <- the faulting instruction, feed to addr2line\nlr       ");
	crash_hex(ctx->lr);
	crash_puts("   <- usually its caller\nsrr1     ");
	crash_hex(ctx->srr1);
	crash_puts("   msr ");
	crash_hex(ctx->msr);
	crash_puts("\ndsisr    ");
	crash_hex(dsisr);
	crash_puts("   dar ");
	crash_hex(dar);
	crash_puts("   <- for a DSI, the address that could not be accessed\nctr      ");
	crash_hex(ctx->ctr);
	crash_puts("   cr ");
	crash_hex(ctx->cr);
	crash_puts("   xer ");
	crash_hex(ctx->xer);
	crash_puts("\nuptime   ");
	crash_dec(uptime_ms());
	crash_puts(" ms\nlast     ");
	sLastMark[sizeof(sLastMark) - 1] = 0;
	crash_puts(sLastMark[0] ? sLastMark : "(no mark)\n");

	crash_puts("thread   ");
	for (i = 0; i < MAX_THREADS; i++)
	{
		if (sThreads[i].state != 0 && sThreads[i].cntrl == _thr_executing)
		{
			crash_puts("#");
			crash_dec(i);
			crash_puts(i == 0 ? " (main)" : " entry ");
			if (i != 0)
				crash_hex((u32) sThreads[i].entry);
			break;
		}
	}
	if (i == MAX_THREADS)
		crash_puts("unknown (interrupt context, or not created through LWP_CreateThread)");
	crash_puts("\n");

	for (i = 0; i < 32; i++)
	{
		crash_puts("r");
		crash_dec(i);
		crash_puts(i < 10 ? "  " : " ");
		crash_hex(ctx->gpr[i]);
		crash_puts((i & 3) == 3 ? "\n" : "   ");
	}

	if (sOomTotal != 0)
	{
		crash_puts("OUT OF MEMORY before the crash: ");
		crash_dec(sOomTotal);
		crash_puts(" failed allocations\n");
		for (i = 0; i < sOomSites; i++)
		{
			crash_puts("  caller ");
			crash_hex(sOom[i].caller);
			crash_puts("  size ");
			crash_dec(sOom[i].size);
			crash_puts("  x");
			crash_dec(sOom[i].count);
			crash_puts("\n");
		}
	}

	crash_puts("stacks (used/size)\n");
	for (i = 0; i < MAX_THREADS; i++)
	{
		const ThreadSlot *t = &sThreads[i];

		if (t->state == 0 || t->state == 3 || t->painted == 0)
			continue;
		crash_puts("  #");
		crash_dec(i);
		crash_puts(" entry ");
		crash_hex((u32) t->entry);
		crash_puts("  ");
		crash_dec(t->size - stack_unused(t));
		crash_puts("/");
		crash_dec(t->size);
		crash_puts(stack_unused(t) == 0 ? "  OVERFLOWED\n" : "\n");
	}

	/* No symbol table on the console: these are the words to hand to
	 * addr2line. Only those that look like code are worth printing. The
	 * stack pointer is range-checked, since a crash may have ruined it. */
	crash_puts("call stack candidates (feed to addr2line)\n");
	if (sp >= 0x80000000u && sp < 0x81800000u && (sp & 3) == 0)
	{
		for (i = 0; i < CRASH_STACK_WORDS; i++)
		{
			u32 addr = sp + i * 4;
			u32 w;

			if (addr >= 0x81800000u)
				break;
			w = *(u32 *) addr;
			if (is_code(w))
			{
				crash_puts("  sp+");
				crash_dec(i * 4);
				crash_puts("  ");
				crash_hex(w);
				crash_puts("\n");
			}
		}
	}
	else
	{
		crash_puts("  sp ");
		crash_hex(sp);
		crash_puts(" is not a MEM1 address: the stack pointer itself is corrupt\n");
	}
	crash_puts("=============================================\n");
}

void __real_c_default_exceptionhandler(frame_context *ctx);

void __wrap_c_default_exceptionhandler(frame_context *ctx)
{
	static volatile bool sInHandler;
	static frame_context sFirstCtx;
	u32 msr, dsisr, dar, i;

	/* A second fault -- almost always the card write below. Do not let its
	 * frame replace the first one on screen. */
	if (sInHandler)
	{
		crash_puts("\n*** second exception ");
		crash_dec(ctx->nExcept);
		crash_puts(" at ");
		crash_hex(ctx->srr0);
		crash_puts(" during the report; the screen shows the FIRST one ***\n");
		__real_c_default_exceptionhandler(&sFirstCtx);
		return;
	}
	sInHandler = true;

	/* Before anything else: memcpy is wrapped to an FPU-using version in this
	 * tree, and an exception leaves MSR[FP] clear. */
	msr = mfmsr();
	mtmsr(msr | MSR_FP);
	dsisr = mfspr(18);
	dar = mfspr(19);

	for (i = 0; i < sizeof(sFirstCtx) / sizeof(u32); i++)
		((u32 *) &sFirstCtx)[i] = ((const u32 *) ctx)[i];

	sCrashMode = true;
	crash_trim_buffer();

	/* The whole report into RAM with interrupts still off, so a second fault
	 * during the card write can only cost the card copy. */
	crash_report(ctx, dsisr, dar);

	/* Interrupts on for the card write only: libfat's locks and the SD
	 * adapter's EXI transfers both need them. */
	if (sCardReady)
	{
		u32 len = sLen;

		mtmsr(msr | MSR_FP | MSR_EE);
		/* The descriptor opened at boot first: no fopen, so no malloc and
		 * no heap -- which may be exactly what just broke. Then the log's
		 * reserved tail, which does need both. */
		if (sCrashFd >= 0)
		{
			lseek(sCrashFd, 0, SEEK_SET);
			write(sCrashFd, sBuf, len);
			fsync(sCrashFd);
		}
		write_card(sBuf, len, sDropped, true);
		sLen = 0;
		mtmsr(msr | MSR_FP);
	}

	__real_c_default_exceptionhandler(ctx);
}

#endif /* WANT_DEBUGLOG */
