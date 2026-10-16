/*
 * Frame codec tests.
 *
 * Two jobs. First, round-tripping: what the host encodes is what a proxy decodes, with
 * timestamps and values intact. Second, and more important, rejecting malformed input:
 * these bytes arrive from the network from an unauthenticated sender, so a truncated or
 * hostile frame must be refused cleanly rather than read past the buffer.
 *
 * The dumped bytes at the end are the golden fixture the MATLAB and Go decoders are
 * pinned against, so all three implementations agree by construction rather than by
 * inspection.
 */

#include "nm_frame.h"
#include "nominal_stream.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void check(int cond, const char *what)
{
    if (cond) {
        printf("ok:   %s\n", what);
    } else {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

static const char *const NAMES[] = { "engine.rpm", "engine.egt", "engine.psi" };
static const char *const RID = "ri.catalog.test.dataset.abc";
#define NCH 3

/* row_count sits at header + sequence(8) + channel_count(2). */
static void put_row_count_ff(uint8_t *buf)
{
    buf[NM_FRAME_HEADER_BYTES + 10] = 0xFF;
    buf[NM_FRAME_HEADER_BYTES + 11] = 0xFF;
}

/*
 * config_id must be real FNV-1a, not merely a working hash.
 *
 * A transcription error in the offset basis once left it deterministic and
 * well-distributed but not FNV-1a — which is invisible to a round-trip test and only
 * shows up when a second implementation uses the published constant, computes a
 * different id, and has every frame rejected as an unknown config. So pin the
 * algorithm to its canonical vector rather than only checking self-consistency.
 *
 * FNV-1a/64 of the single byte 'a' is 0xaf63dc4c8601ec8c. Reproduced here by hashing a
 * one-character name with no RID, so the field framing is exercised too.
 */
static void test_fnv1a_matches_the_spec(void)
{
    /* An empty RID plus the single name "a" hashes the byte sequence
     *   00 00 00 00   (len 0, the RID)
     *   01 00 00 00   (len 1, the name)
     *   61            ('a')
     * Expected value computed independently from the published FNV-1a algorithm, not
     * captured from this implementation -- capturing our own output would have happily
     * blessed the broken constant. */
    static const char *const one[] = { "a" };
    uint64_t got = nm_frame_config_id("", one, 1);

    check(got == 0xd955c00fa740f76fULL, "config_id is genuine FNV-1a");
    if (got != 0xd955c00fa740f76fULL) {
        printf("      got 0x%016llx, want 0xd955c00fa740f76f\n",
               (unsigned long long)got);
    }
}

static void test_config_id_is_stable(void)
{
    uint64_t a = nm_frame_config_id(RID, NAMES, NCH);
    uint64_t b = nm_frame_config_id(RID, NAMES, NCH);

    check(a == b, "config_id is deterministic");
    check(a != 0, "config_id is non-zero");

    /* A changed channel list must change the id — that is the whole mechanism for
     * catching a reconfigured model, and a collision here would silently misalign
     * columns at the proxy. */
    {
        static const char *const other[] = { "engine.rpm", "engine.egt", "engine.oil" };
        check(nm_frame_config_id(RID, other, NCH) != a,
              "config_id changes when a channel name changes");
    }
    {
        check(nm_frame_config_id(RID, NAMES, 2) != a,
              "config_id changes when the channel count changes");
    }
    {
        check(nm_frame_config_id("ri.catalog.test.dataset.xyz", NAMES, NCH) != a,
              "config_id changes when the dataset changes");
    }
    /* Length-prefixing exists to stop this: without it, splitting a name differently
     * would hash the same. */
    {
        static const char *const shifted[] = { "engine.rp", "mengine.egt", "engine.psi" };
        check(nm_frame_config_id(RID, shifted, NCH) != a,
              "config_id is not fooled by shifting a character between names");
    }
}

static void test_hello_roundtrip(void)
{
    uint8_t        buf[NM_FRAME_MAX_BYTES];
    nm_frame_hello h;
    uint64_t       cfg = nm_frame_config_id(RID, NAMES, NCH);
    size_t         n;
    size_t         i;
    int            names_ok = 1;

    n = nm_frame_encode_hello(buf, sizeof buf, 0x1122334455667788ULL, cfg,
                              RID, NAMES, NCH);
    check(n > NM_FRAME_HEADER_BYTES, "HELLO encodes");

    check(nm_frame_decode_hello(buf, n, &h) == 0, "HELLO decodes");
    check(h.channel_count == NCH, "HELLO carries the channel count");
    check(h.dataset_rid.len == strlen(RID) &&
          memcmp(h.dataset_rid.ptr, RID, h.dataset_rid.len) == 0,
          "HELLO carries the dataset RID");

    for (i = 0; i < NCH; i++) {
        if (h.channels[i].len != strlen(NAMES[i]) ||
            memcmp(h.channels[i].ptr, NAMES[i], h.channels[i].len) != 0) {
            names_ok = 0;
        }
    }
    check(names_ok, "HELLO carries channel names in order");

    {
        nm_frame_header hdr;
        check(nm_frame_decode_header(buf, n, &hdr) == 0 &&
              hdr.session_id == 0x1122334455667788ULL && hdr.config_id == cfg,
              "header carries session and config ids");
    }
}

static void test_data_roundtrip(void)
{
    uint8_t       buf[NM_FRAME_MAX_BYTES];
    nm_frame_data d;
    uint64_t      cfg = nm_frame_config_id(RID, NAMES, NCH);
    const int64_t ts[3] = { 1000000000LL, 1000001000LL, 1000002000LL };
    const double  vals[9] = {
        1500.0,  700.0,  30.0,
        1510.5,  701.5,  30.1,
        1521.25, 703.0,  30.2
    };
    size_t n, rows = 0, r;
    int    ok = 1;

    n = nm_frame_encode_data(buf, sizeof buf, 7ULL, cfg, 42ULL,
                             ts, 3, vals, NCH, &rows);
    check(n > 0 && rows == 3, "DATA encodes all three rows");
    check(nm_frame_decode_data(buf, n, &d) == 0, "DATA decodes");
    check(d.sequence == 42ULL, "DATA carries the sequence number");
    check(d.channel_count == NCH && d.row_count == 3, "DATA carries its shape");

    for (r = 0; r < 3; r++) {
        int64_t got_ts;
        double  got[NCH];
        size_t  c;

        nm_frame_read_row(&d, r, &got_ts, got);
        if (got_ts != ts[r]) {
            ok = 0;
        }
        for (c = 0; c < NCH; c++) {
            if (got[c] != vals[r * NCH + c]) {
                ok = 0;
            }
        }
    }
    check(ok, "every timestamp and value survives the round trip");
}

static void test_data_respects_mtu(void)
{
    uint8_t buf[NM_FRAME_MAX_BYTES];
    int64_t ts[400];
    double  vals[400 * 2];
    size_t  i, n, rows = 0;

    for (i = 0; i < 400; i++) {
        ts[i] = (int64_t)i;
        vals[i * 2 + 0] = (double)i;
        vals[i * 2 + 1] = (double)i + 0.5;
    }

    /* Two channels is 24 bytes a row, so 400 rows cannot fit. The encoder must take a
     * prefix and say how many, rather than truncating a row or overrunning. */
    n = nm_frame_encode_data(buf, sizeof buf, 1, 1, 0, ts, 400, vals, 2, &rows);
    check(n > 0 && n <= NM_FRAME_MAX_BYTES, "an oversized batch still fits the MTU");
    check(rows > 0 && rows < 400, "and reports the partial row count");
    check(rows == nm_frame_rows_per_datagram(2), "matching rows_per_datagram");

    {
        nm_frame_data d;
        check(nm_frame_decode_data(buf, n, &d) == 0 && d.row_count == rows,
              "the truncated frame is still well-formed");
    }
}

/*
 * The channel bound must be reachable and not exceedable. NM_FRAME_MAX_CHANNELS was
 * originally a round number picked by hand, which turned out to be three times what
 * the MTU actually allows — unreachable, and an 8 KB decode struct for nothing. It is
 * now derived from NM_FRAME_MAX_BYTES, so this checks the derivation rather than
 * trusting it.
 */
static void test_channel_bound_is_consistent(void)
{
    check(nm_frame_rows_per_datagram(NM_FRAME_MAX_CHANNELS) >= 1,
          "one row at NM_FRAME_MAX_CHANNELS fits a datagram");
    check(nm_frame_rows_per_datagram(NM_FRAME_MAX_CHANNELS + 1) == 0,
          "one more channel than the maximum does not fit");

    /* Encoding at the limit must actually work, not just be arithmetically possible. */
    {
        static int64_t ts[1];
        static double  vals[NM_FRAME_MAX_CHANNELS];
        static uint8_t buf[NM_FRAME_MAX_BYTES];
        size_t rows = 0, n;

        n = nm_frame_encode_data(buf, sizeof buf, 1, 1, 0, ts, 1,
                                 vals, NM_FRAME_MAX_CHANNELS, &rows);
        check(n > 0 && n <= NM_FRAME_MAX_BYTES && rows == 1,
              "a full-width row encodes within the MTU");
    }

    printf("      (NM_FRAME_MAX_CHANNELS = %u, %zu rows/datagram at 3 channels)\n",
           (unsigned)NM_FRAME_MAX_CHANNELS, nm_frame_rows_per_datagram(3));
}

static void test_rejects_malformed(void)
{
    uint8_t         buf[NM_FRAME_MAX_BYTES];
    uint8_t         copy[NM_FRAME_MAX_BYTES];
    nm_frame_header hdr;
    nm_frame_hello  h;
    nm_frame_data   d;
    uint64_t        cfg = nm_frame_config_id(RID, NAMES, NCH);
    size_t          n, i;
    int             all_rejected = 1;

    n = nm_frame_encode_hello(buf, sizeof buf, 1, cfg, RID, NAMES, NCH);

    check(nm_frame_decode_header(buf, 4, &hdr) < 0, "a runt datagram is rejected");

    memcpy(copy, buf, n);
    copy[0] ^= 0xFF;
    check(nm_frame_decode_header(copy, n, &hdr) < 0, "bad magic is rejected");

    memcpy(copy, buf, n);
    copy[4] = 99;
    check(nm_frame_decode_header(copy, n, &hdr) < 0, "an unknown version is rejected");

    memcpy(copy, buf, n);
    copy[5] = 77;
    check(nm_frame_decode_header(copy, n, &hdr) < 0, "an unknown frame type is rejected");

    /* Truncation at every offset. A length field that claims more than the datagram
     * holds is the obvious way to walk a decoder off the end of a buffer, so check
     * every cut rather than one. */
    for (i = 1; i < n; i++) {
        if (nm_frame_decode_hello(buf, i, &h) == 0) {
            printf("  truncating HELLO to %zu bytes was accepted\n", i);
            all_rejected = 0;
        }
    }
    check(all_rejected, "HELLO truncated at any length is rejected");

    /* A HELLO whose channel_count claims more channels than the bytes provide. */
    memcpy(copy, buf, n);
    copy[NM_FRAME_HEADER_BYTES + 2 + strlen(RID)]     = 0xFF;
    copy[NM_FRAME_HEADER_BYTES + 2 + strlen(RID) + 1] = 0x00;
    check(nm_frame_decode_hello(copy, n, &h) < 0,
          "an inflated channel count is rejected");

    /* Same for DATA. */
    {
        const int64_t ts[2] = { 1, 2 };
        const double  vals[4] = { 1.0, 2.0, 3.0, 4.0 };
        size_t        rows = 0, dn;

        dn = nm_frame_encode_data(buf, sizeof buf, 1, cfg, 0, ts, 2, vals, 2, &rows);
        all_rejected = 1;
        for (i = 1; i < dn; i++) {
            if (nm_frame_decode_data(buf, i, &d) == 0) {
                printf("  truncating DATA to %zu bytes was accepted\n", i);
                all_rejected = 0;
            }
        }
        check(all_rejected, "DATA truncated at any length is rejected");

        memcpy(copy, buf, dn);
        put_row_count_ff(copy);
        check(nm_frame_decode_data(copy, dn, &d) < 0,
              "an inflated row count is rejected");
    }

    /* A HELLO must not decode as DATA, or vice versa. */
    n = nm_frame_encode_hello(buf, sizeof buf, 1, cfg, RID, NAMES, NCH);
    check(nm_frame_decode_data(buf, n, &d) < 0, "a HELLO does not decode as DATA");
}

static void dump_fixture(const char *label, const uint8_t *buf, size_t n)
{
    size_t i;
    printf("\n%s (%zu bytes)\n", label, n);
    for (i = 0; i < n; i++) {
        printf("%02x", buf[i]);
        if ((i + 1) % 32 == 0) {
            printf("\n");
        }
    }
    if (n % 32 != 0) {
        printf("\n");
    }
}

/* A proxy-mode stream aimed at the discard port. Nothing needs to listen: UDP sends
 * succeed or fail locally, and these tests are about what nm_open and nm_push decide. */
static nm_stream *open_proxy(const char *const *names, size_t count, size_t capacity,
                             nm_status *status)
{
    nm_config cfg;

    memset(&cfg, 0, sizeof cfg);
    cfg.mode               = NM_MODE_PROXY;
    cfg.proxy_host         = "127.0.0.1";
    cfg.proxy_port         = 9;
    cfg.dataset_rid        = RID;
    cfg.channel_names      = names;
    cfg.channel_count      = count;
    cfg.capacity_rows      = capacity;
    /* Long enough that the sender cannot drain the ring between the pushes below. */
    cfg.max_batch_delay_ms = 1000;
    return nm_open(&cfg, status);
}

/*
 * The channel count limit is not the only one. The HELLO also carries every name and the
 * RID, so forty 40-byte names fit the count but not the datagram. That once passed
 * nm_open, and the stream then never announced its mapping: the relay discarded every
 * row and the host reported nothing wrong.
 */
static void test_open_refuses_a_mapping_too_large_to_send(void)
{
    static char        storage[40][41];
    const char        *names[40];
    nm_status          status = NM_OK;
    nm_stream         *s;
    size_t             i;

    for (i = 0; i < 40; i++) {
        snprintf(storage[i], sizeof storage[i], "subsystem.signal.with.a.long.name.%04u",
                 (unsigned)i);
        names[i] = storage[i];
    }

    s = open_proxy(names, 40, 16, &status);
    check(s == NULL && status == NM_ERR_TOO_LARGE,
          "nm_open refuses a channel list too large for one datagram");
    nm_close(s);

    s = open_proxy(NAMES, NCH, 16, &status);
    check(s != NULL && status == NM_OK, "and still opens one that fits");
    nm_close(s);
}

/*
 * rows_dropped counts rows refused at the ring as well as rows that failed to send, so
 * the total a loss is measured against must count them too. Measured against
 * rows_accepted, the block once reported more rows lost than written.
 */
static void test_pushed_counts_rows_the_ring_refused(void)
{
    const double vals[NCH] = { 1.0, 2.0, 3.0 };
    nm_status    status = NM_OK;
    nm_stats_t   st;
    nm_stream   *s;
    int          i;

    s = open_proxy(NAMES, NCH, 4, &status);
    check(s != NULL, "a small proxy stream opens");
    if (s == NULL) {
        return;
    }
    for (i = 0; i < 10; i++) {
        nm_push(s, 1000000000LL + i, vals, NCH);
    }
    nm_stats(s, &st);
    check(st.rows_pushed == 10, "rows_pushed counts every row handed to nm_push");
    check(st.rows_accepted < 10 && st.rows_accepted + st.rows_dropped == 10,
          "rows the full ring refused are dropped, not accepted");
    nm_close(s);
}

static void print_fixtures(void)
{
    uint8_t  buf[NM_FRAME_MAX_BYTES];
    uint64_t cfg = nm_frame_config_id(RID, NAMES, NCH);
    const int64_t ts[3] = { 1000000000LL, 1000001000LL, 1000002000LL };
    const double  vals[9] = {
        1500.0,  700.0,  30.0,
        1510.5,  701.5,  30.1,
        1521.25, 703.0,  30.2
    };
    size_t n, rows = 0;

    printf("\n--- golden fixtures for the MATLAB and Go decoders ---");
    printf("\nconfig_id = 0x%016llx\n", (unsigned long long)cfg);

    n = nm_frame_encode_hello(buf, sizeof buf, 0x1122334455667788ULL, cfg,
                              RID, NAMES, NCH);
    dump_fixture("HELLO", buf, n);

    n = nm_frame_encode_data(buf, sizeof buf, 0x1122334455667788ULL, cfg, 42ULL,
                             ts, 3, vals, NCH, &rows);
    dump_fixture("DATA", buf, n);
}

int main(int argc, char **argv)
{
    test_fnv1a_matches_the_spec();
    test_channel_bound_is_consistent();
    test_config_id_is_stable();
    test_hello_roundtrip();
    test_data_roundtrip();
    test_data_respects_mtu();
    test_rejects_malformed();
    test_open_refuses_a_mapping_too_large_to_send();
    test_pushed_counts_rows_the_ring_refused();

    if (argc > 1 && strcmp(argv[1], "--fixtures") == 0) {
        print_fixtures();
    }

    printf("\n%s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
