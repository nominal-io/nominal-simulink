/*
 * Skeleton. The API surface compiles and links so the three toolchains can be proven
 * before any logic exists — cross-compiling an empty library for QNX is a cheap way to
 * find out that the toolchain works.
 *
 * Implementation order, once Phase 0b settles which Content-Encodings the server
 * accepts: protobuf encoding (nanopb), then the ring buffer, then the sender thread,
 * then HTTP.
 */

#include "nominal_stream.h"

struct nm_stream {
    int placeholder;
};

nm_stream *nm_open(const nm_config *cfg, nm_status *status)
{
    if (cfg == NULL || cfg->base_url == NULL || cfg->token == NULL ||
        cfg->dataset_rid == NULL || cfg->channel_names == NULL ||
        cfg->channel_count == 0 || cfg->capacity_rows == 0) {
        if (status != NULL) {
            *status = NM_ERR_ARG;
        }
        return NULL;
    }

    /* Not yet implemented. Reported as a transport failure rather than success so a
     * caller wired up against this skeleton fails loudly instead of silently
     * discarding data. */
    if (status != NULL) {
        *status = NM_ERR_TRANSPORT;
    }
    return NULL;
}

int nm_push(nm_stream *s, int64_t timestamp_nanos, const double *values, size_t count)
{
    (void)timestamp_nanos;

    if (s == NULL || values == NULL || count == 0) {
        return NM_ERR_ARG;
    }
    return NM_ERR_CLOSED;
}

void nm_stats(const nm_stream *s, nm_stats_t *out)
{
    if (s == NULL || out == NULL) {
        return;
    }

    out->rows_accepted   = 0;
    out->rows_sent       = 0;
    out->rows_dropped    = 0;
    out->requests_ok     = 0;
    out->requests_failed = 0;
    out->last_http_status = 0;
}

void nm_close(nm_stream *s)
{
    (void)s;
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
