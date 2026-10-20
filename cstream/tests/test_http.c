/*
 * Direct-mode behaviour against a fake server on the loopback interface.
 *
 * The encoder and the ring have their own tests; this covers what only shows up once
 * requests fail or stall: what the stats report afterwards, and how long nm_close is
 * allowed to take. No token or network is needed. The server speaks just enough
 * HTTP/1.1 to read one request and answer it, over plain HTTP so no TLS is involved.
 */

/* Sockets first: on Windows winsock2.h must precede the windows.h nm_thread.h brings. */
#if defined(_WIN32)
#include <winsock2.h>
typedef SOCKET sock_t;
typedef int    socklen_t;
#define NM_CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define NM_CLOSESOCK close
#endif

#include "nominal_stream.h"
#include "nm_thread.h"

#include <stdio.h>
#include <stdlib.h>
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

/* ----------------------------------------------------------- fake server */

/* A listening socket on 127.0.0.1 with an ephemeral port. Connections complete into
 * the backlog whether or not anything accepts them, so a listener nobody serves is a
 * server that never answers. */
static sock_t listen_loopback(unsigned short *port)
{
    struct sockaddr_in addr;
    socklen_t          len = sizeof addr;
    sock_t             s = socket(AF_INET, SOCK_STREAM, 0);

    memset(&addr, 0, sizeof addr);
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    bind(s, (struct sockaddr *)&addr, sizeof addr);
    listen(s, 16);
    getsockname(s, (struct sockaddr *)&addr, &len);
    *port = ntohs(addr.sin_port);
    return s;
}

typedef struct {
    sock_t     listener;
    const int *statuses;   /* one per request, answered in order */
    int        count;
} scripted_server;

/* Read one request: headers, then as many body bytes as Content-Length says. */
static void read_request(sock_t c)
{
    char   buf[8192];
    size_t have = 0;
    char  *end = NULL;
    long   body = 0;

    while (end == NULL && have < sizeof buf - 1) {
        int n = recv(c, buf + have, (int)(sizeof buf - 1 - have), 0);
        if (n <= 0) {
            return;
        }
        have += (size_t)n;
        buf[have] = '\0';
        end = strstr(buf, "\r\n\r\n");
    }
    if (end == NULL) {
        return;
    }
    {
        const char *cl = strstr(buf, "Content-Length:");
        if (cl == NULL) {
            cl = strstr(buf, "content-length:");
        }
        if (cl != NULL) {
            body = strtol(cl + 15, NULL, 10);
        }
    }
    body -= (long)(have - (size_t)(end + 4 - buf));
    while (body > 0) {
        int n = recv(c, buf, (int)sizeof buf, 0);
        if (n <= 0) {
            return;
        }
        body -= n;
    }
}

static NM_THREAD_FN(serve_script, arg)
{
    scripted_server *srv = arg;
    int              i;

    for (i = 0; i < srv->count; i++) {
        char   reply[128];
        sock_t c = accept(srv->listener, NULL, NULL);

        read_request(c);
        snprintf(reply, sizeof reply,
                 "HTTP/1.1 %d X\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                 srv->statuses[i]);
        send(c, reply, (int)strlen(reply), 0);
        NM_CLOSESOCK(c);
    }
    NM_THREAD_RETURN;
}

/* ---------------------------------------------------------------- stream */

static const char *const NAMES[] = { "a", "b", "c" };

static nm_stream *open_direct(unsigned short port, size_t capacity_rows,
                              uint32_t max_delay_ms, uint32_t close_timeout_ms)
{
    char      url[64];
    nm_config cfg;

    snprintf(url, sizeof url, "http://127.0.0.1:%u", (unsigned)port);
    memset(&cfg, 0, sizeof cfg);
    cfg.mode               = NM_MODE_DIRECT;
    cfg.base_url           = url;   /* copied by nm_open */
    cfg.token              = "test-not-a-real-token";
    cfg.dataset_rid        = "ri.catalog.test.dataset.0";
    cfg.channel_names      = NAMES;
    cfg.channel_count      = 3;
    cfg.capacity_rows      = capacity_rows;
    cfg.max_batch_delay_ms = max_delay_ms;
    cfg.close_timeout_ms   = close_timeout_ms;
    return nm_open(&cfg, NULL);
}

static void push_rows(nm_stream *s, int count, int64_t first)
{
    const double vals[3] = { 1.0, 2.0, 3.0 };
    int          i;

    for (i = 0; i < count; i++) {
        nm_push(s, first + i, vals, 3);
    }
}

/*
 * Stopping must not wait on a server that never answers. The final flush used to give
 * every remaining batch three attempts of up to 30 s each, so stopping a model with
 * Nominal unreachable could take many minutes. The request here is already in flight
 * when nm_close is called, which is the case a simple check before each request would
 * miss.
 */
static void test_close_is_bounded_when_the_server_never_answers(void)
{
    unsigned short port;
    sock_t         listener = listen_loopback(&port);
    nm_stream     *s = open_direct(port, 64, 20, 1000);
    nm_stats_t     st;
    int64_t        start, elapsed_ms;

    check(s != NULL, "a direct stream opens against the fake server");
    if (s == NULL) {
        NM_CLOSESOCK(listener);
        return;
    }

    push_rows(s, 10, 1000000000LL);
    nm_sleep_ms(300);   /* let the sender start its request */

    start = nm_now_nanos();
    nm_close_stats(s, &st);
    elapsed_ms = (nm_now_nanos() - start) / 1000000;

    printf("      (nm_close took %lld ms with a 1000 ms limit)\n", (long long)elapsed_ms);
    check(elapsed_ms < 4000, "nm_close returns soon after its limit, not after the timeouts");
    check(st.rows_sent == 0 && st.rows_dropped == 10,
          "rows that never got through are counted as dropped");
    check(st.last_error[0] != '\0', "and the report says why");

    NM_CLOSESOCK(listener);
}

/*
 * The status reported is the one that explains the loss. It used to be overwritten by
 * every request, so a failure followed by a success read "last HTTP status 204".
 */
static void test_a_later_success_keeps_the_failed_status(void)
{
    static const int statuses[] = { 503, 503, 503, 204 };   /* 3 attempts, then OK */
    scripted_server  srv;
    unsigned short   port;
    nm_thread        server;
    nm_stream       *s;
    nm_stats_t       st;

    srv.listener = listen_loopback(&port);
    srv.statuses = statuses;
    srv.count    = 4;
    nm_thread_start(&server, serve_script, &srv);

    s = open_direct(port, 64, 20, 5000);
    check(s != NULL, "a direct stream opens against the scripted server");
    if (s == NULL) {
        NM_CLOSESOCK(srv.listener);
        return;
    }

    push_rows(s, 5, 1000000000LL);
    nm_sleep_ms(1000);   /* first batch: three 503s */
    push_rows(s, 5, 2000000000LL);
    nm_close_stats(s, &st);   /* second batch: 204 */
    nm_thread_join(server);

    check(st.requests_failed == 1 && st.requests_ok == 1,
          "one batch failed and a later one succeeded");
    check(st.last_http_status == 503, "last_http_status is the failure's, not the success's");

    NM_CLOSESOCK(srv.listener);
}

/*
 * rows_dropped is the sum of two counters, one per thread: rows nm_push refused because
 * the buffer was full, and rows the sender gave up on. Both happen here. Four rows fill
 * the buffer and six are refused. The four then never get an answer and are dropped
 * at close.
 */
static void test_dropped_rows_counts_both_threads(void)
{
    unsigned short port;
    sock_t         listener = listen_loopback(&port);
    nm_stream     *s = open_direct(port, 4, 20, 1000);
    nm_stats_t     st;

    check(s != NULL, "a four-row direct stream opens against the fake server");
    if (s == NULL) {
        NM_CLOSESOCK(listener);
        return;
    }

    /* Ten rows at once, before the sender's first look, so four fit and six are
     * refused. The four then never get an answer and are dropped at close. */
    push_rows(s, 10, 1000000000LL);
    check(nm_dropped_rows(s) == 6, "rows refused by the full buffer count as dropped");

    nm_close_stats(s, &st);
    check(st.rows_pushed == 10 && st.rows_sent == 0 && st.rows_dropped == 10,
          "rows_dropped adds the sender's losses to the refused rows");

    NM_CLOSESOCK(listener);
}

/*
 * The sender's schedule. Rows arriving slowly wait for max_batch_delay_ms, so a slow
 * model is a request per delay rather than a request per row. Rows arriving quickly
 * don't wait: once half the buffer is full the sender sends, however long the delay,
 * which is what keeps a buffer that fills in less than the delay from overflowing.
 */
static void test_rows_wait_for_the_delay_or_half_the_buffer(void)
{
    static const int statuses[] = { 204 };
    scripted_server  srv;
    unsigned short   port;
    nm_thread        server;
    nm_stream       *s;
    nm_stats_t       st;

    srv.listener = listen_loopback(&port);
    srv.statuses = statuses;
    srv.count    = 1;
    nm_thread_start(&server, serve_script, &srv);

    /* A 64-row buffer, so the sender sends at 32 rows, and a delay longer than this
     * test takes. */
    s = open_direct(port, 64, 5000, 5000);
    check(s != NULL, "a direct stream opens against the scripted server");
    if (s == NULL) {
        NM_CLOSESOCK(srv.listener);
        return;
    }

    push_rows(s, 3, 1000000000LL);
    nm_sleep_ms(300);
    nm_stats(s, &st);
    check(st.requests_ok == 0 && st.rows_sent == 0,
          "three rows wait for the delay rather than going at once");

    push_rows(s, 29, 2000000000LL);
    nm_sleep_ms(300);
    nm_stats(s, &st);
    check(st.requests_ok == 1 && st.rows_sent == 32,
          "half a buffer of rows goes well before the delay is up");

    nm_close_stats(s, &st);
    nm_thread_join(server);
    check(st.requests_ok == 1 && st.rows_dropped == 0, "and nothing was lost");

    NM_CLOSESOCK(srv.listener);
}

static void test_a_short_delay_sends_a_trickle_promptly(void)
{
    static const int statuses[] = { 204 };
    scripted_server  srv;
    unsigned short   port;
    nm_thread        server;
    nm_stream       *s;
    nm_stats_t       st;

    srv.listener = listen_loopback(&port);
    srv.statuses = statuses;
    srv.count    = 1;
    nm_thread_start(&server, serve_script, &srv);

    s = open_direct(port, 64, 200, 5000);
    check(s != NULL, "a direct stream opens against the scripted server");
    if (s == NULL) {
        NM_CLOSESOCK(srv.listener);
        return;
    }

    push_rows(s, 3, 1000000000LL);
    nm_sleep_ms(600);
    nm_stats(s, &st);
    check(st.requests_ok == 1 && st.rows_sent == 3,
          "three rows go once they have waited the delay");

    nm_close(s);
    nm_thread_join(server);
    NM_CLOSESOCK(srv.listener);
}

/*
 * A request in flight holds no buffer space. The rows leave the buffer as they are
 * encoded, so a slow request, or a server that never answers, doesn't fill the buffer
 * behind it. Before this, the sender held its rows until the request finished, and a
 * stall as long as the buffer took to fill dropped every row that arrived meanwhile.
 */
static void test_a_slow_request_holds_no_buffer_space(void)
{
    unsigned short port;
    sock_t         listener = listen_loopback(&port);
    nm_stream     *s = open_direct(port, 8, 20, 500);
    nm_stats_t     st;

    check(s != NULL, "an eight-row direct stream opens against the fake server");
    if (s == NULL) {
        NM_CLOSESOCK(listener);
        return;
    }

    /* Four rows, half the buffer, so the sender sends them at its next look, into a
     * request that never gets an answer. */
    push_rows(s, 4, 1000000000LL);
    nm_sleep_ms(200);

    /* The buffer holds eight, and all eight fit only if the four in flight have left
     * it. */
    push_rows(s, 8, 2000000000LL);
    check(nm_dropped_rows(s) == 0, "a full buffer of rows fits behind a request in flight");

    nm_close_stats(s, &st);
    check(st.rows_pushed == 12 && st.rows_sent == 0 && st.rows_dropped == 12,
          "and every row is counted as lost once close gives up on them");

    NM_CLOSESOCK(listener);
}

/*
 * Wall-clock mode stamps one row per step with nm_now_nanos, and rows that share a
 * timestamp overwrite each other in Nominal. On Windows the clock once moved only at the
 * timer interrupt, 15.6 ms apart, so a 1 kHz model lost 14 rows in 15. The clock's
 * smallest step has to be well under a model step. macOS steps in whole microseconds,
 * Linux and Windows finer.
 */
static void test_clock_resolves_finer_than_a_step(void)
{
    int64_t last = nm_now_nanos(), finest = INT64_MAX;
    int     changes = 0;

    while (changes < 100) {
        int64_t now = nm_now_nanos();
        if (now != last) {
            if (now - last < finest)
                finest = now - last;
            changes++;
            last = now;
        }
    }
    printf("      (smallest step %lld ns)\n", (long long)finest);
    check(finest <= 10000, "the clock steps by 10 us or less");
}

/*
 * The sender times its sends, its HELLO repeats and the close limit on nm_mono_nanos, so
 * that a clock set backwards doesn't stall them until it catches up. A test can't step
 * the computer's clock, but it can check this one keeps time in nanoseconds and never
 * goes backwards.
 */
static void test_the_sender_clock_keeps_time_and_never_goes_back(void)
{
    int64_t start = nm_mono_nanos(), last = start, elapsed_ms;
    int     backwards = 0, i;

    for (i = 0; i < 100000; i++) {
        int64_t now = nm_mono_nanos();
        if (now < last) {
            backwards = 1;
        }
        last = now;
    }
    check(!backwards, "the sender's clock never goes backwards");

    start = nm_mono_nanos();
    nm_sleep_ms(200);
    elapsed_ms = (nm_mono_nanos() - start) / 1000000;
    check(elapsed_ms >= 190 && elapsed_ms < 1000, "and counts a 200 ms sleep as about 200 ms");
}

/* DIRECT mode needs somewhere to POST, so a missing base URL is refused at open. */
static void test_open_refuses_a_missing_base_url(void)
{
    static const char *const bad_urls[] = { NULL, "" };
    size_t                   i;

    for (i = 0; i < 2; i++) {
        nm_config  cfg;
        nm_status  status = NM_OK;
        nm_stream *s;

        memset(&cfg, 0, sizeof cfg);
        cfg.mode          = NM_MODE_DIRECT;
        cfg.base_url      = bad_urls[i];
        cfg.token         = "test-not-a-real-token";
        cfg.dataset_rid   = "ri.catalog.test.dataset.0";
        cfg.channel_names = NAMES;
        cfg.channel_count = 3;
        cfg.capacity_rows = 64;
        cfg.max_batch_delay_ms = 20;
        cfg.close_timeout_ms   = 1000;

        s = nm_open(&cfg, &status);
        check(s == NULL && status == NM_ERR_ARG,
              i == 0 ? "a NULL base_url is refused as NM_ERR_ARG"
                     : "an empty base_url is refused as NM_ERR_ARG");
        nm_close(s);
    }
}

int main(void)
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    test_close_is_bounded_when_the_server_never_answers();
    test_a_later_success_keeps_the_failed_status();
    test_dropped_rows_counts_both_threads();
    test_rows_wait_for_the_delay_or_half_the_buffer();
    test_a_short_delay_sends_a_trickle_promptly();
    test_a_slow_request_holds_no_buffer_space();
    test_the_sender_clock_keeps_time_and_never_goes_back();
    test_clock_resolves_finer_than_a_step();
    test_open_refuses_a_missing_base_url();

    printf("\n%s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
