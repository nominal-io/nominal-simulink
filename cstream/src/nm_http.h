/*
 * nm_http.h — the POST that writes a batch to Nominal.
 *
 * One libcurl easy handle is created at open and reused for every request, so the TLS
 * handshake and TCP connection are paid for once rather than per batch.
 *
 * Retry policy is shaped by two things we measured against the live server:
 *
 *   - A malformed request comes back as 500, not 4xx. So a permanently broken request
 *     is indistinguishable from a transient fault, and unbounded retry on 5xx would
 *     wedge the queue behind something that can never succeed. Attempts are capped.
 *   - Repeated identical writes collapse on (channel, timestamp). So retrying after an
 *     ambiguous timeout is safe and needs no deduplication tracking.
 */

#ifndef NM_HTTP_H
#define NM_HTTP_H

#include <stdint.h>
#include <stddef.h>

typedef struct nm_http nm_http;

typedef struct {
    long  status;          /* HTTP status, or 0 if no response was received */
    int   ok;              /* non-zero when the batch was accepted */
    char  error[256];      /* transport error or server error body, NUL-terminated */
} nm_http_result;

/*
 * Build the request URL and headers once. `base_url` is the API root,
 * `dataset_rid` an existing dataset, `ca_bundle_path` may be NULL to use the
 * platform default. Returns NULL on failure.
 *
 * `deadline` points at a wall-clock instant in nanoseconds, 0 for none, which another
 * thread may set at any time. Once it has passed, a request in flight is abandoned and
 * no further attempts are made. It is how nm_close bounds the final flush.
 */
nm_http *nm_http_open(const char *base_url, const char *token,
                      const char *dataset_rid, const char *ca_bundle_path,
                      const volatile int64_t *deadline);

/*
 * POST one encoded body, making up to `max_attempts` attempts in all. A transport
 * failure or 5xx is retried, except a certificate failure, and no attempt starts after
 * the deadline has passed. Blocks. Only ever called from the sender thread.
 */
void nm_http_post(nm_http *h, const uint8_t *body, size_t len,
                  int max_attempts, nm_http_result *out);

void nm_http_close(nm_http *h);

#endif /* NM_HTTP_H */
