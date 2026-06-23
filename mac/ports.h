/*
 * ports.h - USB serial-port detection + platform fingerprinting (macOS).
 *
 * The macOS/IOKit twin of tcon/ports.py: enumerate the /dev/cu.usb* devices,
 * read each one's USB vendor/product id from the IORegistry, and map it to a
 * TikuOS platform label + default baud.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef TIKUCONSOLE_PORTS_H
#define TIKUCONSOLE_PORTS_H

#include <stddef.h>

#define PORTS_MAX 16

typedef struct {
    char     device[256]; /* /dev/cu.usbmodemXXXX */
    char     label[64];   /* platform fingerprint, e.g. "Apollo (J-Link VCOM)" */
    unsigned baud;        /* default baud for this platform */
    int      vid, pid;    /* USB ids, or -1 if unknown */
} port_info_t;

/* Scan USB serial ports; fill out[] (up to max entries) and return the count.
 * Recognised boards (Apollo / MSP430 / RP2350) sort ahead of unknown ones. */
int ports_scan(port_info_t *out, int max);

#endif /* TIKUCONSOLE_PORTS_H */
