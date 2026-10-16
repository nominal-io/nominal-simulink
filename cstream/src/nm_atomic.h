/*
 * nm_atomic.h — the two atomic operations the ring buffer needs, and nothing else.
 *
 * C11 <stdatomic.h> would be the obvious answer, but MSVC supports it only from Visual
 * Studio 2022 17.5, and then as an experimental option, and we need this to compile
 * with qcc 7.1 (gcc 8.3) as well. The shim is
 * small enough that avoiding the portability question is cheaper than fighting it.
 *
 * Both real-time targets, QNX and Antelope, are x86-64, as is the Windows build. There,
 * aligned word-sized loads and stores are already
 * atomic and the hardware memory model gives acquire/release ordering on ordinary
 * accesses. What remains is stopping the *compiler* from reordering them.
 */

#ifndef NM_ATOMIC_H
#define NM_ATOMIC_H

#include <stddef.h>

#if defined(_MSC_VER)

#include <intrin.h>

/* MSVC's default /volatile:ms already gives volatile accesses acquire/release
 * semantics on x86/x64; _ReadWriteBarrier pins the compiler ordering. */
#define NM_LOAD_ACQUIRE(p)     (_ReadWriteBarrier(), *(volatile size_t *)(p))
#define NM_STORE_RELEASE(p, v) do { _ReadWriteBarrier(); *(volatile size_t *)(p) = (v); } while (0)

#else

#define NM_LOAD_ACQUIRE(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define NM_STORE_RELEASE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELEASE)

#endif

#endif /* NM_ATOMIC_H */
