#include "nm_frame.h"

#include <string.h>

/* ------------------------------------------------------------ little-endian I/O
 *
 * Written byte by byte rather than by casting a pointer. Two reasons: datagram
 * payloads have no alignment guarantee, so a cast to uint64_t* is undefined behaviour
 * on strict-alignment targets; and being explicit means a third-party implementation
 * has an unambiguous spec to match.
 */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v);
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    int i;
    for (i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static void put_u64(uint8_t *p, uint64_t v)
{
    int i;
    for (i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint64_t get_u64(const uint8_t *p)
{
    uint64_t v = 0;
    int      i;
    for (i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

static void put_f64(uint8_t *p, double d)
{
    uint64_t bits;
    memcpy(&bits, &d, sizeof bits);
    put_u64(p, bits);
}

static double get_f64(const uint8_t *p)
{
    uint64_t bits = get_u64(p);
    double   d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

/* ------------------------------------------------------------------- sizing */

#define ROW_BYTES(nch) (8u + 8u * (unsigned)(nch))
#define DATA_BODY_FIXED ((unsigned)NM_FRAME_DATA_FIXED_BYTES)

size_t nm_frame_rows_per_datagram(size_t channel_count)
{
    size_t overhead, room;

    if (channel_count == 0 || channel_count > NM_FRAME_MAX_CHANNELS) {
        return 0;
    }
    overhead = NM_FRAME_HEADER_BYTES + DATA_BODY_FIXED;
    if (overhead + ROW_BYTES(channel_count) > NM_FRAME_MAX_BYTES) {
        return 0;   /* one row will not fit; caller must reduce the channel count */
    }
    room = NM_FRAME_MAX_BYTES - overhead;
    return room / ROW_BYTES(channel_count);
}

/* --------------------------------------------------------------- config_id
 *
 * FNV-1a (Fowler-Noll-Vo, variant 1a): a non-cryptographic hash that is two constants
 * and two operations per byte —
 *
 *     h = offset_basis
 *     for each byte b:  h ^= b;  h *= prime      (wrapping at 64 bits)
 *
 * The 1a variant XORs before multiplying, where FNV-1 does the reverse; 1a distributes
 * better, so it is the one in general use. Chosen here because every proxy
 * implementation must compute an identical config_id, and this is short enough to
 * reimplement from the spec in any language without getting it subtly wrong. Go's
 * standard library has it as hash/fnv.
 *
 * It is not collision-resistant against an adversary, and does not need to be:
 * config_id only has to distinguish one configuration from another. Frames are
 * unauthenticated anyway, so it is not a security boundary.
 *
 * Constants in hex, matching the published spec. They were originally transcribed as
 * decimal and the offset basis lost a digit — a hash that still worked, but was not
 * FNV-1a, so an implementation using the real constant would have computed a different
 * config_id and had every frame silently rejected.
 *
 * Each field is length-prefixed so {"ab","c"} and {"a","bc"} cannot collide.
 */

#define FNV_OFFSET 0xcbf29ce484222325ULL   /* 14695981039346656037 */
#define FNV_PRIME  0x00000100000001b3ULL   /* 1099511628211 */

static uint64_t fnv1a(uint64_t h, const void *data, size_t len)
{
    const uint8_t *p = data;
    size_t         i;

    for (i = 0; i < len; i++) {
        h ^= (uint64_t)p[i];
        h *= FNV_PRIME;
    }
    return h;
}

static uint64_t fnv1a_field(uint64_t h, const char *s)
{
    uint8_t len_le[4];
    size_t  len = (s != NULL) ? strlen(s) : 0;

    put_u32(len_le, (uint32_t)len);
    h = fnv1a(h, len_le, sizeof len_le);
    return fnv1a(h, s, len);
}

uint64_t nm_frame_config_id(const char *dataset_rid,
                            const char *const *channel_names,
                            size_t channel_count,
                            const nm_tag *tags, size_t tag_count)
{
    uint64_t h = FNV_OFFSET;
    size_t   i;

    h = fnv1a_field(h, dataset_rid);
    for (i = 0; i < channel_count; i++) {
        h = fnv1a_field(h, channel_names != NULL ? channel_names[i] : NULL);
    }
    for (i = 0; i < tag_count && tags != NULL; i++) {
        h = fnv1a_field(h, tags[i].key);
        h = fnv1a_field(h, tags[i].value);
    }
    return h;
}

/* --------------------------------------------------------------- encoding */

/* Append a length-prefixed string, or return 0 if it would pass `limit` bytes or the
 * end of the buffer. */
static size_t put_string(uint8_t *buf, size_t buf_len, size_t at,
                         const char *s, size_t limit)
{
    size_t n;

    if (s == NULL) {
        return 0;
    }
    n = strlen(s);
    if (n > limit || at + 2 + n > buf_len) {
        return 0;
    }
    put_u16(buf + at, (uint16_t)n);
    memcpy(buf + at + 2, s, n);
    return at + 2 + n;
}

static size_t write_header(uint8_t *buf, size_t buf_len, uint8_t type,
                           uint64_t session_id, uint64_t config_id)
{
    if (buf_len < NM_FRAME_HEADER_BYTES) {
        return 0;
    }
    memcpy(buf, NM_FRAME_MAGIC, NM_FRAME_MAGIC_BYTES);   /* 4 bytes, no NUL */
    buf[4] = NM_FRAME_VERSION;
    buf[5] = type;
    put_u16(buf + 6, 0);
    put_u64(buf + 8, session_id);
    put_u64(buf + 16, config_id);
    return NM_FRAME_HEADER_BYTES;
}

size_t nm_frame_encode_hello(uint8_t *buf, size_t buf_len,
                             uint64_t session_id, uint64_t config_id,
                             const char *dataset_rid,
                             const char *const *channel_names,
                             size_t channel_count,
                             const nm_tag *tags, size_t tag_count)
{
    size_t at, rid_len, i;

    if (buf == NULL || dataset_rid == NULL || channel_names == NULL ||
        channel_count == 0 || channel_count > NM_FRAME_MAX_CHANNELS ||
        tag_count > NM_FRAME_MAX_TAGS || (tag_count > 0 && tags == NULL)) {
        return 0;
    }
    rid_len = strlen(dataset_rid);
    if (rid_len > NM_FRAME_MAX_RID_BYTES) {
        return 0;
    }
    if (buf_len > NM_FRAME_MAX_BYTES) {
        buf_len = NM_FRAME_MAX_BYTES;
    }

    at = write_header(buf, buf_len, NM_FRAME_HELLO, session_id, config_id);
    if (at == 0) {
        return 0;
    }

    if (at + 2 + rid_len + 2 > buf_len) {
        return 0;
    }
    put_u16(buf + at, (uint16_t)rid_len);
    at += 2;
    memcpy(buf + at, dataset_rid, rid_len);
    at += rid_len;

    put_u16(buf + at, (uint16_t)channel_count);
    at += 2;

    for (i = 0; i < channel_count; i++) {
        size_t n;
        if (channel_names[i] == NULL) {
            return 0;
        }
        n = strlen(channel_names[i]);
        if (n > NM_FRAME_MAX_NAME_BYTES) {
            return 0;
        }
        /* Refuse rather than truncate. A HELLO split across datagrams could leave a
         * proxy with a partial channel map, and misaligned columns are far worse than
         * a clear failure at open. */
        if (at + 2 + n > buf_len) {
            return 0;
        }
        put_u16(buf + at, (uint16_t)n);
        at += 2;
        memcpy(buf + at, channel_names[i], n);
        at += n;
    }

    if (at + 2 > buf_len) {
        return 0;
    }
    put_u16(buf + at, (uint16_t)tag_count);
    at += 2;

    for (i = 0; i < tag_count; i++) {
        if (tags[i].key == NULL || tags[i].key[0] == '\0') {
            return 0;
        }
        at = put_string(buf, buf_len, at, tags[i].key, NM_FRAME_MAX_NAME_BYTES);
        if (at == 0) {
            return 0;
        }
        at = put_string(buf, buf_len, at, tags[i].value, NM_FRAME_MAX_NAME_BYTES);
        if (at == 0) {
            return 0;
        }
    }

    return at;
}

size_t nm_frame_encode_data(uint8_t *buf, size_t buf_len,
                            uint64_t session_id, uint64_t config_id,
                            uint64_t sequence,
                            const int64_t *timestamps, size_t row_count,
                            const double *values, size_t channel_count,
                            size_t *rows_written)
{
    size_t at, fit, take, r, c;

    if (rows_written != NULL) {
        *rows_written = 0;
    }
    if (buf == NULL || timestamps == NULL || values == NULL ||
        channel_count == 0 || channel_count > NM_FRAME_MAX_CHANNELS) {
        return 0;
    }
    if (buf_len > NM_FRAME_MAX_BYTES) {
        buf_len = NM_FRAME_MAX_BYTES;
    }

    fit = nm_frame_rows_per_datagram(channel_count);
    if (fit == 0) {
        return 0;
    }
    take = row_count < fit ? row_count : fit;
    if (take == 0) {
        return 0;
    }

    at = write_header(buf, buf_len, NM_FRAME_DATA, session_id, config_id);
    if (at == 0) {
        return 0;
    }
    if (at + DATA_BODY_FIXED + take * ROW_BYTES(channel_count) > buf_len) {
        return 0;
    }

    put_u64(buf + at, sequence);
    at += 8;
    put_u16(buf + at, (uint16_t)channel_count);
    at += 2;
    put_u16(buf + at, (uint16_t)take);
    at += 2;

    for (r = 0; r < take; r++) {
        put_u64(buf + at, (uint64_t)timestamps[r]);
        at += 8;
        for (c = 0; c < channel_count; c++) {
            put_f64(buf + at, values[r * channel_count + c]);
            at += 8;
        }
    }

    if (rows_written != NULL) {
        *rows_written = take;
    }
    return at;
}

/* --------------------------------------------------------------- decoding
 *
 * Every read is bounds-checked against the datagram length. These bytes arrive from the
 * network from an unauthenticated sender, so a malformed or hostile frame must produce
 * a clean rejection rather than a read past the buffer.
 */

int nm_frame_decode_header(const uint8_t *buf, size_t len, nm_frame_header *out)
{
    if (buf == NULL || out == NULL || len < NM_FRAME_HEADER_BYTES) {
        return -1;
    }
    if (memcmp(buf, NM_FRAME_MAGIC, NM_FRAME_MAGIC_BYTES) != 0) {
        return -2;
    }
    if (buf[4] != NM_FRAME_VERSION) {
        return -3;
    }
    if (buf[5] != NM_FRAME_HELLO && buf[5] != NM_FRAME_DATA) {
        return -4;
    }

    out->version    = buf[4];
    out->type       = buf[5];
    out->session_id = get_u64(buf + 8);
    out->config_id  = get_u64(buf + 16);
    return 0;
}

/* Read a length-prefixed string at *at, advancing past it. */
static int read_string(const uint8_t *buf, size_t len, size_t *at, nm_frame_str *out)
{
    size_t n;

    if (*at + 2 > len) {
        return -1;
    }
    n = get_u16(buf + *at);
    *at += 2;
    if (*at + n > len) {
        return -1;
    }
    out->ptr = (const char *)(buf + *at);
    out->len = n;
    *at += n;
    return 0;
}

int nm_frame_decode_hello(const uint8_t *buf, size_t len, nm_frame_hello *out)
{
    nm_frame_header h;
    size_t          at, rid_len, count, i;

    if (out == NULL || nm_frame_decode_header(buf, len, &h) != 0) {
        return -1;
    }
    if (h.type != NM_FRAME_HELLO) {
        return -1;
    }

    memset(out, 0, sizeof *out);
    at = NM_FRAME_HEADER_BYTES;

    if (at + 2 > len) {
        return -1;
    }
    rid_len = get_u16(buf + at);
    at += 2;
    if (at + rid_len > len) {
        return -1;
    }
    out->dataset_rid.ptr = (const char *)(buf + at);
    out->dataset_rid.len = rid_len;
    at += rid_len;

    if (at + 2 > len) {
        return -1;
    }
    count = get_u16(buf + at);
    at += 2;
    if (count == 0 || count > NM_FRAME_MAX_CHANNELS) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        size_t n;
        if (at + 2 > len) {
            return -1;
        }
        n = get_u16(buf + at);
        at += 2;
        if (at + n > len) {
            return -1;
        }
        out->channels[i].ptr = (const char *)(buf + at);
        out->channels[i].len = n;
        at += n;
    }
    out->channel_count = count;

    if (at + 2 > len) {
        return -1;
    }
    count = get_u16(buf + at);
    at += 2;
    if (count > NM_FRAME_MAX_TAGS) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        if (read_string(buf, len, &at, &out->tag_keys[i]) != 0 ||
            out->tag_keys[i].len == 0 ||
            read_string(buf, len, &at, &out->tag_values[i]) != 0) {
            return -1;
        }
    }
    out->tag_count = count;
    return 0;
}

int nm_frame_decode_data(const uint8_t *buf, size_t len, nm_frame_data *out)
{
    nm_frame_header h;
    size_t          at, nch, rows, need;

    if (out == NULL || nm_frame_decode_header(buf, len, &h) != 0) {
        return -1;
    }
    if (h.type != NM_FRAME_DATA) {
        return -1;
    }

    at = NM_FRAME_HEADER_BYTES;
    if (at + DATA_BODY_FIXED > len) {
        return -1;
    }
    out->sequence = get_u64(buf + at);
    at += 8;
    nch = get_u16(buf + at);
    at += 2;
    rows = get_u16(buf + at);
    at += 2;

    if (nch == 0 || nch > NM_FRAME_MAX_CHANNELS || rows == 0) {
        return -1;
    }
    need = rows * ROW_BYTES(nch);
    if (at + need > len) {
        return -1;
    }

    out->channel_count = nch;
    out->row_count     = rows;
    out->rows          = buf + at;
    return 0;
}

void nm_frame_read_row(const nm_frame_data *d, size_t row,
                       int64_t *timestamp_nanos, double *values)
{
    const uint8_t *p;
    size_t         c;

    if (d == NULL || row >= d->row_count) {
        return;
    }
    p = d->rows + row * ROW_BYTES(d->channel_count);

    if (timestamp_nanos != NULL) {
        *timestamp_nanos = (int64_t)get_u64(p);
    }
    p += 8;

    if (values != NULL) {
        for (c = 0; c < d->channel_count; c++) {
            values[c] = get_f64(p + 8 * c);
        }
    }
}
