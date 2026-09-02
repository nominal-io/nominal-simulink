/*
 * nm_udp.h — the proxy-mode transport.
 *
 * A connected UDP socket and non-blocking sends. That is the whole point of proxy mode:
 * where nm_http pays for protobuf, TLS and an HTTP round trip, this pays for a memcpy
 * into a datagram and one syscall.
 *
 * Sends are non-blocking so a full socket buffer costs a dropped frame rather than a
 * stalled sender thread. UDP has no retransmission here by design — buffering for
 * retransmission would reintroduce exactly the cost proxy mode exists to avoid. Loss is
 * counted, and the relay detects gaps from the sequence numbers.
 */

#ifndef NM_UDP_H
#define NM_UDP_H

#include <stdint.h>
#include <stddef.h>

typedef struct nm_udp nm_udp;

/*
 * Resolve `host` and connect a datagram socket to it. Connecting rather than using
 * sendto with an address each time lets the kernel cache the route and lets us see
 * ICMP port-unreachable as an error on send.
 *
 * Returns NULL on failure. Performs no I/O beyond name resolution.
 */
nm_udp *nm_udp_open(const char *host, uint16_t port, char *err, size_t err_len);

/*
 * Send one datagram. Returns 0 on success, or -1 if it could not be sent — a full
 * socket buffer, or the relay's port being closed. Never blocks.
 */
int nm_udp_send(nm_udp *u, const uint8_t *buf, size_t len);

void nm_udp_close(nm_udp *u);

#endif /* NM_UDP_H */
