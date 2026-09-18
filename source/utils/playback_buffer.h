#ifndef WIIMC_PLAYBACK_BUFFER_H
#define WIIMC_PLAYBACK_BUFFER_H

/* One compressed-data ring for the current WebDAV track. Capacity is a
 * read-ahead ceiling, not an amount that must be downloaded before play. */
#define DAV_AUDIO_CACHE_BYTES (512 * 1024)
#define DAV_AUDIO_PROBE_BYTES (16 * 1024)

static inline int dav_audio_start_bytes(int bytes_per_second)
{
    /* Unknown bitrate: allow three seconds at the MP3 maximum, 320 kbit/s.
     * Bound other formats so the target always fits the forward cache even
     * after backward-seek history has occupied a quarter of the ring. */
    if (bytes_per_second <= 0) bytes_per_second = 40000;
    if (bytes_per_second >= DAV_AUDIO_CACHE_BYTES / 2 / 3)
        return DAV_AUDIO_CACHE_BYTES / 2;
    int bytes = bytes_per_second * 3;
    return bytes < DAV_AUDIO_PROBE_BYTES ? DAV_AUDIO_PROBE_BYTES : bytes;
}

#endif
