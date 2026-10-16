#include "nm_encode.h"

#include <string.h>

/*
 * Field numbers, from proto/nominal_write.proto. Kept as named constants because a
 * transposed digit here produces bytes the server accepts and silently misfiles.
 */
enum {
    F_REQUEST_SERIES      = 1,  /* WriteRequestNominal.series          */
    F_SERIES_CHANNEL      = 1,  /* Series.channel                      */
    F_SERIES_TAGS         = 2,  /* Series.tags (map<string,string>)    */
    F_SERIES_POINTS       = 3,  /* Series.points                       */
    F_CHANNEL_NAME        = 1,  /* Channel.name                        */
    F_MAP_KEY             = 1,  /* map entry key                       */
    F_MAP_VALUE           = 2,  /* map entry value                     */
    F_POINTS_DOUBLE       = 1,  /* Points.double_points (oneof arm)    */
    F_DOUBLEPOINTS_POINTS = 1,  /* DoublePoints.points                 */
    F_POINT_TIMESTAMP     = 1,  /* DoublePoint.timestamp               */
    F_POINT_VALUE         = 2,  /* DoublePoint.value                   */
    F_TS_SECONDS          = 1,  /* google.protobuf.Timestamp.seconds   */
    F_TS_NANOS            = 2   /* google.protobuf.Timestamp.nanos     */
};

enum { WT_VARINT = 0, WT_FIXED64 = 1, WT_LEN = 2 };

#define NANOS_PER_SEC 1000000000

/* ---------------------------------------------------------------- primitives */

static size_t varint_size(uint64_t v)
{
    size_t n = 1;
    while (v >= 0x80u) {
        v >>= 7;
        n++;
    }
    return n;
}

static size_t tag_size(uint32_t field, uint32_t wire_type)
{
    return varint_size(((uint64_t)field << 3) | wire_type);
}

/* A cursor that tracks overflow rather than trusting the caller. Once `overflow` is
 * set every further write is a no-op, so a too-small buffer fails cleanly at the end
 * instead of corrupting memory partway through. */
typedef struct {
    uint8_t *p;
    size_t   cap;
    size_t   len;
    int      overflow;
} nm_buf;

static void put_bytes(nm_buf *b, const void *src, size_t n)
{
    if (b->overflow) {
        return;
    }
    if (b->len + n > b->cap) {
        b->overflow = 1;
        return;
    }
    memcpy(b->p + b->len, src, n);
    b->len += n;
}

static void put_varint(nm_buf *b, uint64_t v)
{
    uint8_t tmp[10];
    size_t  n = 0;

    while (v >= 0x80u) {
        tmp[n++] = (uint8_t)(v | 0x80u);
        v >>= 7;
    }
    tmp[n++] = (uint8_t)v;
    put_bytes(b, tmp, n);
}

static void put_tag(nm_buf *b, uint32_t field, uint32_t wire_type)
{
    put_varint(b, ((uint64_t)field << 3) | wire_type);
}

static void put_fixed64(nm_buf *b, double value)
{
    /* Protobuf fixed64 is little-endian. Assemble byte by byte so this does not
     * depend on host endianness. QNX and Antelope are both x86-64 today, and handling
     * either byte order costs two lines. */
    uint64_t bits;
    uint8_t  out[8];
    int      i;

    memcpy(&bits, &value, sizeof bits);
    for (i = 0; i < 8; i++) {
        out[i] = (uint8_t)(bits >> (8 * i));
    }
    put_bytes(b, out, sizeof out);
}

static void put_len_field(nm_buf *b, uint32_t field, const char *s, size_t n)
{
    put_tag(b, field, WT_LEN);
    put_varint(b, (uint64_t)n);
    put_bytes(b, s, n);
}

/* ---------------------------------------------------------------- timestamps */

void nm_split_timestamp(int64_t nanos_since_epoch, int64_t *out_seconds, int32_t *out_nanos)
{
    int64_t secs = nanos_since_epoch / NANOS_PER_SEC;
    int64_t rem  = nanos_since_epoch % NANOS_PER_SEC;

    /* C integer division truncates toward zero, so a negative instant would yield a
     * negative remainder. Timestamp requires nanos in [0, 1e9), so borrow a second. */
    if (rem < 0) {
        rem += NANOS_PER_SEC;
        secs -= 1;
    }

    *out_seconds = secs;
    *out_nanos   = (int32_t)rem;
}

/* ---------------------------------------------------------------- sizing pass
 *
 * Every size_* function must mirror its write_* counterpart exactly, including proto3's
 * rule that a scalar equal to its default is omitted entirely. The fixture depends on
 * this: the first point's timestamp encodes seconds but not nanos, because nanos is 0.
 */

static size_t size_timestamp_body(int64_t seconds, int32_t nanos)
{
    size_t n = 0;

    if (seconds != 0) {
        n += tag_size(F_TS_SECONDS, WT_VARINT) + varint_size((uint64_t)seconds);
    }
    if (nanos != 0) {
        n += tag_size(F_TS_NANOS, WT_VARINT) + varint_size((uint64_t)(int64_t)nanos);
    }
    return n;
}

static size_t size_point_body(int64_t timestamp_nanos, double value)
{
    int64_t seconds;
    int32_t nanos;
    size_t  ts, n = 0;

    nm_split_timestamp(timestamp_nanos, &seconds, &nanos);
    ts = size_timestamp_body(seconds, nanos);

    /* The timestamp submessage is always present, even when it encodes to nothing. */
    n += tag_size(F_POINT_TIMESTAMP, WT_LEN) + varint_size((uint64_t)ts) + ts;

    if (value != 0.0) {
        n += tag_size(F_POINT_VALUE, WT_FIXED64) + 8;
    }
    return n;
}

static size_t size_map_entry_body(const nm_tag *tag)
{
    size_t klen = strlen(tag->key);
    size_t vlen = strlen(tag->value);
    size_t n = 0;

    if (klen != 0) {
        n += tag_size(F_MAP_KEY, WT_LEN) + varint_size((uint64_t)klen) + klen;
    }
    if (vlen != 0) {
        n += tag_size(F_MAP_VALUE, WT_LEN) + varint_size((uint64_t)vlen) + vlen;
    }
    return n;
}

static size_t size_series_body(const nm_channel_desc *ch,
                               const int64_t *timestamps, size_t row_count,
                               const double *values, size_t stride, size_t column)
{
    size_t namelen = strlen(ch->name);
    size_t chan_body = 0, points_inner = 0, points_body, n = 0;
    size_t i;

    if (namelen != 0) {
        chan_body = tag_size(F_CHANNEL_NAME, WT_LEN) + varint_size((uint64_t)namelen) + namelen;
    }
    n += tag_size(F_SERIES_CHANNEL, WT_LEN) + varint_size((uint64_t)chan_body) + chan_body;

    for (i = 0; i < ch->tag_count; i++) {
        size_t entry = size_map_entry_body(&ch->tags[i]);
        n += tag_size(F_SERIES_TAGS, WT_LEN) + varint_size((uint64_t)entry) + entry;
    }

    for (i = 0; i < row_count; i++) {
        size_t pt = size_point_body(timestamps[i], values[i * stride + column]);
        points_inner += tag_size(F_DOUBLEPOINTS_POINTS, WT_LEN) + varint_size((uint64_t)pt) + pt;
    }

    /* Points wraps DoublePoints wraps the repeated points: two nested length prefixes. */
    points_body = tag_size(F_POINTS_DOUBLE, WT_LEN)
                + varint_size((uint64_t)points_inner) + points_inner;
    n += tag_size(F_SERIES_POINTS, WT_LEN) + varint_size((uint64_t)points_body) + points_body;

    return n;
}

size_t nm_encoded_size(const nm_channel_desc *channels, size_t channel_count,
                       const int64_t *timestamps, size_t row_count,
                       const double *values)
{
    size_t total = 0, c;

    if (channels == NULL || timestamps == NULL || values == NULL) {
        return 0;
    }

    for (c = 0; c < channel_count; c++) {
        size_t body = size_series_body(&channels[c], timestamps, row_count,
                                       values, channel_count, c);
        total += tag_size(F_REQUEST_SERIES, WT_LEN) + varint_size((uint64_t)body) + body;
    }
    return total;
}

/* ---------------------------------------------------------------- write pass */

static void write_timestamp_body(nm_buf *b, int64_t seconds, int32_t nanos)
{
    if (seconds != 0) {
        put_tag(b, F_TS_SECONDS, WT_VARINT);
        put_varint(b, (uint64_t)seconds);
    }
    if (nanos != 0) {
        put_tag(b, F_TS_NANOS, WT_VARINT);
        put_varint(b, (uint64_t)(int64_t)nanos);
    }
}

static void write_point(nm_buf *b, int64_t timestamp_nanos, double value)
{
    int64_t seconds;
    int32_t nanos;

    nm_split_timestamp(timestamp_nanos, &seconds, &nanos);

    put_tag(b, F_POINT_TIMESTAMP, WT_LEN);
    put_varint(b, (uint64_t)size_timestamp_body(seconds, nanos));
    write_timestamp_body(b, seconds, nanos);

    if (value != 0.0) {
        put_tag(b, F_POINT_VALUE, WT_FIXED64);
        put_fixed64(b, value);
    }
}

static void write_series(nm_buf *b, const nm_channel_desc *ch,
                         const int64_t *timestamps, size_t row_count,
                         const double *values, size_t stride, size_t column)
{
    size_t namelen = strlen(ch->name);
    size_t chan_body = 0, points_inner = 0;
    size_t i;

    if (namelen != 0) {
        chan_body = tag_size(F_CHANNEL_NAME, WT_LEN) + varint_size((uint64_t)namelen) + namelen;
    }
    put_tag(b, F_SERIES_CHANNEL, WT_LEN);
    put_varint(b, (uint64_t)chan_body);
    if (namelen != 0) {
        put_len_field(b, F_CHANNEL_NAME, ch->name, namelen);
    }

    for (i = 0; i < ch->tag_count; i++) {
        const nm_tag *t = &ch->tags[i];
        size_t klen = strlen(t->key);
        size_t vlen = strlen(t->value);

        put_tag(b, F_SERIES_TAGS, WT_LEN);
        put_varint(b, (uint64_t)size_map_entry_body(t));
        if (klen != 0) {
            put_len_field(b, F_MAP_KEY, t->key, klen);
        }
        if (vlen != 0) {
            put_len_field(b, F_MAP_VALUE, t->value, vlen);
        }
    }

    for (i = 0; i < row_count; i++) {
        size_t pt = size_point_body(timestamps[i], values[i * stride + column]);
        points_inner += tag_size(F_DOUBLEPOINTS_POINTS, WT_LEN) + varint_size((uint64_t)pt) + pt;
    }

    put_tag(b, F_SERIES_POINTS, WT_LEN);
    put_varint(b, (uint64_t)(tag_size(F_POINTS_DOUBLE, WT_LEN)
                             + varint_size((uint64_t)points_inner) + points_inner));
    put_tag(b, F_POINTS_DOUBLE, WT_LEN);
    put_varint(b, (uint64_t)points_inner);

    for (i = 0; i < row_count; i++) {
        double v = values[i * stride + column];
        put_tag(b, F_DOUBLEPOINTS_POINTS, WT_LEN);
        put_varint(b, (uint64_t)size_point_body(timestamps[i], v));
        write_point(b, timestamps[i], v);
    }
}

size_t nm_encode_request(uint8_t *buf, size_t buf_len,
                         const nm_channel_desc *channels, size_t channel_count,
                         const int64_t *timestamps, size_t row_count,
                         const double *values)
{
    nm_buf b;
    size_t c;

    if (buf == NULL || channels == NULL || timestamps == NULL || values == NULL) {
        return 0;
    }

    b.p = buf;
    b.cap = buf_len;
    b.len = 0;
    b.overflow = 0;

    for (c = 0; c < channel_count; c++) {
        put_tag(&b, F_REQUEST_SERIES, WT_LEN);
        put_varint(&b, (uint64_t)size_series_body(&channels[c], timestamps, row_count,
                                                  values, channel_count, c));
        write_series(&b, &channels[c], timestamps, row_count, values, channel_count, c);
    }

    return b.overflow ? 0 : b.len;
}
