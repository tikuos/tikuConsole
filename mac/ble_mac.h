/*
 * TikuConsole -- macOS CoreBluetooth transport for the tikuOS wireless shell.
 *
 * Presents a board's BLE UART service (exposed by the Apollo510 Blue EVB's
 * `ble uart` command) to the GTK console as an ordinary file
 * descriptor: bytes the device notifies on the TX characteristic become
 * readable on the fd, and bytes written to the fd are forwarded to the RX
 * characteristic. The GUI can then drive a BLE link through the exact same
 * ser_fd path it uses for a serial port.
 *
 * Implemented in ble_mac.m (Objective-C + CoreBluetooth). CoreBluetooth is
 * asynchronous, so the fd is returned immediately and traffic begins once the
 * scan/connect/subscribe completes; a failed or dropped link closes the fd
 * (the reader sees EOF).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BLE_MAC_H
#define BLE_MAC_H

/*
 * Start scanning for a BLE UART peripheral whose advertised name begins with
 * @p name (NULL -> "tikuOS"), connect, and bridge it to a socket fd.
 *
 * Returns a non-blocking fd the caller reads/writes like a serial port, or -1
 * on setup failure. Call ble_mac_close() to tear the link down. Only one BLE
 * link is supported at a time.
 */
int ble_mac_open(const char *name);

/* Tear down the active BLE link and close the internal fd. Idempotent. */
void ble_mac_close(void);

/* Human-readable one-line status of the link (thread-safe snapshot), e.g.
 * "scanning", "connecting", "connected", "no radio", "not found". */
const char *ble_mac_status(void);

#endif /* BLE_MAC_H */
