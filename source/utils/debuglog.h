/****************************************************************************
 * WiiMC-GCN-Online
 *
 * debuglog.h
 *
 * A log file on the SD card, and everything needed to make a crash that shows
 * nothing on screen leave something behind. See debuglog.c for the design and
 * PORTING.md section 11 for how to read the file.
 *
 * Built only with ENABLE_DEBUGLOG = 1 in Makefile.gc. Without it every call
 * below compiles to nothing.
 ***************************************************************************/

#ifndef _DEBUGLOG_H_
#define _DEBUGLOG_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WANT_DEBUGLOG

/* First line of main(): RAM buffer, stdout/stderr capture, main stack paint.
 * Nothing reaches the card until DebugLogAttachCard. */
void DebugLogInit(void);

/* Once sd1: is mounted: create the file, write out what was buffered since
 * boot, and start the heartbeat thread. */
void DebugLogAttachCard(void);

/* The boot screen. Between InitVideo() -- which blanks the display -- and
 * InitVideo2() -- which gives it a framebuffer -- nothing can be seen, not
 * even libogc's exception dump. DebugLogScreenShow() puts the boot console
 * back after InitVideo(); DebugLogScreenEnd() hands the display to the menu
 * once InitVideo2() has run. */
void DebugLogScreenShow(void);
void DebugLogScreenEnd(void);

/* Buffered. Reaches the card with the next heartbeat, within two seconds. */
void DebugLog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* A breadcrumb: timestamped, remembered as "the last thing that happened",
 * and flushed to the card before it returns. For the places a crash report
 * has to be able to name. */
void DebugMark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void DebugLogFlush(void);

#else

#define DebugLogInit()
#define DebugLogAttachCard()
#define DebugLogScreenShow()
#define DebugLogScreenEnd()
#define DebugLog(...)
#define DebugMark(...)
#define DebugLogFlush()

#endif

#ifdef __cplusplus
}
#endif

#endif
