/*
 * The ring's contract is what makes nm_push safe to call from a control loop: bounded
 * capacity, no blocking, and an exact count of what was discarded. These tests pin
 * that behaviour, including the wrap, where an off-by-one would corrupt data rather
 * than fail loudly.
 */

#include "nm_ring.h"

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

#define CHANNELS 3

static void test_fills_then_discards(void)
{
    nm_ring r;
    double  row[CHANNELS] = { 1.0, 2.0, 3.0 };
    int     accepted = 0, rejected = 0, i;

    check(nm_ring_init(&r, 4, CHANNELS) == 0, "init");

    for (i = 0; i < 10; i++) {
        if (nm_ring_push(&r, (int64_t)i, row)) {
            accepted++;
        } else {
            rejected++;
        }
    }

    check(accepted == 4, "accepts exactly capacity rows");
    check(rejected == 6, "discards the rest rather than growing");
    check(nm_ring_readable(&r) == 4, "readable equals what was accepted");

    nm_ring_free(&r);
}

static void test_roundtrip_preserves_data(void)
{
    nm_ring r;
    const int64_t *ts;
    const double  *vals;
    size_t  run;
    int     i;

    nm_ring_init(&r, 8, CHANNELS);

    for (i = 0; i < 5; i++) {
        double row[CHANNELS] = { i * 10.0, i * 10.0 + 1, i * 10.0 + 2 };
        nm_ring_push(&r, 1000 + i, row);
    }

    run = nm_ring_peek(&r, &ts, &vals);
    check(run == 5, "peek reports every buffered row");

    for (i = 0; i < 5; i++) {
        if (ts[i] != 1000 + i ||
            vals[i * CHANNELS + 0] != i * 10.0 ||
            vals[i * CHANNELS + 1] != i * 10.0 + 1 ||
            vals[i * CHANNELS + 2] != i * 10.0 + 2) {
            break;
        }
    }
    check(i == 5, "rows come back row-major and unmodified");

    nm_ring_consume(&r, run);
    check(nm_ring_readable(&r) == 0, "consume empties the ring");

    nm_ring_free(&r);
}

static void test_wrap_splits_the_run(void)
{
    nm_ring r;
    const int64_t *ts;
    const double  *vals;
    double  row[CHANNELS] = { 0.0, 0.0, 0.0 };
    size_t  first, second;
    int     i;

    nm_ring_init(&r, 4, CHANNELS);

    /* Advance the tail so the next writes straddle the end of the backing array. */
    for (i = 0; i < 3; i++) {
        nm_ring_push(&r, i, row);
    }
    nm_ring_consume(&r, 3);

    for (i = 0; i < 4; i++) {
        row[0] = 100.0 + i;
        nm_ring_push(&r, 500 + i, row);
    }
    check(nm_ring_readable(&r) == 4, "ring refills after consume");

    first = nm_ring_peek(&r, &ts, &vals);
    check(first == 1, "a wrapped run stops at the end of the array");
    check(ts[0] == 500 && vals[0] == 100.0, "first run holds the oldest row");

    nm_ring_consume(&r, first);
    second = nm_ring_peek(&r, &ts, &vals);
    check(second == 3, "the remainder is available on the next peek");
    check(ts[0] == 501 && vals[0] == 101.0, "and continues in order");

    nm_ring_free(&r);
}

static void test_reuse_after_drain(void)
{
    nm_ring r;
    double  row[CHANNELS] = { 7.0, 8.0, 9.0 };
    int     i, accepted = 0;

    nm_ring_init(&r, 2, CHANNELS);

    /* Fill, drain, fill again, many times over -- this is the steady state of a
     * running model, and it is where a free-running index would go wrong if the
     * modulo arithmetic were off. */
    for (i = 0; i < 100; i++) {
        const int64_t *ts;
        const double  *vals;
        size_t run;

        if (nm_ring_push(&r, i, row)) {
            accepted++;
        }
        run = nm_ring_peek(&r, &ts, &vals);
        nm_ring_consume(&r, run);
    }

    check(accepted == 100, "steady fill-and-drain never falsely reports full");
    check(nm_ring_readable(&r) == 0, "and ends empty");

    nm_ring_free(&r);
}

int main(void)
{
    test_fills_then_discards();
    test_roundtrip_preserves_data();
    test_wrap_splits_the_run();
    test_reuse_after_drain();

    printf("\n%s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
