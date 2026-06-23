/*
 * packets.h - ICMP/UDP/IPv4 build + parse for the in-app pinger and UDP relay.
 *
 * The C twin of tcon/packets.py: pure functions (no GTK, no sockets) so the
 * board's own net stack answers echo requests with no TUN, no root, and no
 * system `ping`.  All addresses are 4-byte network-order (as from inet_aton).
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKUCONSOLE_PACKETS_H
#define TIKUCONSOLE_PACKETS_H

#include <stddef.h>
#include <stdint.h>

/* 16-bit one's-complement Internet checksum (RFC 1071). */
uint16_t inet_checksum(const uint8_t *data, size_t len);

/* ICMP echo request (type 8, code 0) with checksum.  out >= 8 + plen. */
size_t build_icmp_echo(uint16_t ident, uint16_t seq,
                       const uint8_t *payload, size_t plen, uint8_t *out);

/* Minimal 20-byte IPv4 header (no options) + payload.  out >= 20 + plen. */
size_t build_ip(const uint8_t src[4], const uint8_t dst[4], uint8_t proto,
                const uint8_t *payload, size_t plen, uint16_t ident,
                uint8_t *out);

/* UDP datagram (8-byte header + payload) with checksum.  out >= 8 + plen. */
size_t build_udp(const uint8_t src_ip[4], uint16_t src_port,
                 const uint8_t dst_ip[4], uint16_t dst_port,
                 const uint8_t *payload, size_t plen, uint8_t *out);

/* seq (>= 0) if pkt is an ICMP echo reply (type 0) for ident, else -1. */
int parse_icmp_echo_reply(const uint8_t *pkt, size_t len, uint16_t ident);

#endif /* TIKUCONSOLE_PACKETS_H */
