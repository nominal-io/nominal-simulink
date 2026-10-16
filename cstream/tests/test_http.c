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

static nm_stream *open_direct(unsigned short port, uint32_t close_timeout_ms)
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
    cfg.capacity_rows      = 64;
    cfg.max_batch_delay_ms = 20;
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
    nm_stream     *s = open_direct(port, 1000);
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

    s = open_direct(port, 5000);
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

int main(void)
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    test_close_is_bounded_when_the_server_never_answers();
    test_a_later_success_keeps_the_failed_status();

    printf("\n%s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
