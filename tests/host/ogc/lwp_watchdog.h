#ifndef WIIMC_TEST_WATCHDOG_H
#define WIIMC_TEST_WATCHDOG_H
#include <stdint.h>
#include <time.h>
static inline uint64_t gettime(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
#define ticks_to_millisecs(t) ((t) / 1000)
#define ticks_to_microsecs(t) (t)
#endif
