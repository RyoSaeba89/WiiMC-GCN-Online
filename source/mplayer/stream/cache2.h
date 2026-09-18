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

#ifndef MPLAYER_CACHE2_H
#define MPLAYER_CACHE2_H

#include "stream.h"

extern volatile float cache_fill_status;

/* Read by the WiiMC WebDAV scheduler from another LWP thread. */
float MPlayerCacheFillPercent(void);

/* Wait at the current read position without reallocating the cache.
 * Returns 1 on success/EOF, 0 on cancellation, -1 on failure. */
int stream_cache_prefill(stream_t *stream, int min);
void cache_uninit(stream_t *s);
int cache_do_control(stream_t *stream, int cmd, void *arg);

#endif /* MPLAYER_CACHE2_H */
