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
#include "nm_frame.h"
#include "nm_http.h"
#include "nm_ring.h"
#include "nm_thread.h"
#include "nm_udp.h"

#include <curl/curl.h>

#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <time.h>   /* clock_gettime; arrives transitively via nm_thread.h, but
                     * relying on another header's includes is how builds break */
#endif

/* Bounds one request. Large enough that batching is effective, small enough that a
 * single failure loses a modest amount of data and a single encode stays quick. */
#define NM_MAX_ROWS_PER_REQUEST 4096

/* A malformed body reports as 500 here, so retrying cannot be persistent -- see
 * nm_http.h. Three attempts absorbs a transient fault without wedging the queue. */
#define NM_MAX_ATTEMPTS 3

/* How often proxy mode repeats HELLO. UDP is lossy and a relay may restart mid-run,
 * so the channel mapping is re-announced rather than sent once; this bounds how long a
 * restarted relay discards data. One second costs a few dozen bytes. */
#define NM_HELLO_INTERVAL_MS 1000

struct nm_stream {
    nm_mode   mode;
    nm_ring   ring;

    /* DIRECT */
    nm_http  *http;

    /* PROXY */
    nm_udp   *udp;
    uint64_t  session_id;
    uint64_t  config_id;
    uint64_t  sequence;
    uint32_t  ms_since_hello;
    char     *dataset_rid;   /* kept, because HELLO re-sends it */

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

/* ------------------------------------------------------------- proxy mode */

/* Re-announce the channel mapping. Losing this to a dropped datagram is expected and
 * harmless, which is why it repeats rather than being checked. */
static void send_hello(nm_stream *s)
{
    uint8_t      buf[NM_FRAME_MAX_BYTES];
    const char  *names[NM_FRAME_MAX_CHANNELS];
    size_t       i, n;

    if (s->channel_count > NM_FRAME_MAX_CHANNELS) {
        return;
    }
    for (i = 0; i < s->channel_count; i++) {
        names[i] = s->channels[i].name;
    }

    n = nm_frame_encode_hello(buf, sizeof buf, s->session_id, s->config_id,
                              s->dataset_rid, names, s->channel_count);
    if (n > 0) {
        nm_udp_send(s->udp, buf, n);
    }
    s->ms_since_hello = 0;
}

/*
 * Drain the ring into datagrams.
 *
 * Rows are packed until the MTU is reached, so the syscall count falls as the channel
 * count does rather than being one per row. A send failure — a full socket buffer, or
 * the relay's port closed — costs those rows: retransmission would need buffering and
 * acknowledgements, which is precisely the expense proxy mode exists to avoid.
 */
static void drain_proxy(nm_stream *s)
{
    for (;;) {
        const int64_t *timestamps;
        const double  *values;
        uint8_t        buf[NM_FRAME_MAX_BYTES];
        size_t         rows, sent_rows = 0, n;

        rows = nm_ring_peek(&s->ring, &timestamps, &values);
        if (rows == 0) {
            return;
        }

        n = nm_frame_encode_data(buf, sizeof buf, s->session_id, s->config_id,
                                 s->sequence, timestamps, rows,
                                 values, s->channel_count, &sent_rows);
        if (n == 0 || sent_rows == 0) {
            /* Only reachable if a single row cannot fit a datagram, which means the
             * channel count is too high for proxy mode. Drop rather than spin. */
            s->rows_dropped += rows;
            nm_ring_consume(&s->ring, rows);
            continue;
        }

        if (nm_udp_send(s->udp, buf, n) == 0) {
            s->requests_ok += 1;
            s->rows_sent   += sent_rows;
        } else {
            s->requests_failed += 1;
            s->rows_dropped    += sent_rows;
            snprintf(s->last_error, sizeof s->last_error,
                     "could not send a datagram to the proxy");
        }

        /* Advance regardless: the sequence numbers a relay sees are what let it
         * detect the gap, so a failed send must still consume one. */
        s->sequence += 1;
        nm_ring_consume(&s->ring, sent_rows);
    }
}

/* ------------------------------------------------------------ direct mode */

/*
 * Send whatever is currently readable. Called on the sender thread's schedule and
 * once more from nm_close.
 *
 * A wrapped ring yields its rows in two runs; each becomes its own request, which
 * costs an extra POST at the wrap and keeps the encoder reading contiguous memory.
 */
static void drain_direct(nm_stream *s)
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

static void drain(nm_stream *s)
{
    if (s->mode == NM_MODE_PROXY) {
        drain_proxy(s);
    } else {
        drain_direct(s);
    }
}

static NM_THREAD_FN(sender_main, arg)
{
    nm_stream *s = arg;

    while (s->running) {
        nm_sleep_ms(s->batch_delay_ms);

        if (s->mode == NM_MODE_PROXY) {
            s->ms_since_hello += s->batch_delay_ms;
            if (s->ms_since_hello >= NM_HELLO_INTERVAL_MS) {
                send_hello(s);
            }
        }

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

    /* Common requirements. The mode then adds its own: DIRECT needs somewhere to POST
     * and a credential, PROXY needs a relay to send to and neither of those. */
    if (cfg == NULL || cfg->dataset_rid == NULL || cfg->channel_names == NULL ||
        cfg->channel_count == 0 || cfg->capacity_rows == 0) {
        st = NM_ERR_ARG;
        goto done;
    }
    if (cfg->mode == NM_MODE_PROXY) {
        if (cfg->proxy_host == NULL || cfg->proxy_host[0] == '\0' ||
            cfg->proxy_port == 0) {
            st = NM_ERR_ARG;
            goto done;
        }
        /* One row must fit a datagram, and that depends only on the channel count.
         * Failing here is far kinder than discovering it per-batch at runtime. */
        if (nm_frame_rows_per_datagram(cfg->channel_count) == 0) {
            st = NM_ERR_ARG;
            goto done;
        }
    } else if (cfg->token == NULL || cfg->token[0] == '\0') {
        st = NM_ERR_ARG;
        goto done;
    }

    s = calloc(1, sizeof *s);
    if (s == NULL) {
        st = NM_ERR_ALLOC;
        goto done;
    }

    s->mode           = cfg->mode;
    s->batch_delay_ms = cfg->max_batch_delay_ms > 0 ? cfg->max_batch_delay_ms : 100u;

    if (copy_channels(s, cfg->channel_names, cfg->channel_count) != 0) {
        st = NM_ERR_ALLOC;
        goto fail;
    }

    if (nm_ring_init(&s->ring, cfg->capacity_rows, cfg->channel_count) != 0) {
        st = NM_ERR_ALLOC;
        goto fail;
    }

    if (cfg->mode == NM_MODE_PROXY) {
        char err[192];

        /* No protobuf in proxy mode, so no encode buffer: frames are built on the
         * sender thread's stack, bounded by the MTU. */
        s->dataset_rid = malloc(strlen(cfg->dataset_rid) + 1);
        if (s->dataset_rid == NULL) {
            st = NM_ERR_ALLOC;
            goto fail;
        }
        strcpy(s->dataset_rid, cfg->dataset_rid);

        s->config_id = nm_frame_config_id(cfg->dataset_rid, cfg->channel_names,
                                          cfg->channel_count);
        /* Distinguishes concurrent models writing to one relay. Derived from the
         * config and the clock rather than a PRNG, which keeps this dependency-free
         * and is sufficient: it only has to be distinct, not unpredictable. */
        s->session_id = s->config_id ^ (uint64_t)nm_now_nanos();

        s->udp = nm_udp_open(cfg->proxy_host, cfg->proxy_port, err, sizeof err);
        if (s->udp == NULL) {
            memcpy(s->last_error, err, sizeof s->last_error > sizeof err
                                       ? sizeof err : sizeof s->last_error);
            st = NM_ERR_TRANSPORT;
            goto fail;
        }
    } else {
        /* Reserve the encode buffer for the largest request we will build. Roughly 32
         * bytes per point covers tag, length, a full-width timestamp and a fixed64
         * value; 256 per channel covers the name and the nested length prefixes. */
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
    }

    s->running = 1;
    if (nm_thread_start(&s->thread, sender_main, s) != 0) {
        s->running = 0;
        st = NM_ERR_THREAD;
        goto fail;
    }
    s->thread_started = 1;

    /* Announce the mapping immediately rather than waiting a full interval, so a relay
     * that is already listening can accept the first batch. */
    if (cfg->mode == NM_MODE_PROXY) {
        send_hello(s);
    }

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

uint64_t nm_dropped_rows(const nm_stream *s)
{
    return s != NULL ? s->rows_dropped : 0;
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
    if (s->http != NULL || s->udp != NULL) {
        drain(s);
    }
    if (s->http != NULL) {
        nm_http_close(s->http);
    }
    if (s->udp != NULL) {
        nm_udp_close(s->udp);
    }

    nm_ring_free(&s->ring);
    free(s->encode_buf);
    free(s->channels);
    free(s->name_pool);
    free(s->dataset_rid);
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
