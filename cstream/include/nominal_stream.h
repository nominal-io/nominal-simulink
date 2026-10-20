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
 *   - nm_push() does no TLS, socket, or compression work.
 *
 * That means nm_push() has a bounded cost that does not depend on the network, which is
 * what makes it safe to call from a control loop. The cost of that guarantee is that
 * telemetry is lossy under sustained backpressure — which is the correct trade for a
 * hardware-in-the-loop rig, where stalling the model is far worse than dropping samples.
 * nm_stats() reports what was lost.
 *
 * Scope is deliberately narrow: this writes points to an existing dataset, and reads the
 * profile that says where and with which API key. Creating datasets, resolving assets,
 * and every other catalog operation belongs to the Rust client in the nominal-c
 * repository. A direct stream needs an API key, a dataset RID and a base URL. A proxy
 * stream needs a dataset RID and the relay's address. Opening either makes no API call.
 */

#ifndef NOMINAL_STREAM_H
#define NOMINAL_STREAM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes. Negative values are failures. nm_push also returns 1 when it discards
 * a row, so test for < 0 rather than != 0. */
typedef enum {
    NM_OK              =  0,
    NM_ERR_ARG         = -1,  /* a required field was missing or malformed */
    NM_ERR_ALLOC       = -2,  /* could not reserve the buffers at open time */
    NM_ERR_THREAD      = -3,  /* the sender thread would not start */
    NM_ERR_TRANSPORT   = -4,  /* the HTTPS client or the relay socket could not be set up */
    NM_ERR_CLOSED      = -5,  /* reserved, not returned by any function */
    NM_ERR_TOO_LARGE   = -6   /* PROXY: too many channels for one row to fit a datagram,
                               * or the RID, names and tags do not fit one HELLO datagram */
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
 * The choice affects only where the sender thread delivers rows. The ring, timestamps
 * and the Simulink block are the same either way.
 */
typedef enum {
    NM_MODE_DIRECT = 0,
    NM_MODE_PROXY  = 1
} nm_mode;

/* One tag, such as {"execution-id", "7"}. */
typedef struct {
    const char *key;
    const char *value;
} nm_tag;

typedef struct {
    nm_mode mode;

    /* DIRECT only. base_url is the REST API root, e.g. "https://api.gov.nominal.io/api".
     * token is a Nominal API key. */
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

    /* Tags on every channel this stream writes. They are what keep many runs of a model
     * apart in one dataset. Copied at open, like the names. Keys can't be empty. NULL
     * when tag_count is 0. In PROXY mode they travel in the HELLO frame. */
    const nm_tag *tags;
    size_t        tag_count;

    /* PROXY only. A run for the relay to create when the stream opens and to end when it
     * closes, as nm_run_begin and nm_run_end do in DIRECT mode. NULL or "" run_title asks
     * for none. The run starts at run_start_nanos and shows the dataset's points with the
     * first run_tag_count tags, under run_ref_name. run_shared asks for one run per title,
     * as nm_run_config's shared does. Copied at open. */
    const char *run_title;
    const char *run_ref_name;
    size_t      run_tag_count;
    int64_t     run_start_nanos;
    int         run_shared;

    /* Rows of capacity to reserve. One row is one timestamp plus one value per
     * channel. The buffer never grows past this: nm_push discards rows instead. Size
     * it for the longest network stall worth surviving — at 1 kHz, 60000 rows is a
     * minute. */
    size_t capacity_rows;

    /* The longest a row waits in the buffer before the sender sends it, when rows
     * arrive slowly. 0 means 1000. The sender sends sooner as rows arrive faster: in
     * DIRECT mode once a request's worth is waiting, or half the buffer if that is
     * less, and in PROXY mode once a datagram's worth is. So this bounds how far
     * behind the producer a slow stream is, and a fast one is sent as its rows arrive,
     * without waiting on the buffer to fill. */
    uint32_t max_batch_delay_ms;

    /* How long nm_close may spend sending what is still buffered. Rows not sent by
     * then are counted as dropped. 0 means 15000. Without the limit, every remaining
     * batch would get its full retries and timeouts, so stopping a model with Nominal
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
    uint64_t rows_sent;        /* DIRECT: acknowledged by the server.
                                * PROXY: handed to the socket, unacknowledged. */
    uint64_t rows_dropped;     /* discarded: buffer full, send failed, or still
                                * buffered when close_timeout_ms ran out */
    uint64_t requests_ok;      /* HTTP requests, or datagrams in PROXY mode */
    uint64_t requests_failed;

    /* The HTTP status of the most recent *failed* request: 0 if none has failed, or if
     * it failed without a response. A success does not overwrite it, so it still says
     * why rows were lost after later requests went through. */
    long     last_http_status;

    /* Why the last failure happened: a transport message, or the start of the
     * server's JSON error body, which includes an errorInstanceId for support. Empty
     * when nothing has failed. The counters show that rows are being lost, and this
     * shows why. */
    char last_error[256];
} nm_stats_t;

/*
 * Open a stream. Makes no API call. Reserves buffers, copies the channel names, and
 * starts the sender thread. In DIRECT mode this does no network I/O. In PROXY mode it
 * resolves proxy_host and sends the first HELLO.
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
 * close_timeout_ms has passed, so it must not be called from a real-time context. In
 * PROXY mode it then tells the relay the stream has ended, which takes another 100 ms.
 * Passing NULL is a no-op.
 */
void nm_close(nm_stream *s);

/*
 * Flush, report, and release.
 *
 * Same as nm_close, but fills *out after the final flush and before releasing the
 * stream. Exists because the counts a caller actually wants are the ones after the
 * final flush, and nm_close frees the stream straight after it, so there is otherwise
 * no moment at which the true totals can be read.
 *
 * With out == NULL this is nm_close. With s == NULL it zeroes *out.
 */
void nm_close_stats(nm_stream *s, nm_stats_t *out);

/* Human-readable text for a status code. Returns a static string; never NULL. */
const char *nm_strerror(nm_status status);

/*
 * A connection profile from Nominal's config file, the file the Python client, the nom
 * CLI and nominal-matlab also read, so a computer set up for one is set up for all.
 * Every field is NUL-terminated. workspace_rid is empty when the profile sets none.
 */
typedef struct {
    char base_url[512];
    char token[4096];
    char workspace_rid[256];
} nm_profile;

typedef enum {
    NM_PROFILE_OK = 0,
    NM_PROFILE_NO_FILE,       /* no file at the path, or it can't be opened */
    NM_PROFILE_NOT_FOUND,     /* the file has no profile by that name */
    NM_PROFILE_NO_TOKEN,      /* the profile has no token */
    NM_PROFILE_NO_BASE_URL,   /* the profile has no base_url */
    NM_PROFILE_TOO_LONG       /* a value is longer than nm_profile holds */
} nm_profile_status;

/*
 * Read the profile `name` from the config file at `path`. NULL or "" reads the default
 * file: ~/.config/nominal/config.yml, or %USERPROFILE%\.config\nominal\config.yml on
 * Windows. The path read is written to path_used, if it is non-NULL, so a caller can
 * name it in a message. Makes no network call.
 *
 * Reads the two-level shape the file has, `profiles:` then a name then its keys, and
 * nothing more of YAML: values may be quoted, `null` and `~` mean unset, and lines
 * starting with # are comments.
 */
nm_profile_status nm_profile_read(const char *path, const char *name, nm_profile *out,
                                  char *path_used, size_t path_used_len);

/*
 * Runs. A run is one test or simulation in Nominal: a title, a start and end time, and
 * the dataset it draws on, filtered to some tags. These two functions call the REST API
 * and block, so call them where blocking is fine, such as before a model starts and
 * after it stops, never from a real-time context. They are separate from nm_open so a
 * slow run request can't hold up the rows a stream is buffering.
 */
typedef struct {
    const char   *base_url;        /* as for nm_config */
    const char   *token;
    const char   *ca_bundle_path;  /* NULL for the platform default */
    const char   *workspace_rid;   /* NULL or "" for the organization's default */
    const char   *dataset_rid;     /* the dataset the run draws on */
    const char   *ref_name;        /* the name the dataset goes under, such as "default" */
    const char   *title;
    const nm_tag *tags;            /* the run shows only the dataset's points with these */
    size_t        tag_count;
    int64_t       start_nanos;
    int           shared;          /* non-zero: one run for many streams, see below */
} nm_run_config;

/*
 * Create the run and write its RID to rid. With shared set, first look for a run with
 * exactly this title, the most recently created if there are several, and use that one
 * instead. Blocks for up to about 15 s per request. On failure returns NM_ERR_TRANSPORT,
 * with the reason in err, or NM_ERR_ARG for a missing setting.
 */
nm_status nm_run_begin(const nm_run_config *cfg, char *rid, size_t rid_len,
                       char *err, size_t err_len);

/*
 * Set the run's end time, which has to be after its start. With shared set, this only
 * ever moves the end later, so each stream sharing a run extends it. Blocks for up to
 * about 15 s.
 */
nm_status nm_run_end(const nm_run_config *cfg, const char *rid, int64_t end_nanos,
                     char *err, size_t err_len);

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
