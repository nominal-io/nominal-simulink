/*
 * nm_frame.h — the host-to-proxy wire format.
 *
 * Proxy mode exists because some hosts have no performance headroom left. Direct mode
 * is cheap on the step path but the sender thread still pays for protobuf, TLS and HTTP
 * on the same machine. Proxy mode moves all of that to another box, leaving the host
 * doing little more than a sendto.
 *
 * Pure encode/decode, no sockets, no allocation. Kept in its own translation unit so a
 * static link into a real-time application drops the decoder as dead code — a
 * Speedgoat target never receives frames, only sends them.
 *
 *
 * DESIGN
 *
 * Automatic channel configuration is the property worth protecting, so the format is
 * built around it. Two frame types:
 *
 *   HELLO  session, config_id, dataset RID, ordered channel names.
 *   DATA   session, config_id, sequence, rows of (timestamp, values[]).
 *
 * DATA references channels by index into the HELLO list, never by name, so a row costs
 * 8 bytes per channel and the sender does no string work at all.
 *
 * HELLO is sent when the stream opens AND repeated periodically. That is not redundancy
 * for its own sake: UDP is lossy, and a proxy restarting mid-run would otherwise have no
 * way to recover the mapping. There is no back-channel to ask over, so periodic HELLO is
 * what makes the system self-healing.
 *
 * config_id is a hash of the channel list plus the RID. A proxy seeing a DATA frame with
 * an unknown config_id discards it and waits for the next HELLO — bounded loss after a
 * restart. It also makes a *changed* channel list unambiguous rather than silently
 * misaligning columns, which is the failure mode that would otherwise produce
 * plausible-looking wrong data.
 *
 * Everything is little-endian. Encoders must write LE explicitly rather than memcpy a struct.
 *
 * Frames carry no authentication. Anyone who can reach the proxy's port can write to
 * any dataset it serves. 
 */

#ifndef NM_FRAME_H
#define NM_FRAME_H

#include <stdint.h>
#include <stddef.h>

#define NM_FRAME_MAGIC   0x4E4D5331u  /* "NMS1", checked before anything is trusted */
#define NM_FRAME_VERSION 1

/* Datagrams stay under this so IP never fragments them. Fragmentation would turn one
 * lost fragment into a lost frame, which is a worse failure mode than sending more
 * datagrams. */
#define NM_FRAME_MAX_BYTES 1400

/* Size of the common header, laid out in full further down. Defined here because the
 * channel bound is derived from it. */
#define NM_FRAME_HEADER_BYTES 24

/* Bytes of DATA body before the rows begin: sequence(8) + channel_count(2) +
 * row_count(2). In the header rather than the implementation so the channel bound
 * below is derivable from the spec alone. */
#define NM_FRAME_DATA_FIXED_BYTES 12

/*
 * The most channels a DATA frame can carry, derived rather than chosen so it cannot
 * drift from the MTU. One row is 8 bytes of timestamp plus 8 per channel, and it has
 * to fit alongside the header and the fixed DATA fields:
 *
 *   8*nch <= NM_FRAME_MAX_BYTES - HEADER - DATA_FIXED - 8   ->  nch <= 169
 *
 * A third-party decoder can size its buffers from this and never see a larger frame.
 *
 * HELLO's practical limit is lower and depends on name lengths, so it is enforced
 * dynamically by nm_frame_encode_hello rather than by a constant: ~110 channels with
 * ten-character names. Both encoders refuse rather than truncate.
 */
#define NM_FRAME_MAX_CHANNELS \
    (((NM_FRAME_MAX_BYTES - NM_FRAME_HEADER_BYTES - NM_FRAME_DATA_FIXED_BYTES - 8u) / 8u))

/* Per-field sanity limits. The frame size bounds the totals; these only stop a single
 * absurd name or RID. */
#define NM_FRAME_MAX_NAME_BYTES 256
#define NM_FRAME_MAX_RID_BYTES  256

typedef enum {
    NM_FRAME_HELLO = 1,
    NM_FRAME_DATA  = 2
} nm_frame_type;

/*
 * Common header, 24 bytes, identical in both frame types so a decoder can dispatch
 * before knowing which it holds.
 *
 *   offset size field
 *   0      4    magic       NM_FRAME_MAGIC
 *   4      1    version     NM_FRAME_VERSION
 *   5      1    type        nm_frame_type
 *   6      2    reserved    zero
 *   8      8    session_id  random per stream; distinguishes concurrent models
 *   16     8    config_id   hash of the channel list and RID
 */

typedef struct {
    uint8_t  version;
    uint8_t  type;
    uint64_t session_id;
    uint64_t config_id;
} nm_frame_header;

/*
 * GRAMMAR
 *
 * In ABNF (RFC 5234), which unlike plain BNF can express fixed octet counts. `nOCTET`
 * means exactly n octets; `%xNN` is a literal octet.
 *
 *   frame         = hello-frame / data-frame
 *
 *   hello-frame   = magic version %x01 reserved session-id config-id hello-body
 *   data-frame    = magic version %x02 reserved session-id config-id data-body
 *
 *   magic         = %x31.53.4D.4E   ; NM_FRAME_MAGIC as a little-endian uint32
 *   version       = OCTET           ; NM_FRAME_VERSION; a decoder rejects others
 *   reserved      = 2OCTET          ; zero
 *   session-id    = u64             ; distinguishes concurrent models
 *   config-id     = u64             ; hash of the RID and channel names
 *
 *   hello-body    = rid channel-count *channel-name
 *   rid           = length *OCTET   ; not NUL-terminated
 *   channel-name  = length *OCTET   ; not NUL-terminated
 *
 *   data-body     = sequence channel-count row-count *row
 *   sequence      = u64             ; per-session, monotonic; gaps are loss
 *   row-count     = u16
 *   row           = timestamp *value
 *   timestamp     = i64             ; nanoseconds, Unix epoch
 *   value         = f64
 *
 *   channel-count = u16
 *   length        = u16
 *   u16           = 2OCTET          ; little-endian
 *   u64           = 8OCTET          ; little-endian
 *   i64           = 8OCTET          ; little-endian, two's complement
 *   f64           = 8OCTET          ; little-endian IEEE 754 binary64
 *
 * CONSTRAINTS NOT EXPRESSIBLE IN THE GRAMMAR
 *
 * The `*` repetitions above are unbounded, but each is really governed by a field
 * earlier in the same frame. That is a context-sensitive dependency, so no context-free
 * grammar can state it:
 *
 *   1. hello-body contains exactly channel-count occurrences of channel-name.
 *   2. data-body contains exactly row-count occurrences of row.
 *   3. each row contains exactly channel-count occurrences of value.
 *   4. channel-count in a data-frame must equal the channel-count of the hello-frame
 *      bearing the same config-id.
 *   5. no frame exceeds NM_FRAME_MAX_BYTES, and channel-count <= NM_FRAME_MAX_CHANNELS.
 *
 * This is precisely why a decoder must check every length against the bytes actually
 * received rather than trusting the grammar: a frame can be perfectly well-formed
 * against the productions above and still claim more rows than it carries. Those length
 * fields are the obvious way to walk a decoder off the end of a buffer, which is what
 * the truncation tests in tests/test_frame.c exist to prevent.
 *
 * A channel list too large for one datagram is rejected at open rather than split
 * across frames — a partial channel map is a subtle source of misaligned data, and
 * 1400 bytes holds a lot of names.
 */

/* Rows that fit one datagram, given a channel count. Zero if a single row cannot. */
size_t nm_frame_rows_per_datagram(size_t channel_count);

/* Stable hash of the channel list and RID, for config_id. Must be reproducible by every
 * implementation, so the algorithm is fixed: FNV-1a over RID then each name, each
 * length-prefixed to keep boundaries unambiguous. */
uint64_t nm_frame_config_id(const char *dataset_rid,
                            const char *const *channel_names,
                            size_t channel_count);

/*
 * Encode a HELLO. Returns bytes written, or 0 if it would not fit or an argument is
 * out of range.
 */
size_t nm_frame_encode_hello(uint8_t *buf, size_t buf_len,
                             uint64_t session_id, uint64_t config_id,
                             const char *dataset_rid,
                             const char *const *channel_names,
                             size_t channel_count);

/*
 * Encode a DATA frame from row-major values — the same layout the ring stores and the
 * protobuf encoder reads, so no transposition happens anywhere.
 *
 * Writes at most `row_count` rows and reports how many it actually took through
 * *rows_written, so a caller can advance its cursor and send again.
 */
size_t nm_frame_encode_data(uint8_t *buf, size_t buf_len,
                            uint64_t session_id, uint64_t config_id,
                            uint64_t sequence,
                            const int64_t *timestamps, size_t row_count,
                            const double *values, size_t channel_count,
                            size_t *rows_written);

/*
 * Decode just the header. Returns 0 on success, negative if the datagram is too short,
 * has the wrong magic, or an unsupported version. Cheap enough to run before deciding
 * whether to care about a frame.
 */
int nm_frame_decode_header(const uint8_t *buf, size_t len, nm_frame_header *out);

/*
 * Decode a HELLO body. Names are returned as offsets into `buf` with explicit lengths
 * rather than NUL-terminated copies, so decoding allocates nothing; the caller copies
 * what it wants to keep.
 */
typedef struct {
    const char *ptr;
    size_t      len;
} nm_frame_str;

typedef struct {
    nm_frame_str dataset_rid;
    nm_frame_str channels[NM_FRAME_MAX_CHANNELS];
    size_t       channel_count;
} nm_frame_hello;

int nm_frame_decode_hello(const uint8_t *buf, size_t len, nm_frame_hello *out);

/*
 * Decode a DATA body. Points into `buf`; values are little-endian float64 and must be
 * read through nm_frame_read_f64 rather than cast, since alignment is not guaranteed.
 */
typedef struct {
    uint64_t       sequence;
    size_t         channel_count;
    size_t         row_count;
    const uint8_t *rows;   /* row_count * (8 + 8*channel_count) bytes */
} nm_frame_data;

int nm_frame_decode_data(const uint8_t *buf, size_t len, nm_frame_data *out);

/* Read row `row` out of a decoded DATA frame. `values` must hold channel_count doubles. */
void nm_frame_read_row(const nm_frame_data *d, size_t row,
                       int64_t *timestamp_nanos, double *values);

#endif /* NM_FRAME_H */
