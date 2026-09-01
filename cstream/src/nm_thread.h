/*
 * nm_thread.h — the minimum threading surface this library needs.
 *
 * Only start, join, and sleep. There is deliberately no condition variable or mutex:
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

#define NM_THREAD_FN(name, arg) void *name(void *arg)
#define NM_THREAD_RETURN        return NULL

#endif

#endif /* NM_THREAD_H */
