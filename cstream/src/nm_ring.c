#include "nm_ring.h"
#include "nm_atomic.h"

#include <stdlib.h>
#include <string.h>

int nm_ring_init(nm_ring *r, size_t capacity_rows, size_t channel_count)
{
    if (r == NULL || capacity_rows == 0 || channel_count == 0) {
        return -1;
    }

    memset(r, 0, sizeof *r);

    r->timestamps = calloc(capacity_rows, sizeof *r->timestamps);
    r->values     = calloc(capacity_rows * channel_count, sizeof *r->values);
    if (r->timestamps == NULL || r->values == NULL) {
        nm_ring_free(r);
        return -1;
    }

    r->capacity      = capacity_rows;
    r->channel_count = channel_count;
    r->head          = 0;
    r->tail          = 0;
    return 0;
}

void nm_ring_free(nm_ring *r)
{
    if (r == NULL) {
        return;
    }
    free(r->timestamps);
    free(r->values);
    r->timestamps = NULL;
    r->values     = NULL;
    r->capacity   = 0;
}

int nm_ring_push(nm_ring *r, int64_t timestamp_nanos, const double *values)
{
    size_t head, tail, slot;

    /* head is ours alone, so it needs no synchronisation to read. The acquire load on
     * tail pairs with the consumer's release in nm_ring_consume, so the consumer has
     * finished reading a slot before we overwrite it. */
    head = r->head;
    tail = NM_LOAD_ACQUIRE(&r->tail);

    if (head - tail >= r->capacity) {
        return 0;   /* full: discard rather than block */
    }

    slot = head % r->capacity;
    r->timestamps[slot] = timestamp_nanos;
    memcpy(&r->values[slot * r->channel_count], values,
           r->channel_count * sizeof *values);

    /* Release: the row above must be visible before the consumer can see the index
     * that admits it. */
    NM_STORE_RELEASE(&r->head, head + 1);
    return 1;
}

size_t nm_ring_readable(const nm_ring *r)
{
    size_t head = NM_LOAD_ACQUIRE(&r->head);
    return head - r->tail;
}

size_t nm_ring_peek(const nm_ring *r, const int64_t **timestamps, const double **values)
{
    size_t head = NM_LOAD_ACQUIRE(&r->head);
    size_t tail = r->tail;
    size_t available = head - tail;
    size_t offset, to_end;

    if (available == 0) {
        return 0;
    }

    offset = tail % r->capacity;
    to_end = r->capacity - offset;
    if (available > to_end) {
        available = to_end;   /* stop at the wrap; caller comes back for the rest */
    }

    *timestamps = &r->timestamps[offset];
    *values     = &r->values[offset * r->channel_count];
    return available;
}

void nm_ring_consume(nm_ring *r, size_t count)
{
    /* Release so the producer cannot reuse these slots until we are done reading. */
    NM_STORE_RELEASE(&r->tail, r->tail + count);
}
