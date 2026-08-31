/*
 * nm_encode.h — protobuf encoding for WriteRequestNominal.
 *
 * Hand-written rather than generated. The schema is six messages of scalar fields
 * (proto/nominal_write.proto), and the repeated fields are streaming-sized, which would
 * push a generator like nanopb into callback-based encoding. Encoding straight into a
 * caller-owned buffer is what lets nm_push() promise never to allocate, so we keep
 * direct control of the bytes.
 *
 * Correctness is pinned by tests/fixtures/body_identity.pb — a byte-exact capture of
 * what the Rust client produces for a known input. That fixture, regenerated against the
 * current nominal-streaming, is the real guard against schema drift; generated code
 * would not have caught the change that actually happened (a content-encoding switch).
 *
 * Encoding is two-pass: measure, then write. Protobuf length-prefixes every nested
 * message, so the alternative is writing backwards or memmoving, and for a schema this
 * small the sizing pass is cheap and much easier to read.
 */

#ifndef NM_ENCODE_H
#define NM_ENCODE_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    const char *key;
    const char *value;
} nm_tag;

typedef struct {
    const char   *name;   /* channel name, as it will appear in Nominal */
    const nm_tag *tags;   /* may be NULL when tag_count is 0 */
    size_t        tag_count;
} nm_channel_desc;

/*
 * Measure the encoding of one WriteRequestNominal without writing it.
 *
 * `values` is row-major, `row_count` rows of `channel_count` values — the shape a
 * Simulink bus produces naturally, one sample per channel per timestamp. All channels
 * share the `timestamps` column.
 *
 * Returns the exact byte count nm_encode_request would write.
 */
size_t nm_encoded_size(const nm_channel_desc *channels, size_t channel_count,
                       const int64_t *timestamps, size_t row_count,
                       const double *values);

/*
 * Encode into `buf`. Returns bytes written, or 0 if the buffer was too small —
 * call nm_encoded_size first, or size the buffer at open time and check.
 *
 * Never allocates.
 */
size_t nm_encode_request(uint8_t *buf, size_t buf_len,
                         const nm_channel_desc *channels, size_t channel_count,
                         const int64_t *timestamps, size_t row_count,
                         const double *values);

/* Split a Unix nanosecond count into protobuf Timestamp fields. Exposed for tests.
 * Floors toward negative infinity so nanos is always in [0, 1e9), which is what
 * google.protobuf.Timestamp requires for instants before the epoch. */
void nm_split_timestamp(int64_t nanos_since_epoch, int64_t *out_seconds, int32_t *out_nanos);

#endif /* NM_ENCODE_H */
