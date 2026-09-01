/*
 * nominal_stream.c — the public API, tying the ring, the encoder and the transport
 * together.
 *
 * The split of work across threads is the whole design:
 *
 *   caller's thread    nm_push -> nm_ring_push          bounded, no alloc, no block
 *   sender thread      ring -> nm_encode -> nm_http     everything slow lives here
 *
 * The sender encodes straight out of ring storage. The ring stores rows as
 * timestamp + one value per channel, row-major, which is exactly the layout
 * nm_encode_request reads, so a batch reaches the wire without an intermediate copy.
 */

#include "nominal_stream.h"

#include "nm_encode.h"
#include "nm_http.h"
#include "nm_ring.h"
#include "nm_thread.h"

#include <curl/curl.h>

#include <stdlib.h>
#include <string.h>

/* Bounds one request. Large enough that batching is effective, small enough that a
 * single failure loses a modest amount of data and a single encode stays quick. */
#define NM_MAX_ROWS_PER_REQUEST 4096

/* A malformed body reports as 500 here, so retrying cannot be persistent -- see
 * nm_http.h. Three attempts absorbs a transient fault without wedging the queue. */
#define NM_MAX_ATTEMPTS 3

struct nm_stream {
    nm_ring   ring;
    nm_http  *http;

    nm_channel_desc *channels;      /* channel_count entries */
    char            *name_pool;     /* one allocation holding every channel name */
    size_t           channel_count;

    uint8_t *encode_buf;
    size_t   encode_cap;

    nm_thread thread;
    int       thread_started;
    volatile int running;
    uint32_t  batch_delay_ms;

    /* Counters. Each is written by exactly one thread -- accepted and dropped-on-full
     * by the producer, the rest by the sender -- and read by nm_stats. Aligned 64-bit
     * reads are atomic on the targets we build for, so nm_stats needs no lock and
     * stays safe to call from the same real-time context as nm_push. */
    uint64_t rows_accepted;
    uint64_t rows_dropped;
    uint64_t rows_sent;
    uint64_t requests_ok;
    uint64_t requests_failed;
    long     last_status;

    /* Written by the sender, read by nm_stats. A concurrent read could in principle
     * catch this mid-update; it is diagnostic text, so a torn read is preferable to
     * putting a lock on the stats path. */
    char last_error[256];
};

/* libcurl wants one process-wide init. nm_open is not expected to race with itself;
 * if that changes this needs a call-once. */
static int g_curl_ready = 0;

/* ------------------------------------------------------------------ helpers */

/* Copy the caller's channel names into one block so their storage need not outlive
 * nm_open, and so the sender never chases a dangling pointer. Tags are not copied:
 * the v1 API takes none, and the encoder reads them only during nm_open. */
static int copy_channels(nm_stream *s, const char *const *names, size_t count)
{
    size_t total = 0, i;
    char  *cursor;

    for (i = 0; i < count; i++) {
        if (names[i] == NULL) {
            return -1;
        }
        total += strlen(names[i]) + 1;
    }

    s->channels  = calloc(count, sizeof *s->channels);
    s->name_pool = malloc(total);
    if (s->channels == NULL || s->name_pool == NULL) {
        return -1;
    }

    cursor = s->name_pool;
    for (i = 0; i < count; i++) {
        size_t n = strlen(names[i]) + 1;
        memcpy(cursor, names[i], n);
        s->channels[i].name      = cursor;
        s->channels[i].tags      = NULL;
        s->channels[i].tag_count = 0;
        cursor += n;
    }

    s->channel_count = count;
    return 0;
}

/*
 * Send whatever is currently readable. Called on the sender thread's schedule and
 * once more from nm_close.
 *
 * A wrapped ring yields its rows in two runs; each becomes its own request, which
 * costs an extra POST at the wrap and keeps the encoder reading contiguous memory.
 */
static void drain(nm_stream *s)
{
    for (;;) {
        const int64_t *timestamps;
        const double  *values;
        size_t rows, encoded;
        nm_http_result result;

        rows = nm_ring_peek(&s->ring, &timestamps, &values);
        if (rows == 0) {
            return;
        }
        if (rows > NM_MAX_ROWS_PER_REQUEST) {
            rows = NM_MAX_ROWS_PER_REQUEST;
        }

        /* Shrink until the batch fits the buffer we reserved at open. Values that
         * happen to be zero encode shorter, so the exact size is only knowable now;
         * halving converges in a few steps and cannot fail. */
        while (rows > 1 &&
               nm_encoded_size(s->channels, s->channel_count, timestamps, rows, values)
                   > s->encode_cap) {
            rows /= 2;
        }

        encoded = nm_encode_request(s->encode_buf, s->encode_cap,
                                    s->channels, s->channel_count,
                                    timestamps, rows, values);
        if (encoded == 0) {
            /* A single row that will not fit cannot be made smaller. Discard it
             * rather than spin on it forever. */
            s->rows_dropped += rows;
            nm_ring_consume(&s->ring, rows);
            continue;
        }

        nm_http_post(s->http, s->encode_buf, encoded, NM_MAX_ATTEMPTS, &result);
        s->last_status = result.status;

        if (result.ok) {
            s->requests_ok += 1;
            s->rows_sent   += rows;
        } else {
            memcpy(s->last_error, result.error, sizeof s->last_error);
            /* Retries are already exhausted inside nm_http_post. Holding the rows
             * would wedge the ring behind a request that may never succeed, so they
             * are counted as lost and released. */
            s->requests_failed += 1;
            s->rows_dropped    += rows;
        }

        nm_ring_consume(&s->ring, rows);
    }
}

static NM_THREAD_FN(sender_main, arg)
{
    nm_stream *s = arg;

    while (s->running) {
        nm_sleep_ms(s->batch_delay_ms);
        drain(s);
    }
    NM_THREAD_RETURN;
}

/* ------------------------------------------------------------------ public */

nm_stream *nm_open(const nm_config *cfg, nm_status *status)
{
    nm_stream *s;
    nm_status  st = NM_OK;
    size_t     worst_case;

    if (cfg == NULL || cfg->base_url == NULL || cfg->token == NULL ||
        cfg->dataset_rid == NULL || cfg->channel_names == NULL ||
        cfg->channel_count == 0 || cfg->capacity_rows == 0) {
        st = NM_ERR_ARG;
        goto done;
    }

    s = calloc(1, sizeof *s);
    if (s == NULL) {
        st = NM_ERR_ALLOC;
        goto done;
    }

    s->batch_delay_ms = cfg->max_batch_delay_ms > 0 ? cfg->max_batch_delay_ms : 100u;

    if (copy_channels(s, cfg->channel_names, cfg->channel_count) != 0) {
        st = NM_ERR_ALLOC;
        goto fail;
    }

    if (nm_ring_init(&s->ring, cfg->capacity_rows, cfg->channel_count) != 0) {
        st = NM_ERR_ALLOC;
        goto fail;
    }

    /* Reserve the encode buffer for the largest request we will build. Roughly 32
     * bytes per point covers tag, length, a full-width timestamp and a fixed64 value;
     * 256 per channel covers the name, and the nested length prefixes around it. */
    {
        size_t rows = cfg->capacity_rows < NM_MAX_ROWS_PER_REQUEST
                    ? cfg->capacity_rows : NM_MAX_ROWS_PER_REQUEST;
        worst_case = cfg->channel_count * 256u + rows * cfg->channel_count * 32u;
    }
    s->encode_buf = malloc(worst_case);
    if (s->encode_buf == NULL) {
        st = NM_ERR_ALLOC;
        goto fail;
    }
    s->encode_cap = worst_case;

    if (!g_curl_ready) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            st = NM_ERR_TRANSPORT;
            goto fail;
        }
        g_curl_ready = 1;
    }

    s->http = nm_http_open(cfg->base_url, cfg->token, cfg->dataset_rid,
                           cfg->ca_bundle_path);
    if (s->http == NULL) {
        st = NM_ERR_TRANSPORT;
        goto fail;
    }

    s->running = 1;
    if (nm_thread_start(&s->thread, sender_main, s) != 0) {
        s->running = 0;
        st = NM_ERR_THREAD;
        goto fail;
    }
    s->thread_started = 1;

    if (status != NULL) {
        *status = NM_OK;
    }
    return s;

fail:
    nm_close(s);
done:
    if (status != NULL) {
        *status = st;
    }
    return NULL;
}

int nm_push(nm_stream *s, int64_t timestamp_nanos, const double *values, size_t count)
{
    if (s == NULL || values == NULL || count == 0) {
        return NM_ERR_ARG;
    }
    if (count != s->channel_count) {
        return NM_ERR_ARG;
    }

    if (nm_ring_push(&s->ring, timestamp_nanos, values)) {
        s->rows_accepted += 1;
        return 0;
    }

    s->rows_dropped += 1;
    return 1;
}

void nm_stats(const nm_stream *s, nm_stats_t *out)
{
    if (s == NULL || out == NULL) {
        return;
    }
    out->rows_accepted    = s->rows_accepted;
    out->rows_sent        = s->rows_sent;
    out->rows_dropped     = s->rows_dropped;
    out->requests_ok      = s->requests_ok;
    out->requests_failed  = s->requests_failed;
    out->last_http_status = s->last_status;
    memcpy(out->last_error, s->last_error, sizeof out->last_error);
}

void nm_close(nm_stream *s)
{
    if (s == NULL) {
        return;
    }

    if (s->thread_started) {
        s->running = 0;
        nm_thread_join(s->thread);
        s->thread_started = 0;
    }

    /* Whatever the sender did not reach. Safe now that it has stopped. */
    if (s->http != NULL) {
        drain(s);
        nm_http_close(s->http);
    }

    nm_ring_free(&s->ring);
    free(s->encode_buf);
    free(s->channels);
    free(s->name_pool);
    free(s);
}

int64_t nm_now_nanos(void)
{
#if defined(_WIN32)
    /* FILETIME counts 100 ns ticks from 1601-01-01; this is the offset to 1970. */
    static const int64_t EPOCH_DELTA_100NS = 116444736000000000LL;
    FILETIME       ft;
    ULARGE_INTEGER u;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return ((int64_t)u.QuadPart - EPOCH_DELTA_100NS) * 100;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
#endif
}

const char *nm_strerror(nm_status status)
{
    switch (status) {
    case NM_OK:            return "ok";
    case NM_ERR_ARG:       return "invalid argument";
    case NM_ERR_ALLOC:     return "could not reserve buffers";
    case NM_ERR_THREAD:    return "could not start sender thread";
    case NM_ERR_TRANSPORT: return "transport not initialised";
    case NM_ERR_CLOSED:    return "stream is closed";
    }
    return "unknown status";
}
