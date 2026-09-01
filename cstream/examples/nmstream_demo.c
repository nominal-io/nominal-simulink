/*
 * nmstream-demo — streams a few seconds of synthetic signal into a Nominal dataset.
 *
 * This is the end-to-end proof for the C client: protobuf encoding, the ring, the
 * sender thread and the HTTPS POST, exercised together against the real server before
 * any Simulink code exists.
 *
 *   set NOMINAL_TOKEN=...
 *   nmstream-demo <dataset-rid> [base-url]
 *
 * Writes about 2000 points across three channels, so point it at something disposable.
 */

#include "nominal_stream.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The library's own sleep is internal, and widening the public API for an example
 * would be the tail wagging the dog. */
#if defined(_WIN32)
#include <windows.h>
static void nm_demo_sleep_ms(unsigned ms) { Sleep(ms); }
#else
#include <time.h>
static void nm_demo_sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
}
#endif

#define ROWS      1000
#define CHANNELS  3
#define PERIOD_NS 1000000LL   /* 1 ms between samples */

int main(int argc, char **argv)
{
    static const char *const names[CHANNELS] = {
        "cdemo.sine", "cdemo.cosine", "cdemo.ramp"
    };

    const char *token = getenv("NOMINAL_TOKEN");
    const char *rid   = (argc > 1) ? argv[1] : NULL;
    const char *base  = (argc > 2) ? argv[2] : "https://api.gov.nominal.io/api";

    nm_config  cfg;
    nm_status  status;
    nm_stream *s;
    nm_stats_t stats;
    int64_t    t0;
    int        i, dropped = 0;

    if (token == NULL || token[0] == '\0') {
        fprintf(stderr, "set NOMINAL_TOKEN in the environment\n");
        return 2;
    }
    if (rid == NULL) {
        fprintf(stderr, "usage: nmstream-demo <dataset-rid> [base-url]\n");
        return 2;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.base_url           = base;
    cfg.token              = token;
    cfg.dataset_rid        = rid;
    cfg.channel_names      = names;
    cfg.channel_count      = CHANNELS;
    cfg.capacity_rows      = 4096;
    cfg.max_batch_delay_ms = 100;
    cfg.ca_bundle_path     = NULL;

    printf("opening stream to %s\n", rid);
    s = nm_open(&cfg, &status);
    if (s == NULL) {
        fprintf(stderr, "nm_open failed: %s\n", nm_strerror(status));
        return 1;
    }

    /* Timestamps are the caller's business. Anchor one second in the PAST and count
     * forward, so every point is behind wall clock by the time it is sent -- stamping
     * into the future is an easy way to write data that never shows up. */
    t0 = nm_now_nanos() - (int64_t)ROWS * PERIOD_NS;

    printf("t0            = %lld ns since epoch\n", (long long)t0);
    printf("last          = %lld ns (%d rows, %lld ns apart)\n",
           (long long)(t0 + (int64_t)(ROWS - 1) * PERIOD_NS), ROWS, (long long)PERIOD_NS);
    printf("wall clock is = %lld ns\n\n", (long long)nm_now_nanos());

    for (i = 0; i < ROWS; i++) {
        double  phase = (double)i / 50.0;
        int64_t ts    = t0 + (int64_t)i * PERIOD_NS;
        double  row[CHANNELS];

        row[0] = sin(phase);
        row[1] = cos(phase);
        row[2] = (double)i;

        if (i < 3) {
            printf("row %d: ts=%lld  %s=%.6f  %s=%.6f  %s=%.6f\n",
                   i, (long long)ts,
                   names[0], row[0], names[1], row[1], names[2], row[2]);
        }

        if (nm_push(s, ts, row, CHANNELS) == 1) {
            dropped++;
        }
    }

    printf("\npushed %d rows (%d discarded at the ring)\n", ROWS, dropped);

    /* nm_close flushes, but it also frees the stream -- so poll until the sender has
     * accounted for every row, then report. Otherwise a total transport failure looks
     * exactly like success: the loop still completes and the process still exits 0. */
    for (i = 0; i < 100; i++) {
        nm_stats(s, &stats);
        if (stats.rows_sent + stats.rows_dropped >= stats.rows_accepted) {
            break;
        }
        nm_demo_sleep_ms(100);
    }

    printf("\naccepted=%llu sent=%llu dropped=%llu  requests ok=%llu failed=%llu\n",
           (unsigned long long)stats.rows_accepted,
           (unsigned long long)stats.rows_sent,
           (unsigned long long)stats.rows_dropped,
           (unsigned long long)stats.requests_ok,
           (unsigned long long)stats.requests_failed);
    printf("last HTTP status = %ld\n", stats.last_http_status);
    if (stats.last_error[0] != '\0') {
        printf("last error       = %s\n", stats.last_error);
    }

    nm_close(s);

    if (stats.requests_failed > 0 || stats.rows_sent == 0) {
        printf("\nnothing landed -- see the status and error above.\n");
        return 1;
    }

    printf("\ndone. Points appear under %s / %s / %s;\n", names[0], names[1], names[2]);
    printf("ingest is not instant, so allow a moment.\n");
    return 0;
}
