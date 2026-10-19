/*
 * nm_frame.h — the host-to-proxy wire format.
 *
 * Proxy mode exists because some hosts have no performance headroom left. Direct mode
 * is cheap on the step path but the sender thread still pays for protobuf, TLS and HTTP
 * on the same machine. Proxy mode moves all of that to another box, leaving the host
 * doing little more than a sendto.
 *
 * Pure encode/decode, no sockets, no allocation. A Speedgoat target only ever sends
 * frames. The decoder is here for the tests and as the reference the MATLAB and Go
 * decoders are checked against.
 *
 *
 * DESIGN
 *
 * Automatic channel configuration is the property worth protecting, so the format is
 * built around it. Three frame types:
 *
 *   HELLO  session, config_id, dataset RID, ordered channel names, tags, the run.
 *   DATA   session, config_id, sequence, rows of (timestamp, values[]).
 *   END    session, config_id, the time the session's data ends.
 *
 * DATA references channels by index into the HELLO list, never by name, so a row costs
 * 8 bytes per channel and the sender does no string work at all.
 *
 * HELLO is sent when the stream opens AND repeated periodically. That is not redundancy
 * for its own sake: UDP is lossy, and a proxy restarting mid-run would otherwise have no
 * way to recover the mapping. There is no back-channel to ask over, so periodic HELLO is
 * what makes the system self-healing.
 *
 * config_id is a hash of the RID, the channel list and the tags. A proxy seeing a DATA frame with
 * an unknown config_id discards it and waits for the next HELLO — bounded loss after a
 * restart. It also makes a *changed* channel list unambiguous rather than silently
 * misaligning columns, which is the failure mode that would otherwise produce
 * plausible-looking wrong data.
 *
 * The run is per session, not per configuration, so it is left out of config_id. A relay
 * creates the run on a session's first HELLO and sets its end on END. END is sent several
 * times, since UDP can lose any one of them, and a relay acts on the first.
 *
 * Numeric fields are little-endian, not the big-endian "network byte order" convention.
 * That convention governs IP/TCP/UDP header fields. A datagram payload is opaque to the
 * stack, and protobuf, FlatBuffers and Arrow also use LE. Here both ends are x86-64,
 * and the payload is mostly IEEE-754 doubles that already sit in memory little-endian.
 * Big-endian would mean byte-swapping every value — at 1 kHz across 20 channels,
 * ~160 KB/s of pure swapping on the machine whose lack of headroom is why proxy mode
 * exists at all. The magic is a byte sequence precisely so this choice does not also
 * make packet captures unreadable.
 *
 * Encoders must write LE explicitly rather than memcpy a struct.
 *
 * Frames carry no authentication. Anyone who can reach the proxy's port can write to
 * any dataset it serves. 
 */

#ifndef NM_FRAME_H
#define NM_FRAME_H

#include <stdint.h>
#include <stddef.h>

#include "nominal_stream.h"   /* nm_tag */

/*
 * Magic, checked before anything else is trusted.
 *
 * A byte sequence rather than an integer, on purpose: written as a little-endian
 * uint32 it would appear as "1SMN" in tcpdump or Wireshark, and a magic number's
 * whole job is to be recognisable when you are staring at a packet capture.
 */
#define NM_FRAME_MAGIC       "NMS1"
#define NM_FRAME_MAGIC_BYTES 4

#define NM_FRAME_VERSION 1

/* Datagrams stay under this so IP does not fragment them on a standard 1500-byte
 * Ethernet MTU. Fragmentation would turn one lost fragment into a lost frame, which is
 * a worse failure mode than sending more datagrams. */
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
 * absurd name, RID or tag. A tag's key and value are each held to the name limit. */
#define NM_FRAME_MAX_NAME_BYTES 256
#define NM_FRAME_MAX_RID_BYTES  256
#define NM_FRAME_MAX_TAGS       64

typedef enum {
    NM_FRAME_HELLO = 1,
    NM_FRAME_DATA  = 2,
    NM_FRAME_END   = 3
} nm_frame_type;

/* What run a relay makes for a session, from the HELLO's run-mode. */
typedef enum {
    NM_FRAME_RUN_NONE   = 0,
    NM_FRAME_RUN_OWN    = 1,   /* a run for this session alone */
    NM_FRAME_RUN_SHARED = 2    /* one run for every session with this title */
} nm_frame_run_mode;

/*
 * Common header, 24 bytes, identical in both frame types so a decoder can dispatch
 * before knowing which it holds.
 *
 *   offset size field
 *   0      4    magic       NM_FRAME_MAGIC
 *   4      1    version     NM_FRAME_VERSION
 *   5      1    type        nm_frame_type
 *   6      2    reserved    zero
 *   8      8    session_id  unique per stream; distinguishes concurrent models
 *   16     8    config_id   hash of the RID, channel list and tags
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
 *   frame         = hello-frame / data-frame / end-frame
 *
 *   hello-frame   = magic version %x01 reserved session-id config-id hello-body
 *   data-frame    = magic version %x02 reserved session-id config-id data-body
 *   end-frame     = magic version %x03 reserved session-id config-id end-time
 *
 *   magic         = %x4E.4D.53.31   ; "NMS1", in order
 *   version       = OCTET           ; NM_FRAME_VERSION; a decoder rejects others
 *   reserved      = 2OCTET          ; zero
 *   session-id    = u64             ; distinguishes concurrent models
 *   config-id     = u64             ; hash of the RID, channel names and tags
 *
 *   hello-body    = rid channel-count *channel-name tag-count *tag run
 *   rid           = length *OCTET   ; not NUL-terminated
 *   channel-name  = length *OCTET   ; not NUL-terminated
 *   tag-count     = u16
 *   tag           = tag-key tag-value
 *   tag-key       = length 1*OCTET  ; not NUL-terminated, not empty
 *   tag-value     = length *OCTET   ; not NUL-terminated
 *
 *   run           = run-mode run-start run-tag-count run-title ref-name
 *   run-mode      = %x00            ; no run
 *                 / %x01            ; a run for this session
 *                 / %x02            ; one run for every session with this title
 *   run-start     = i64             ; nanoseconds, Unix epoch
 *   run-tag-count = u16             ; the run shows the points with the first this-many tags
 *   run-title     = length *OCTET   ; not NUL-terminated
 *   ref-name      = length *OCTET   ; the name the dataset goes under in the run
 *
 *   end-time      = i64             ; nanoseconds, Unix epoch: the newest row's timestamp
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
 *   1. hello-body contains exactly channel-count occurrences of channel-name, and
 *      exactly tag-count occurrences of tag.
 *   2. data-body contains exactly row-count occurrences of row.
 *   3. each row contains exactly channel-count occurrences of value.
 *   4. channel-count in a data-frame must equal the channel-count of the hello-frame
 *      bearing the same config-id.
 *   5. no frame exceeds NM_FRAME_MAX_BYTES, channel-count <= NM_FRAME_MAX_CHANNELS, and
 *      tag-count <= NM_FRAME_MAX_TAGS.
 *   6. run-tag-count <= tag-count, and unless run-mode is %x00, run-title and ref-name
 *      are not empty.
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

/* Stable hash of the RID, channel list and tags, for config_id. Must be reproducible by
 * every implementation, so the algorithm is fixed: FNV-1a over the RID, each name, then
 * each tag's key and value, each string preceded by its length as a little-endian u32 to
 * keep boundaries unambiguous. With no tags it is the hash of the RID and names alone. */
uint64_t nm_frame_config_id(const char *dataset_rid,
                            const char *const *channel_names,
                            size_t channel_count,
                            const nm_tag *tags, size_t tag_count);

/* The run a HELLO asks a relay for. title and ref_name are each at most
 * NM_FRAME_MAX_NAME_BYTES, and both are required unless mode is NM_FRAME_RUN_NONE. */
typedef struct {
    nm_frame_run_mode mode;
    int64_t           start_nanos;
    size_t            tag_count;   /* the run shows the points with the first tag_count tags */
    const char       *title;
    const char       *ref_name;
} nm_frame_run;

/*
 * Encode a HELLO. run NULL asks for no run. Returns bytes written, or 0 if it would not
 * fit or an argument is out of range.
 */
size_t nm_frame_encode_hello(uint8_t *buf, size_t buf_len,
                             uint64_t session_id, uint64_t config_id,
                             const char *dataset_rid,
                             const char *const *channel_names,
                             size_t channel_count,
                             const nm_tag *tags, size_t tag_count,
                             const nm_frame_run *run);

/* Encode an END. Returns bytes written, or 0 if buf is too small. */
size_t nm_frame_encode_end(uint8_t *buf, size_t buf_len,
                           uint64_t session_id, uint64_t config_id, int64_t end_nanos);

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
 * has the wrong magic, an unsupported version, or an unknown frame type. Cheap enough
 * to run before deciding whether to care about a frame.
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
    nm_frame_str tag_keys[NM_FRAME_MAX_TAGS];
    nm_frame_str tag_values[NM_FRAME_MAX_TAGS];
    size_t       tag_count;
    uint8_t      run_mode;        /* an nm_frame_run_mode */
    int64_t      run_start;
    size_t       run_tag_count;
    nm_frame_str run_title;
    nm_frame_str ref_name;
} nm_frame_hello;

int nm_frame_decode_hello(const uint8_t *buf, size_t len, nm_frame_hello *out);

/* Decode an END. Returns 0 and the end time, or negative if malformed. */
int nm_frame_decode_end(const uint8_t *buf, size_t len, int64_t *end_nanos);

/*
 * Decode a DATA body. Points into `buf`; values are little-endian float64 and must be
 * read through nm_frame_read_row rather than cast, since alignment is not guaranteed.
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
