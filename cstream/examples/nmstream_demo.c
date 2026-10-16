/*
 * nmstream-demo — pushes one second of synthetic signal into a Nominal dataset.
 *
 * Exercises the C client end to end without Simulink: the ring, the sender thread, and
 * either the HTTPS POST to Nominal (direct) or frames to a relay (proxy, -p).
 *
 *   set NOMINAL_TOKEN=...      (a Nominal API key, direct mode only)
 *   nmstream-demo [-p host:port] <dataset-rid> [base-url]
 *
 * Writes 1000 rows across three channels (3000 points), so point it at something
 * disposable.
 */

#include "nominal_stream.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROWS      1000
#define CHANNELS  3
#define PERIOD_NS 1000000LL   /* 1 ms between samples */

int main(int argc, char **argv)
{
    static const char *const names[CHANNELS] = {
        "cdemo.sine", "cdemo.cosine", "cdemo.ramp"
    };

    const char *token = getenv("NOMINAL_TOKEN");
    const char *rid   = NULL;
    const char *base  = "https://api.gov.nominal.io/api";

    /* -p host:port switches to proxy mode, where this process sends datagrams and a
     * relay does the protobuf, TLS and HTTP. No API key is needed: the dataset RID
     * travels in the frames and the credential stays on the relay. */
    const char *proxy_host = NULL;
    uint16_t    proxy_port = 0;
    int         argi = 1;

    if (argc > 2 && strcmp(argv[1], "-p") == 0) {
        static char hostbuf[256];
        const char *colon = strrchr(argv[2], ':');

        if (colon == NULL || (size_t)(colon - argv[2]) >= sizeof hostbuf) {
            fprintf(stderr, "-p wants host:port, e.g. -p 127.0.0.1:15400\n");
            return 2;
        }
        memcpy(hostbuf, argv[2], (size_t)(colon - argv[2]));
        hostbuf[colon - argv[2]] = '\0';
        proxy_host = hostbuf;
        proxy_port = (uint16_t)atoi(colon + 1);
        argi = 3;
    }

    if (argc > argi) {
        rid = argv[argi];
    }
    if (argc > argi + 1) {
        base = argv[argi + 1];
    }

    nm_config  cfg;
    nm_status  status;
    nm_stream *s;
    nm_stats_t stats;
    int64_t    t0;
    int        i, dropped = 0;

    if (rid == NULL) {
        fprintf(stderr,
                "usage: nmstream-demo [-p host:port] <dataset-rid> [base-url]\n"
                "  no -p   send directly to Nominal over HTTPS (set NOMINAL_TOKEN to an "
                "API key)\n"
                "  -p      send to a relay instead (no API key needed)\n");
        return 2;
    }
    if (proxy_host == NULL && (token == NULL || token[0] == '\0')) {
        fprintf(stderr, "direct mode needs a Nominal API key. Set NOMINAL_TOKEN, or use "
                        "-p to send through a relay.\n");
        return 2;
    }

    memset(&cfg, 0, sizeof cfg);
    cfg.dataset_rid        = rid;
    cfg.channel_names      = names;
    cfg.channel_count      = CHANNELS;
    cfg.capacity_rows      = 4096;
    cfg.max_batch_delay_ms = 100;

    if (proxy_host != NULL) {
        cfg.mode       = NM_MODE_PROXY;
        cfg.proxy_host = proxy_host;
        cfg.proxy_port = proxy_port;
        printf("mode          = proxy -> %s:%u\n", proxy_host, (unsigned)proxy_port);
    } else {
        cfg.mode     = NM_MODE_DIRECT;
        cfg.base_url = base;
        cfg.token    = token;
        printf("mode          = direct -> %s\n", base);
    }

    /* NULL means the platform's own trust store (on Windows, Schannel and the Windows
     * certificate store). An RTOS has no such store, so
     * NOMINAL_CA_BUNDLE exists for the targets, where an explicit bundle is the only
     * option. */
    cfg.ca_bundle_path = getenv("NOMINAL_CA_BUNDLE");
    if (cfg.ca_bundle_path != NULL && cfg.ca_bundle_path[0] == '\0') {
        cfg.ca_bundle_path = NULL;
    }
    printf("CA bundle     = %s\n",
           cfg.ca_bundle_path ? cfg.ca_bundle_path : "(platform trust store)");

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

    printf("\npushed %d rows (%d discarded because the buffer was full)\n", ROWS, dropped);

    /* Flush, then report the totals after that flush. Reporting before close would show
     * whatever happened to be sent so far, so a total transport failure could look like
     * success. */
    nm_close_stats(s, &stats);

    printf("\naccepted=%llu sent=%llu dropped=%llu  requests ok=%llu failed=%llu\n",
           (unsigned long long)stats.rows_accepted,
           (unsigned long long)stats.rows_sent,
           (unsigned long long)stats.rows_dropped,
           (unsigned long long)stats.requests_ok,
           (unsigned long long)stats.requests_failed);
    printf("last failed HTTP status = %ld\n", stats.last_http_status);
    if (stats.last_error[0] != '\0') {
        printf("last error       = %s\n", stats.last_error);
    }

    if (stats.rows_sent == 0) {
        printf("\nnothing landed. See the status and error above.\n");
        return 1;
    }
    if (stats.rows_dropped > 0) {
        printf("\n%llu of %d rows were not sent. See the status and error above.\n",
               (unsigned long long)stats.rows_dropped, ROWS);
        return 1;
    }

    if (cfg.mode == NM_MODE_PROXY) {
        /* rows_sent counts rows handed to the socket. Whether they reach Nominal is
         * up to the relay, so claim no more than that. */
        printf("\ndone. Handed %llu rows to the relay.\n",
               (unsigned long long)stats.rows_sent);
        return 0;
    }

    printf("\ndone. Points appear under %s / %s / %s.\n", names[0], names[1], names[2]);
    printf("They can take a moment to show up.\n");
    return 0;
}
