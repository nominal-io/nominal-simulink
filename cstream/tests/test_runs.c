/*
 * Run requests against a fake server on the loopback interface, which records each
 * request and answers with canned JSON. No token or network is needed. Covers what the
 * requests say, and what is read back from the answers.
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

/* ------------------------------------------------------------ fake server */

#define MAX_REQUESTS 4

typedef struct {
    sock_t      listener;
    int         count;                     /* requests to answer, then stop */
    int         status[MAX_REQUESTS];
    const char *reply[MAX_REQUESTS];
    char        seen[MAX_REQUESTS][8192];  /* each request as received */
} fake_server;

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

/* Read headers, then as many body bytes as Content-Length says, into out. */
static void read_request(sock_t c, char *out, size_t cap)
{
    size_t have = 0;
    char  *end = NULL;
    long   body = 0;

    while (end == NULL && have < cap - 1) {
        int n = recv(c, out + have, (int)(cap - 1 - have), 0);
        if (n <= 0) {
            return;
        }
        have += (size_t)n;
        out[have] = '\0';
        end = strstr(out, "\r\n\r\n");
    }
    if (end == NULL) {
        return;
    }
    {
        const char *cl = strstr(out, "Content-Length:");
        if (cl == NULL) {
            cl = strstr(out, "content-length:");
        }
        if (cl != NULL) {
            body = strtol(cl + 15, NULL, 10);
        }
    }
    body -= (long)(have - (size_t)(end + 4 - out));
    while (body > 0 && have < cap - 1) {
        int n = recv(c, out + have, (int)(cap - 1 - have), 0);
        if (n <= 0) {
            return;
        }
        have += (size_t)n;
        out[have] = '\0';
        body -= n;
    }
}

static NM_THREAD_FN(serve, arg)
{
    fake_server *srv = arg;
    int          i;

    for (i = 0; i < srv->count; i++) {
        char   head[160];
        sock_t c = accept(srv->listener, NULL, NULL);

        read_request(c, srv->seen[i], sizeof srv->seen[i]);
        snprintf(head, sizeof head,
                 "HTTP/1.1 %d X\r\nContent-Type: application/json\r\n"
                 "Content-Length: %u\r\nConnection: close\r\n\r\n",
                 srv->status[i], (unsigned)strlen(srv->reply[i]));
        send(c, head, (int)strlen(head), 0);
        send(c, srv->reply[i], (int)strlen(srv->reply[i]), 0);
        NM_CLOSESOCK(c);
    }
    NM_THREAD_RETURN;
}

/* --------------------------------------------------------------- the runs */

static const nm_tag TAGS[] = { { "model", "A" }, { "execution-id", "7" } };

static char url[64];

static nm_run_config run_config(unsigned short port, const char *title, int shared)
{
    nm_run_config cfg;

    snprintf(url, sizeof url, "http://127.0.0.1:%u", (unsigned)port);
    memset(&cfg, 0, sizeof cfg);
    cfg.base_url    = url;
    cfg.token       = "test-not-a-real-key";
    cfg.dataset_rid = "ri.catalog.test.dataset.0";
    cfg.ref_name    = "default";
    cfg.title       = title;
    cfg.tags        = TAGS;
    cfg.tag_count   = 2;
    cfg.start_nanos = 1500000000LL;   /* 1.5 s after the epoch */
    cfg.shared      = shared;
    return cfg;
}

/* Answer `count` requests in the background. */
static nm_thread start_server(fake_server *srv, unsigned short *port)
{
    nm_thread t;

    srv->listener = listen_loopback(port);
    nm_thread_start(&t, serve, srv);
    return t;
}

static void stop_server(fake_server *srv, nm_thread t)
{
    nm_thread_join(t);
    NM_CLOSESOCK(srv->listener);
}

static void test_begin_creates_a_run(void)
{
    static fake_server srv;
    unsigned short     port;
    nm_thread          t;
    nm_run_config      cfg;
    char               rid[128], err[256];
    nm_status          st;

    memset(&srv, 0, sizeof srv);
    srv.count    = 1;
    srv.status[0] = 200;
    /* A nested rid comes first, as in a real Run, whose data sources carry RIDs too. */
    srv.reply[0] = "{\"dataSources\":{\"default\":{\"dataSource\":{\"rid\":\"nested\"}}},"
                   "\"title\":\"Model A, execution 7\",\"rid\":\"ri.scout.test.run.1\"}";
    t   = start_server(&srv, &port);
    cfg = run_config(port, "Model A, execution 7", 0);

    st = nm_run_begin(&cfg, rid, sizeof rid, err, sizeof err);
    stop_server(&srv, t);

    check(st == NM_OK && strcmp(rid, "ri.scout.test.run.1") == 0,
          "begin returns the new run's RID, not a nested one");
    check(strncmp(srv.seen[0], "POST /scout/v1/run ", 19) == 0, "begin POSTs to /scout/v1/run");
    check(strstr(srv.seen[0], "Authorization: Bearer test-not-a-real-key") != NULL,
          "with the API key");
    check(strstr(srv.seen[0], "\"title\":\"Model A, execution 7\"") != NULL &&
          strstr(srv.seen[0], "\"startTime\":{\"secondsSinceEpoch\":1,"
                              "\"offsetNanoseconds\":500000000}") != NULL,
          "with the title and start time");
    check(strstr(srv.seen[0], "\"dataSources\":{\"default\":{\"dataSourceRid\":"
                              "\"ri.catalog.test.dataset.0\",\"seriesTags\":"
                              "{\"model\":\"A\",\"execution-id\":\"7\"}}}") != NULL,
          "with the dataset under its ref name, filtered to the tags");
    check(strstr(srv.seen[0], "\"workspace\"") == NULL, "and no workspace unless given one");
}

static void test_end_sets_the_end_time(void)
{
    static fake_server srv;
    unsigned short     port;
    nm_thread          t;
    nm_run_config      cfg;
    char               err[256];
    nm_status          st;

    memset(&srv, 0, sizeof srv);
    srv.count     = 2;
    srv.status[0] = srv.status[1] = 200;
    srv.reply[0]  = srv.reply[1] = "{\"rid\":\"ri.scout.test.run.1\"}";
    t = start_server(&srv, &port);

    cfg = run_config(port, "Model A, execution 7", 0);
    st  = nm_run_end(&cfg, "ri.scout.test.run.1", 61000000000LL, err, sizeof err);
    check(st == NM_OK, "end succeeds");
    cfg.shared = 1;
    nm_run_end(&cfg, "ri.scout.test.run.1", 61000000000LL, err, sizeof err);
    stop_server(&srv, t);

    check(strncmp(srv.seen[0], "PUT /scout/v1/run/ri.scout.test.run.1 ", 38) == 0,
          "end PUTs to the run");
    check(strstr(srv.seen[0], "{\"endTime\":{\"secondsSinceEpoch\":61,"
                              "\"offsetNanoseconds\":0}}") != NULL,
          "with only the end time");
    check(strstr(srv.seen[1], "\"strictOverwrite\":false") != NULL,
          "and on a shared run, only moves the end later");
}

static void test_shared_reuses_the_exact_title(void)
{
    static fake_server srv;
    unsigned short     port;
    nm_thread          t;
    nm_run_config      cfg;
    char               rid[128], err[256];
    nm_status          st;

    memset(&srv, 0, sizeof srv);
    srv.count     = 1;
    srv.status[0] = 200;
    /* The search matches substrings, case-insensitively, so only the exact title counts.
     * The match here also has an escaped character in it. */
    srv.reply[0]  = "{\"results\":["
                    "{\"rid\":\"ri.scout.test.run.longer\",\"title\":\"Mod\\u00e9l A, take 2\"},"
                    "{\"rid\":\"ri.scout.test.run.case\",\"title\":\"MOD\\u00c9L A\"},"
                    "{\"title\":\"Mod\\u00e9l A\",\"links\":[],\"rid\":\"ri.scout.test.run.right\"}"
                    "],\"nextPageToken\":null}";
    t   = start_server(&srv, &port);
    cfg = run_config(port, "Mod\xc3\xa9l A", 1);

    st = nm_run_begin(&cfg, rid, sizeof rid, err, sizeof err);
    stop_server(&srv, t);

    check(st == NM_OK && strcmp(rid, "ri.scout.test.run.right") == 0,
          "a shared run reuses the run with exactly its title");
    check(strncmp(srv.seen[0], "POST /scout/v1/search-runs ", 27) == 0 &&
          strstr(srv.seen[0], "\"query\":{\"type\":\"exactMatch\",\"exactMatch\":") != NULL,
          "found through search-runs");
}

static void test_shared_creates_when_none_exists(void)
{
    static fake_server srv;
    unsigned short     port;
    nm_thread          t;
    nm_run_config      cfg;
    char               rid[128], err[256];
    nm_status          st;

    memset(&srv, 0, sizeof srv);
    srv.count     = 2;
    srv.status[0] = srv.status[1] = 200;
    srv.reply[0]  = "{\"results\":[]}";
    srv.reply[1]  = "{\"rid\":\"ri.scout.test.run.new\"}";
    t   = start_server(&srv, &port);
    cfg = run_config(port, "Model A", 1);
    cfg.workspace_rid = "ri.security.test.workspace.1";

    st = nm_run_begin(&cfg, rid, sizeof rid, err, sizeof err);
    stop_server(&srv, t);

    check(st == NM_OK && strcmp(rid, "ri.scout.test.run.new") == 0,
          "a shared run with no match creates one");
    check(strstr(srv.seen[0], "{\"type\":\"workspace\",\"workspace\":"
                              "\"ri.security.test.workspace.1\"}") != NULL,
          "searching only the profile's workspace");
    check(strstr(srv.seen[1], "\"workspace\":\"ri.security.test.workspace.1\"") != NULL,
          "and creating it there");
}

static void test_a_refusal_is_reported(void)
{
    static fake_server srv;
    unsigned short     port;
    nm_thread          t;
    nm_run_config      cfg;
    char               rid[128], err[256];
    nm_status          st;

    memset(&srv, 0, sizeof srv);
    srv.count     = 1;
    srv.status[0] = 403;
    srv.reply[0]  = "{\"errorCode\":\"PERMISSION_DENIED\",\"errorInstanceId\":\"abc\"}";
    t   = start_server(&srv, &port);
    cfg = run_config(port, "Model A, execution 7", 0);

    st = nm_run_begin(&cfg, rid, sizeof rid, err, sizeof err);
    stop_server(&srv, t);

    check(st == NM_ERR_TRANSPORT && strstr(err, "HTTP status 403") != NULL &&
          strstr(err, "errorInstanceId") != NULL,
          "a refusal is reported with its status and the server's error");

    cfg.title = "";
    check(nm_run_begin(&cfg, rid, sizeof rid, err, sizeof err) == NM_ERR_ARG,
          "a run with no title is refused before any request");
}

int main(void)
{
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif

    test_begin_creates_a_run();
    test_end_sets_the_end_time();
    test_shared_reuses_the_exact_title();
    test_shared_creates_when_none_exists();
    test_a_refusal_is_reported();

    printf("\n%s\n", failures == 0 ? "all passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
