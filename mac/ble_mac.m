/*
 * TikuConsole -- macOS CoreBluetooth transport for the tikuOS wireless shell.
 * See ble_mac.h. Bridges a board's BLE UART service to a socket fd so the
 * GTK console drives it exactly like a serial port.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ble_mac.h"

/* BLE UART service UUIDs -- the well-known Nordic UART Service UUIDs, so any
 * stock BLE-serial app interoperates. Must match arch/ambiq/tiku_ble_uart.c. */
static NSString *const UART_SVC = @"6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static NSString *const UART_RX  = @"6E400002-B5A3-F393-E0A9-E50E24DCCA9E"; /* -> device */
static NSString *const UART_TX  = @"6E400003-B5A3-F393-E0A9-E50E24DCCA9E"; /* <- device */

enum { ST_IDLE, ST_SCAN, ST_CONNECTING, ST_CONNECTED,
       ST_NORADIO, ST_NOTFOUND, ST_DISC };
static const char *const STATUS[] = {
    "idle", "scanning", "connecting", "connected",
    "no radio", "not found", "disconnected"
};
static volatile int g_status = ST_IDLE;

/* Keep each RX write inside one ATT MTU worth of payload. */
#define BLE_WRITE_CHUNK 180


@interface BleUart : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
@property (strong) CBCentralManager  *central;
@property (strong) CBPeripheral      *peer;
@property (strong) CBCharacteristic  *rx;
@property (strong) CBCharacteristic  *tx;
@property (copy)   NSString          *want;        /* lowercase name prefix */
@property (assign) int                ifd;         /* internal socketpair end */
@property (strong) dispatch_queue_t   q;
@property (strong) dispatch_source_t  rxSource;
@end

/* The single active link (one board at a time). */
static BleUart *g_ble;


@implementation BleUart

/* Radio state -> begin scanning, or fail out. */
- (void)centralManagerDidUpdateState:(CBCentralManager *)c {
    if (c.state == CBManagerStatePoweredOn) {
        g_status = ST_SCAN;
        /* The device advertises only its name (no 128-bit service UUID), so we
         * must scan for everything and match by name. */
        [c scanForPeripheralsWithServices:nil options:nil];
    } else {
        g_status = ST_NORADIO;
        [self teardown];
    }
}

- (void)centralManager:(CBCentralManager *)c
 didDiscoverPeripheral:(CBPeripheral *)p
     advertisementData:(NSDictionary<NSString *, id> *)adv
                  RSSI:(NSNumber *)rssi {
    NSString *nm = p.name ?: adv[CBAdvertisementDataLocalNameKey];
    BOOL match = (nm && [[nm lowercaseString] hasPrefix:self.want]);
    for (CBUUID *u in adv[CBAdvertisementDataServiceUUIDsKey]) {
        if ([u isEqual:[CBUUID UUIDWithString:UART_SVC]]) { match = YES; }
    }
    if (!match) { return; }
    [c stopScan];
    self.peer = p;
    p.delegate = self;
    g_status = ST_CONNECTING;
    [c connectPeripheral:p options:nil];
}

- (void)centralManager:(CBCentralManager *)c
  didConnectPeripheral:(CBPeripheral *)p {
    [p discoverServices:@[[CBUUID UUIDWithString:UART_SVC]]];
}

- (void)peripheral:(CBPeripheral *)p didDiscoverServices:(NSError *)err {
    for (CBService *s in p.services) {
        [p discoverCharacteristics:@[[CBUUID UUIDWithString:UART_RX],
                                     [CBUUID UUIDWithString:UART_TX]]
                        forService:s];
    }
}

- (void)peripheral:(CBPeripheral *)p
didDiscoverCharacteristicsForService:(CBService *)s
             error:(NSError *)err {
    for (CBCharacteristic *ch in s.characteristics) {
        if ([ch.UUID isEqual:[CBUUID UUIDWithString:UART_RX]]) {
            self.rx = ch;
        } else if ([ch.UUID isEqual:[CBUUID UUIDWithString:UART_TX]]) {
            self.tx = ch;
            [p setNotifyValue:YES forCharacteristic:ch];   /* subscribe */
        }
    }
    if (self.rx && self.tx) {
        g_status = ST_CONNECTED;
        [self startRxForwarding];
    }
}

/* Device -> host: notification bytes become readable on the caller's fd. */
- (void)peripheral:(CBPeripheral *)p
didUpdateValueForCharacteristic:(CBCharacteristic *)ch
             error:(NSError *)err {
    if (ch != self.tx || self.ifd < 0) { return; }
    NSData *d = ch.value;
    const uint8_t *b = d.bytes;
    size_t off = 0, n = d.length;
    while (off < n) {
        ssize_t w = write(self.ifd, b + off, n - off);
        if (w > 0) { off += (size_t)w; }
        else { break; }                 /* buffer full: drop (console keeps up) */
    }
}

- (void)centralManager:(CBCentralManager *)c
didDisconnectPeripheral:(CBPeripheral *)p error:(NSError *)err {
    g_status = ST_DISC;
    [self teardown];
}

- (void)centralManager:(CBCentralManager *)c
didFailToConnectPeripheral:(CBPeripheral *)p error:(NSError *)err {
    g_status = ST_NOTFOUND;
    [self teardown];
}

/* host -> device: forward everything written to the fd onto the RX char. */
- (void)startRxForwarding {
    self.rxSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,
                                           (uintptr_t)self.ifd, 0, self.q);
    __weak BleUart *weak = self;
    dispatch_source_set_event_handler(self.rxSource, ^{
        BleUart *self2 = weak;
        if (!self2) { return; }
        uint8_t buf[512];
        ssize_t nr = read(self2.ifd, buf, sizeof(buf));
        if (nr == 0) { [self2 teardown]; return; }         /* peer closed fd */
        if (nr < 0) { if (errno != EAGAIN) { [self2 teardown]; } return; }
        for (ssize_t off = 0; off < nr; ) {
            ssize_t chunk = nr - off;
            if (chunk > BLE_WRITE_CHUNK) { chunk = BLE_WRITE_CHUNK; }
            [self2.peer writeValue:[NSData dataWithBytes:buf + off length:chunk]
                 forCharacteristic:self2.rx
                              type:CBCharacteristicWriteWithoutResponse];
            off += chunk;
        }
    });
    dispatch_resume(self.rxSource);
}

- (void)teardown {
    if (self.rxSource) {
        dispatch_source_cancel(self.rxSource);
        self.rxSource = nil;
    }
    if (self.peer && self.central) {
        [self.central cancelPeripheralConnection:self.peer];
    }
    if (self.ifd >= 0) {
        close(self.ifd);          /* EOF on the caller's end -> GUI disconnects */
        self.ifd = -1;
    }
}
@end


/* ---- C API ---- */

int ble_mac_open(const char *name) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return -1;
    }
    fcntl(sv[0], F_SETFL, O_NONBLOCK);          /* caller (GUI) end */
    fcntl(sv[1], F_SETFL, O_NONBLOCK);          /* internal end */

    g_status = ST_IDLE;
    BleUart *b = [BleUart new];
    b.want = (name && *name) ? [[NSString stringWithUTF8String:name] lowercaseString]
                             : @"tikuos";
    b.ifd = sv[1];
    b.q = dispatch_queue_create("org.tiku.ble", DISPATCH_QUEUE_SERIAL);
    b.central = [[CBCentralManager alloc] initWithDelegate:b queue:b.q];
    g_ble = b;
    return sv[0];
}

void ble_mac_close(void) {
    BleUart *b = g_ble;
    g_ble = nil;
    if (!b) { return; }
    dispatch_async(b.q, ^{ [b teardown]; });
}

const char *ble_mac_status(void) {
    int s = g_status;
    if (s < 0 || s >= (int)(sizeof(STATUS) / sizeof(STATUS[0]))) {
        return "?";
    }
    return STATUS[s];
}


/* ---- standalone smoke test: `make ble_test && ./ble_test [name]` ---- */
#ifdef BLE_MAC_TEST
#include <stdio.h>
#include <time.h>

static double now_s(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    const char *name = (argc > 1) ? argv[1] : "tikuOS";
    int fd = ble_mac_open(name);
    if (fd < 0) { fprintf(stderr, "ble_mac_open failed\n"); return 1; }

    fprintf(stderr, "connecting to \"%s\" ...\n", name);
    double t0 = now_s();
    int last = -1;
    while (now_s() - t0 < 15.0 && g_status != ST_CONNECTED) {
        if (g_status != last) { fprintf(stderr, "  [%s]\n", ble_mac_status()); last = g_status; }
        usleep(50 * 1000);
        if (g_status == ST_NORADIO || g_status == ST_NOTFOUND) break;
    }
    if (g_status != ST_CONNECTED) {
        fprintf(stderr, "not connected (%s)\n", ble_mac_status());
        ble_mac_close(); return 2;
    }
    fprintf(stderr, "connected -- sending 'info'\n");
    usleep(700 * 1000);                          /* let banner/prompt land */
    (void)!write(fd, "info\r", 5);

    double t1 = now_s();
    while (now_s() - t1 < 4.0) {
        uint8_t buf[256];
        ssize_t nr = read(fd, buf, sizeof(buf));
        if (nr > 0) { (void)!write(STDOUT_FILENO, buf, nr); }
        else { usleep(20 * 1000); }
    }
    fprintf(stderr, "\n[done]\n");
    ble_mac_close();
    return 0;
}
#endif
