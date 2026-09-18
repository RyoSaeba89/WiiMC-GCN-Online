/* Host adapters for exercising the production GameCube TLS/cache code. */
#ifndef WIIMC_TEST_GCCORE_H
#define WIIMC_TEST_GCCORE_H
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef pthread_mutex_t *mutex_t;
#define LWP_MUTEX_NULL NULL
static inline int LWP_MutexInit(mutex_t *m, bool recursive)
{
    (void)recursive;
    *m = malloc(sizeof(**m));
    return *m ? pthread_mutex_init(*m, NULL) : -1;
}
static inline int LWP_MutexLock(mutex_t m) { return pthread_mutex_lock(m); }
static inline int LWP_MutexUnlock(mutex_t m) { return pthread_mutex_unlock(m); }
#endif
