/*
 * nm_thread.h — the minimum threading surface this library needs.
 *
 * Only start, join, sleep, and a monotonic clock. There is deliberately no condition
 * variable or mutex:
 * the producer never signals the sender, so the sender simply wakes on its batching
 * interval. That keeps nm_push free of any synchronisation primitive that could block
 * a real-time caller, and it keeps this shim small enough to be obviously correct on
 * pthreads (QNX Neutrino, Antelope) and Win32 alike.
 */

#ifndef NM_THREAD_H
#define NM_THREAD_H

#include <stdint.h>

#if defined(_WIN32)

#include <windows.h>

typedef HANDLE nm_thread;

static inline int nm_thread_start(nm_thread *t, unsigned long (__stdcall *fn)(void *), void *arg)
{
    *t = CreateThread(NULL, 0, fn, arg, 0, NULL);
    return (*t == NULL) ? -1 : 0;
}

static inline void nm_thread_join(nm_thread t)
{
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

static inline void nm_sleep_ms(uint32_t ms)
{
    Sleep(ms);
}

/* Nanoseconds on a clock that only ever moves forwards, from an arbitrary start. For
 * timing the sender, never for timestamps: unlike nm_now_nanos, it doesn't step when the
 * computer's clock is set or corrected. Split into seconds and the rest so the
 * multiplication can't overflow however long the computer has been up. */
static inline int64_t nm_mono_nanos(void)
{
    LARGE_INTEGER count, freq;

    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&freq);
    return (count.QuadPart / freq.QuadPart) * 1000000000LL +
           (int64_t)((uint64_t)(count.QuadPart % freq.QuadPart) * 1000000000ULL /
                     (uint64_t)freq.QuadPart);
}

#define NM_THREAD_FN(name, arg) unsigned long __stdcall name(void *arg)
#define NM_THREAD_RETURN        return 0

#else

#include <pthread.h>
#include <time.h>

typedef pthread_t nm_thread;

static inline int nm_thread_start(nm_thread *t, void *(*fn)(void *), void *arg)
{
    return pthread_create(t, NULL, fn, arg) == 0 ? 0 : -1;
}

static inline void nm_thread_join(nm_thread t)
{
    pthread_join(t, NULL);
}

static inline void nm_sleep_ms(uint32_t ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}

/* Nanoseconds on a clock that only ever moves forwards, from an arbitrary start. For
 * timing the sender, never for timestamps: unlike nm_now_nanos, it doesn't step when the
 * computer's clock is set or corrected. */
static inline int64_t nm_mono_nanos(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

#define NM_THREAD_FN(name, arg) void *name(void *arg)
#define NM_THREAD_RETURN        return NULL

#endif

#endif /* NM_THREAD_H */
