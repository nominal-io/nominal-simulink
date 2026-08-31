/*
 * Pins the encoder against tests/fixtures/body_identity.pb — the byte-exact protobuf the
 * Rust client produces for this exact input. If these bytes stop matching, either we
 * broke the encoder or the schema moved; regenerating the fixture (tools/fixturegen)
 * tells you which.
 */

#include "nm_encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void fail(const char *what)
{
    printf("FAIL: %s\n", what);
    failures++;
}

static void ok(const char *what)
{
    printf("ok:   %s\n", what);
}

/* The fixture's input, mirrored from tools/fixturegen/src/main.rs.
 * 2026-01-01T00:00:00Z, three samples 1 ms apart. */
#define T0_SECONDS 1767225600LL
#define T0_NANOS   (T0_SECONDS * 1000000000LL)

static void test_against_fixture(const char *path)
{
    static const nm_tag rpm_tags[] = { { "bank", "1" } };
    static const nm_channel_desc channels[] = {
        { "demo.rpm", rpm_tags, 1 },
        { "demo.egt", NULL,     0 }
    };
    static const int64_t timestamps[] = {
        T0_NANOS,
        T0_NANOS + 1000000LL,
        T0_NANOS + 2000000LL
    };
    /* Row-major: one row per timestamp, one column per channel — the shape a
     * Simulink bus produces at each step. */
    static const double values[] = {
        1500.00, 700.0,
        1510.50, 701.5,
        1521.25, 703.0
    };

    uint8_t  actual[512];
    uint8_t *expected;
    long     expected_len;
    size_t   sized, written;
    FILE    *f;

    f = fopen(path, "rb");
    if (f == NULL) {
        printf("FAIL: cannot open fixture %s\n", path);
        failures++;
        return;
    }
    fseek(f, 0, SEEK_END);
    expected_len = ftell(f);
    fseek(f, 0, SEEK_SET);
    expected = malloc((size_t)expected_len);
    if (expected == NULL || fread(expected, 1, (size_t)expected_len, f) != (size_t)expected_len) {
        printf("FAIL: cannot read fixture\n");
        failures++;
        fclose(f);
        free(expected);
        return;
    }
    fclose(f);

    sized = nm_encoded_size(channels, 2, timestamps, 3, values);
    if (sized != (size_t)expected_len) {
        printf("FAIL: nm_encoded_size said %zu, fixture is %ld\n", sized, expected_len);
        failures++;
    } else {
        ok("nm_encoded_size matches fixture length");
    }

    written = nm_encode_request(actual, sizeof actual, channels, 2, timestamps, 3, values);
    if (written != (size_t)expected_len) {
        printf("FAIL: encoded %zu bytes, fixture is %ld\n", written, expected_len);
        failures++;
    } else if (memcmp(actual, expected, (size_t)expected_len) != 0) {
        size_t i;
        for (i = 0; i < (size_t)expected_len; i++) {
            if (actual[i] != expected[i]) {
                printf("FAIL: first difference at byte %zu: got 0x%02x, want 0x%02x\n",
                       i, actual[i], expected[i]);
                break;
            }
        }
        failures++;
    } else {
        ok("encoded bytes match fixture exactly");
    }

    free(expected);
}

static void test_timestamp_split(void)
{
    int64_t secs;
    int32_t nanos;

    nm_split_timestamp(T0_NANOS, &secs, &nanos);
    if (secs != T0_SECONDS || nanos != 0) {
        fail("split of a whole second");
    } else {
        ok("split of a whole second");
    }

    nm_split_timestamp(T0_NANOS + 1500000LL, &secs, &nanos);
    if (secs != T0_SECONDS || nanos != 1500000) {
        fail("split of a sub-second offset");
    } else {
        ok("split of a sub-second offset");
    }

    /* Before the epoch. Timestamp requires nanos in [0, 1e9), so this must borrow a
     * second rather than produce a negative remainder as C division would. */
    nm_split_timestamp(-1LL, &secs, &nanos);
    if (secs != -1 || nanos != 999999999) {
        printf("FAIL: pre-epoch split gave %lld s, %d ns\n", (long long)secs, nanos);
        failures++;
    } else {
        ok("pre-epoch split borrows a second");
    }
}

static void test_buffer_too_small(void)
{
    static const nm_channel_desc channels[] = { { "x", NULL, 0 } };
    static const int64_t timestamps[] = { T0_NANOS };
    static const double values[] = { 1.0 };
    uint8_t tiny[4];

    if (nm_encode_request(tiny, sizeof tiny, channels, 1, timestamps, 1, values) != 0) {
        fail("undersized buffer should report 0, not a partial write");
    } else {
        ok("undersized buffer reports 0");
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: test_encode <path to body_identity.pb>\n");
        return 2;
    }

    test_against_fixture(argv[1]);
    test_timestamp_split();
    test_buffer_too_small();

    printf("\n%s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
