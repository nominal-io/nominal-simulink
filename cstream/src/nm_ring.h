/*
 * nm_ring.h — a fixed-capacity single-producer/single-consumer row buffer.
 *
 * One row is a timestamp plus one value per channel. The producer is the caller's
 * real-time thread; the consumer is our sender thread. All storage is reserved once,
 * in nm_ring_init, so nm_ring_push neither allocates nor blocks: when the buffer is
 * full it discards the row and says so, which is the behaviour a control loop needs.
 *
 * The producer does not signal the consumer. The sender wakes on its own batching
 * schedule instead, which keeps nm_ring_push down to a bounds check, a memcpy and one
 * release-store — no mutex, no condition variable, nothing that can block.
 *
 * Indices are free-running counters rather than wrapped offsets, so "full" is a
 * subtraction and no slot has to be sacrificed to distinguish full from empty. They
 * are size_t and would need ~2^64 rows to wrap on a 64-bit target.
 */

#ifndef NM_RING_H
#define NM_RING_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    int64_t *timestamps;   /* capacity entries */
    double  *values;       /* capacity * channel_count entries, row-major */
    size_t   capacity;
    size_t   channel_count;

    /* Written by the producer, read by the consumer, and vice versa. Access only
     * through NM_LOAD_ACQUIRE / NM_STORE_RELEASE. */
    size_t head;
    size_t tail;
} nm_ring;

/* Reserve storage. Returns 0 on success, -1 on allocation failure. */
int  nm_ring_init(nm_ring *r, size_t capacity_rows, size_t channel_count);
void nm_ring_free(nm_ring *r);

/*
 * Producer side. Copies one row in. Returns 1 if the row was accepted, 0 if it was
 * discarded because the buffer was full. Never blocks, never allocates.
 */
int nm_ring_push(nm_ring *r, int64_t timestamp_nanos, const double *values);

/*
 * Consumer side. Reports how many rows are readable, and gives direct access to a
 * contiguous run of them starting at the tail — the caller encodes straight out of
 * ring storage rather than copying to a staging buffer.
 *
 * A run stops at the end of the backing array, so a wrapped buffer needs two calls.
 * Returns the number of rows in the run, 0 if empty.
 */
size_t nm_ring_readable(const nm_ring *r);
size_t nm_ring_peek(const nm_ring *r, const int64_t **timestamps, const double **values);

/* Consumer side. Releases `count` rows previously reported by nm_ring_peek. */
void nm_ring_consume(nm_ring *r, size_t count);

#endif /* NM_RING_H */
