/*
 * nominal_stream.h — a write-only Nominal streaming client for real-time targets.
 *
 * This library exists because a Simulink model's step function is not a place to do
 * network I/O. It splits the work in two: a producer side that a real-time task can
 * call safely, and a sender thread that owns everything slow.
 *
 * The producer contract, which is the whole point:
 *
 *   - Every allocation happens in nm_open(). nm_push() never calls malloc.
 *   - nm_push() never blocks. If there is no room it discards the sample and says so.
 *   - No TLS, socket, or compression work happens on the calling thread.
 *
 * That means nm_push() has a bounded cost that does not depend on the network, which is
 * what makes it safe to call from a control loop. The cost of that guarantee is that
 * telemetry is lossy under sustained backpressure — which is the correct trade for a
 * hardware-in-the-loop rig, where stalling the model is far worse than dropping samples.
 * nm_stats() reports what was lost.
 *
 * Scope is deliberately narrow: this writes points to an existing dataset and does
 * nothing else. Creating datasets, resolving assets, and every other catalog operation
 * belongs to the Rust client in the nominal-c repository. A stream needs only a token,
 * a dataset RID, and a base URL — no API call is required to open one.
 */

#ifndef NOMINAL_STREAM_H
#define NOMINAL_STREAM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes. Negative values are failures; nm_push additionally returns a positive
 * count of discarded samples, so callers should test for < 0 rather than != 0. */
typedef enum {
    NM_OK              =  0,
    NM_ERR_ARG         = -1,  /* a required field was missing or malformed */
    NM_ERR_ALLOC       = -2,  /* could not reserve the buffers at open time */
    NM_ERR_THREAD      = -3,  /* the sender thread would not start */
    NM_ERR_TRANSPORT   = -4,  /* the HTTP client could not be initialised */
    NM_ERR_CLOSED      = -5,  /* used after nm_close */
    NM_ERR_TOO_LARGE   = -6   /* PROXY: the channel list does not fit one datagram */
} nm_status;

typedef struct nm_stream nm_stream;

/*
 * How the data leaves this machine.
 *
 * DIRECT is the default: this process talks HTTPS to Nominal itself. Simple to deploy,
 * nothing else to run — but the sender thread pays for protobuf, TLS and HTTP here.
 *
 * PROXY sends compact datagrams to a relay elsewhere, which does that work instead.
 * The host is left doing little more than a sendto, which is what makes telemetry
 * affordable on a machine that is already at its performance limit. It also means no
 * credential lives on this machine: the dataset RID travels, the token does not.
 *
 * The choice affects only the sink. The ring, the sender thread, timestamps and the
 * Simulink block are identical either way.
 */
typedef enum {
    NM_MODE_DIRECT = 0,
    NM_MODE_PROXY  = 1
} nm_mode;

typedef struct {
    nm_mode mode;

    /* DIRECT only. base_url is the API root, e.g. "https://api.gov.nominal.io/api". */
    const char *base_url;
    const char *token;

    /* PROXY only. An address and port the relay is listening on. Prefer a private
     * segment: frames are not authenticated, so anything that can reach this port can
     * write to the datasets the relay serves. */
    const char *proxy_host;
    uint16_t    proxy_port;

    /* Both modes. In PROXY mode this travels in the HELLO frame so the relay knows
     * where to write, which is why the host needs no token. */
    const char *dataset_rid;

    /* The channels this stream writes, in the order values are supplied to nm_push.
     * Names are copied at open, so the caller's storage need not outlive this call.
     * Fixing the set up front is what lets nm_push address channels by index and do
     * no string work on the hot path. */
    const char *const *channel_names;
    size_t             channel_count;

    /* Rows of capacity to reserve. One row is one timestamp plus one value per
     * channel. This bounds the library's memory exactly: it will never allocate
     * beyond this, and will discard samples rather than grow. Size it for the
     * longest network stall worth surviving — at 1 kHz, 60000 rows is a minute. */
    size_t capacity_rows;

    /* How long the sender may wait for more rows before sending a partial batch.
     * Lower means fresher data and more requests; 100 ms matches the Rust client. */
    uint32_t max_batch_delay_ms;

    /* How long nm_close may spend sending what is still buffered. Rows not sent by
     * then are counted as dropped. 0 means 15000. Without a limit, every remaining
     * batch got its full retries and timeouts, so stopping a model with Nominal
     * unreachable could take many minutes. */
    uint32_t close_timeout_ms;

    /* Path to a CA bundle for TLS verification. Supplying one explicitly, rather
     * than relying on a system trust store, is deliberate: it is what keeps this
     * portable to targets that have no such store. NULL uses the platform default,
     * which is fine on desktop and generally not on an RTOS. */
    const char *ca_bundle_path;
} nm_config;

typedef struct {
    uint64_t rows_pushed;      /* handed to nm_push, whether or not there was room */
    uint64_t rows_accepted;    /* taken into the buffer by nm_push */
    uint64_t rows_sent;        /* acknowledged by the server */
    uint64_t rows_dropped;     /* discarded: buffer full, or the request failed */
    uint64_t requests_ok;
    uint64_t requests_failed;

    /* The HTTP status of the most recent *failed* request: 0 if none has failed, or if
     * it failed without a response. A success does not overwrite it, so it still says
     * why rows were lost after later requests went through. */
    long     last_http_status;

    /* Why the last failure happened — a transport message, or the server's error
     * body, which is a conjure envelope carrying an errorInstanceId. Empty when
     * nothing has failed. Without this a caller has no way to tell a working stream
     * from one silently discarding everything, since both keep running. */
    char last_error[256];
} nm_stats_t;

/*
 * Open a stream. Performs no network I/O and makes no API call — it reserves buffers,
 * copies the channel names, and starts the sender thread.
 *
 * Returns NULL on failure, writing the reason to *status if status is non-NULL.
 */
nm_stream *nm_open(const nm_config *cfg, nm_status *status);

/*
 * Write one row: a timestamp and one value per channel, in the order the channel names
 * were given to nm_open. `count` must equal channel_count.
 *
 * Timestamps are nanoseconds since the Unix epoch and are taken literally — zero is a
 * real timestamp here, not a request for the current time.
 *
 * Returns 0 when the row was accepted, or 1 when it was discarded because the buffer
 * was full. Returns a negative nm_status on misuse. It never blocks and never
 * allocates, so it is safe to call from a real-time context.
 */
int nm_push(nm_stream *s, int64_t timestamp_nanos, const double *values, size_t count);

/*
 * Copy out the current counters. Lock-free, but it copies the whole struct including
 * last_error, so prefer nm_dropped_rows on a per-step path — see below.
 */
void nm_stats(const nm_stream *s, nm_stats_t *out);

/*
 * Just the discarded-row count.
 *
 * Exists because a Simulink block wants this every step to drive an output port, and
 * nm_stats would memcpy a 256-byte error string each time to deliver one number. At
 * 1 kHz that is a quarter of a megabyte per second of pointless copying on precisely
 * the path this library promises to keep cheap.
 */
uint64_t nm_dropped_rows(const nm_stream *s);

/*
 * Flush what is buffered, stop the sender thread, and release everything.
 *
 * This blocks until the outstanding rows have been sent or have failed, or until
 * close_timeout_ms has passed, so it must not be called from a real-time context.
 * Passing NULL is a no-op.
 */
void nm_close(nm_stream *s);

/*
 * Flush, report, and release.
 *
 * Identical to nm_close but fills *out first. Exists because the counts a caller
 * actually wants are the ones after the final flush, and by then nm_close has freed
 * the stream — so there is otherwise no moment at which the true totals can be read.
 * Passing NULL is the same as nm_close.
 */
void nm_close_stats(nm_stream *s, nm_stats_t *out);

/* Human-readable text for a status code. Returns a static string; never NULL. */
const char *nm_strerror(nm_status status);

/*
 * Current wall-clock time as nanoseconds since the Unix epoch.
 *
 * A convenience, not something the library uses: nm_push always takes the timestamp
 * from the caller, because a model may be stamping simulation time or a
 * PTP-disciplined target clock rather than host wall time. Resolution follows the
 * platform and is typically coarser than a nanosecond.
 */
int64_t nm_now_nanos(void);

#ifdef __cplusplus
}
#endif

#endif /* NOMINAL_STREAM_H */
