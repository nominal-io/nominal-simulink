#include "nm_udp.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET nm_socket;
  #define NM_INVALID_SOCKET INVALID_SOCKET
  #define nm_close_socket   closesocket
  #define nm_socket_errno   WSAGetLastError()
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int nm_socket;
  #define NM_INVALID_SOCKET (-1)
  #define nm_close_socket   close
  #define nm_socket_errno   errno
#endif

struct nm_udp {
    nm_socket fd;
};

/* Write a reason into the caller's buffer, if it gave one. */
static void say(char *err, size_t err_len, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL || err_len == 0) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

/* Winsock needs process-wide startup. Reference-counted rather than one-shot so a
 * host that opens and closes streams repeatedly does not leave it initialised
 * forever, and so nothing breaks if the application also uses sockets. */
#if defined(_WIN32)
static long g_wsa_refs = 0;

static int wsa_acquire(char *err, size_t err_len)
{
    WSADATA data;
    if (InterlockedIncrement(&g_wsa_refs) == 1) {
        int rc = WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            InterlockedDecrement(&g_wsa_refs);
            say(err, err_len, "WSAStartup failed (%d)", rc);
            return -1;
        }
    }
    return 0;
}

static void wsa_release(void)
{
    if (InterlockedDecrement(&g_wsa_refs) == 0) {
        WSACleanup();
    }
}
#endif

static int set_nonblocking(nm_socket fd)
{
#if defined(_WIN32)
    u_long on = 1;
    return ioctlsocket(fd, FIONBIO, &on) == 0 ? 0 : -1;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0 ? 0 : -1;
#endif
}

nm_udp *nm_udp_open(const char *host, uint16_t port, char *err, size_t err_len)
{
    struct addrinfo  hints;
    struct addrinfo *res = NULL, *ai;
    nm_udp          *u = NULL;
    char             port_text[16];
    int              rc;

    say(err, err_len, "%s", "");
    if (host == NULL || host[0] == '\0' || port == 0) {
        say(err, err_len, "a relay host and port are required");
        return NULL;
    }

#if defined(_WIN32)
    if (wsa_acquire(err, err_len) != 0) {
        return NULL;
    }
#endif

    snprintf(port_text, sizeof port_text, "%u", (unsigned)port);

    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_UNSPEC;      /* IPv4 or IPv6, whichever resolves */
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    rc = getaddrinfo(host, port_text, &hints, &res);
    if (rc != 0 || res == NULL) {
        say(err, err_len, "could not resolve %s: %s", host, gai_strerror(rc));
        goto fail;
    }

    u = calloc(1, sizeof *u);
    if (u == NULL) {
        say(err, err_len, "out of memory");
        goto fail;
    }
    u->fd = NM_INVALID_SOCKET;

    for (ai = res; ai != NULL; ai = ai->ai_next) {
        nm_socket fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd == NM_INVALID_SOCKET) {
            continue;
        }
        if (connect(fd, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
            nm_close_socket(fd);
            continue;
        }
        if (set_nonblocking(fd) != 0) {
            nm_close_socket(fd);
            continue;
        }
        u->fd = fd;
        break;
    }

    if (u->fd == NM_INVALID_SOCKET) {
        say(err, err_len, "could not open a socket to the relay at %s:%u (error %d)",
                 host, (unsigned)port, nm_socket_errno);
        goto fail;
    }

    freeaddrinfo(res);
    return u;

fail:
    if (res != NULL) {
        freeaddrinfo(res);
    }
    free(u);
#if defined(_WIN32)
    wsa_release();
#endif
    return NULL;
}

int nm_udp_send(nm_udp *u, const uint8_t *buf, size_t len)
{
    if (u == NULL || buf == NULL || len == 0) {
        return -1;
    }
#if defined(_WIN32)
    return send(u->fd, (const char *)buf, (int)len, 0) == (int)len ? 0 : -1;
#else
    /* MSG_NOSIGNAL is meaningless for UDP but harmless, and guards against a future
     * change to a stream socket raising SIGPIPE in the host process. */
    #ifdef MSG_NOSIGNAL
    return send(u->fd, buf, len, MSG_NOSIGNAL) == (ssize_t)len ? 0 : -1;
    #else
    return send(u->fd, buf, len, 0) == (ssize_t)len ? 0 : -1;
    #endif
#endif
}

void nm_udp_close(nm_udp *u)
{
    if (u == NULL) {
        return;
    }
    if (u->fd != NM_INVALID_SOCKET) {
        nm_close_socket(u->fd);
    }
    free(u);
#if defined(_WIN32)
    wsa_release();
#endif
}
