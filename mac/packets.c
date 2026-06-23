/*
 * packets.c - ICMP/UDP/IPv4 build + parse.  See packets.h.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "packets.h"

#include <string.h>

uint16_t inet_checksum(const uint8_t *data, size_t len)
{
    uint32_t s = 0;
    size_t i = 0;
    for (; i + 1 < len; i += 2) {
        s += ((uint32_t)data[i] << 8) | data[i + 1];
    }
    if (i < len) {                      /* odd trailing byte, padded with 0 */
        s += (uint32_t)data[i] << 8;
    }
    while (s >> 16) {
        s = (s >> 16) + (s & 0xffff);
    }
    return (uint16_t)(~s & 0xffff);
}

size_t build_icmp_echo(uint16_t ident, uint16_t seq,
                       const uint8_t *payload, size_t plen, uint8_t *out)
{
    out[0] = 8;                         /* type: echo request */
    out[1] = 0;                         /* code */
    out[2] = out[3] = 0;                /* checksum (filled below) */
    out[4] = (uint8_t)(ident >> 8);
    out[5] = (uint8_t)(ident & 0xff);
    out[6] = (uint8_t)(seq >> 8);
    out[7] = (uint8_t)(seq & 0xff);
    if (plen) {
        memcpy(out + 8, payload, plen);
    }
    uint16_t cks = inet_checksum(out, 8 + plen);
    out[2] = (uint8_t)(cks >> 8);
    out[3] = (uint8_t)(cks & 0xff);
    return 8 + plen;
}

size_t build_ip(const uint8_t src[4], const uint8_t dst[4], uint8_t proto,
                const uint8_t *payload, size_t plen, uint16_t ident,
                uint8_t *out)
{
    uint16_t total = (uint16_t)(20 + plen);
    out[0] = 0x45;                      /* version 4, IHL 5 */
    out[1] = 0;                         /* DSCP/ECN */
    out[2] = (uint8_t)(total >> 8);
    out[3] = (uint8_t)(total & 0xff);
    out[4] = (uint8_t)(ident >> 8);
    out[5] = (uint8_t)(ident & 0xff);
    out[6] = 0;                         /* flags/frag */
    out[7] = 0;
    out[8] = 64;                        /* TTL */
    out[9] = proto;
    out[10] = out[11] = 0;              /* header checksum (filled below) */
    memcpy(out + 12, src, 4);
    memcpy(out + 16, dst, 4);
    uint16_t cks = inet_checksum(out, 20);
    out[10] = (uint8_t)(cks >> 8);
    out[11] = (uint8_t)(cks & 0xff);
    if (plen) {
        memcpy(out + 20, payload, plen);
    }
    return total;
}

size_t build_udp(const uint8_t src_ip[4], uint16_t src_port,
                 const uint8_t dst_ip[4], uint16_t dst_port,
                 const uint8_t *payload, size_t plen, uint8_t *out)
{
    uint16_t length = (uint16_t)(8 + plen);
    out[0] = (uint8_t)(src_port >> 8);
    out[1] = (uint8_t)(src_port & 0xff);
    out[2] = (uint8_t)(dst_port >> 8);
    out[3] = (uint8_t)(dst_port & 0xff);
    out[4] = (uint8_t)(length >> 8);
    out[5] = (uint8_t)(length & 0xff);
    out[6] = out[7] = 0;                /* checksum (filled below) */
    if (plen) {
        memcpy(out + 8, payload, plen);
    }
    /* Checksum over the pseudo-header + UDP header + payload, summed in place. */
    uint32_t s = 0;
    s += ((uint32_t)src_ip[0] << 8) | src_ip[1];
    s += ((uint32_t)src_ip[2] << 8) | src_ip[3];
    s += ((uint32_t)dst_ip[0] << 8) | dst_ip[1];
    s += ((uint32_t)dst_ip[2] << 8) | dst_ip[3];
    s += 17;                            /* protocol */
    s += length;
    for (size_t i = 0; i + 1 < length; i += 2) {
        s += ((uint32_t)out[i] << 8) | out[i + 1];
    }
    if (length & 1) {
        s += (uint32_t)out[length - 1] << 8;
    }
    while (s >> 16) {
        s = (s >> 16) + (s & 0xffff);
    }
    uint16_t cks = (uint16_t)(~s & 0xffff);
    if (cks == 0) {                     /* RFC 768: a computed 0 is sent as ~0 */
        cks = 0xffff;
    }
    out[6] = (uint8_t)(cks >> 8);
    out[7] = (uint8_t)(cks & 0xff);
    return length;
}

int parse_icmp_echo_reply(const uint8_t *pkt, size_t len, uint16_t ident)
{
    if (len < 28 || (pkt[0] >> 4) != 4 || pkt[9] != 1) {
        return -1;                      /* short / not IPv4 / not ICMP */
    }
    size_t ihl = (size_t)(pkt[0] & 0x0f) * 4;
    if (len < ihl + 8) {
        return -1;
    }
    const uint8_t *icmp = pkt + ihl;
    if (icmp[0] != 0) {                 /* type 0 = echo reply */
        return -1;
    }
    uint16_t rid = ((uint16_t)icmp[4] << 8) | icmp[5];
    uint16_t rseq = ((uint16_t)icmp[6] << 8) | icmp[7];
    return (rid == ident) ? (int)rseq : -1;
}

#ifdef PACKETS_TEST
#include <stdio.h>
int main(void)
{
    uint8_t src[4] = {172, 16, 7, 1}, dst[4] = {172, 16, 7, 2};
    uint8_t icmp[64], ip[128];
    size_t il = build_icmp_echo(0x4242, 7, (const uint8_t *)"tikuconsole", 11,
                                icmp);
    size_t pl = build_ip(src, dst, 1, icmp, il, 1, ip);
    printf("icmp=%zu ip=%zu  icmp_cks=%02x%02x ip_cks=%02x%02x\n",
           il, pl, icmp[2], icmp[3], ip[10], ip[11]);

    /* Verify IP + ICMP checksums are self-consistent (sum == 0). */
    int ip_ok = (inet_checksum(ip, 20) == 0);
    int icmp_ok = (inet_checksum(icmp, il) == 0);

    /* Flip the request into a reply (type 8 -> 0, refresh cks) and parse it. */
    ip[20] = 0;
    ip[22] = ip[23] = 0;
    uint16_t c = inet_checksum(ip + 20, il);
    ip[22] = (uint8_t)(c >> 8);
    ip[23] = (uint8_t)(c & 0xff);
    int seq = parse_icmp_echo_reply(ip, pl, 0x4242);

    /* UDP build + self-consistent checksum. */
    uint8_t udp[64];
    size_t ul = build_udp(src, 5000, dst, 53, (const uint8_t *)"hi", 2, udp);

    printf("ip_cks_ok=%d icmp_cks_ok=%d parsed_seq=%d(want 7) udp_len=%zu\n",
           ip_ok, icmp_ok, seq, ul);
    return (ip_ok && icmp_ok && seq == 7 && ul == 10) ? 0 : 1;
}
#endif
