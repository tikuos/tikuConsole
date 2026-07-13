/*
 * ports.c - USB serial-port detection + platform fingerprinting (macOS).
 *
 * Enumerates IOSerialBSDClient services, keeps the /dev/cu.usb* callout nodes,
 * walks up the IORegistry to read each device's USB idVendor/idProduct, and
 * fingerprints it against the same table as the Linux tcon/ports.py.
 *
 * Links against the IOKit and CoreFoundation frameworks.
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ports.h"

#include <string.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/serial/IOSerialKeys.h>

/* MACH_PORT_NULL requests the default IOKit port on every macOS version --
 * avoids choosing between kIOMainPortDefault (12+) and the now-deprecated
 * kIOMasterPortDefault, which are const globals (not macros) and so can't be
 * selected with the preprocessor. */

/* Platform fingerprints: (vid, pid|-1, label, default baud).  First match
 * wins -- mirrors tcon/ports.py's _USB_IDS. */
static const struct {
    int vid, pid;
    const char *label;
    unsigned baud;
} IDS[] = {
    {0x2E8A, 0x0009, "RP2350 (USB CDC)",     115200},
    {0x2E8A, -1,     "RP2040/RP2350",        115200},
    {0x1366, 0x1069, "nRF54L15-DK (J-Link)", 115200},
    {0x1366, -1,     "Apollo (J-Link VCOM)", 115200},
    {0x0451, -1,     "MSP430 (eZ-FET)",        9600},
    {0x0403, 0x6001, "MSP430 (FT232)",         9600},
};

static void to_lower(char *s)
{
    for (; *s; s++) {
        if (*s >= 'A' && *s <= 'Z') {
            *s = (char)(*s - 'A' + 'a');
        }
    }
}

/* Map (vid, pid, product-name) to a platform label + default baud. */
static void fingerprint(int vid, int pid, const char *name,
                        char *label, size_t lsz, unsigned *baud)
{
    for (size_t i = 0; i < sizeof(IDS) / sizeof(IDS[0]); i++) {
        if (vid == IDS[i].vid && (IDS[i].pid < 0 || pid == IDS[i].pid)) {
            strlcpy(label, IDS[i].label, lsz);
            *baud = IDS[i].baud;
            return;
        }
    }
    /* Name-based fallback (matches the Python's description heuristic). */
    char d[160];
    strlcpy(d, name ? name : "", sizeof(d));
    to_lower(d);
    if (strstr(d, "j-link") || strstr(d, "jlink") || strstr(d, "segger")) {
        strlcpy(label, "Apollo (J-Link VCOM)", lsz); *baud = 115200; return;
    }
    if (strstr(d, "ez-fet") || strstr(d, "msp")) {
        strlcpy(label, "MSP430 (eZ-FET)", lsz); *baud = 9600; return;
    }
    if (strstr(d, "pico") || strstr(d, "rp2")) {
        strlcpy(label, "RP2 (Pico)", lsz); *baud = 115200; return;
    }
    strlcpy(label, "unknown", lsz);
    *baud = 115200;
}

/* Read an integer IORegistry property from a node (or any ancestor). */
static int read_int_prop(io_registry_entry_t node, CFStringRef key, int *out)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(node, key,
                                                  kCFAllocatorDefault, 0);
    if (!v) {
        return 0;
    }
    int ok = 0;
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        ok = CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, out);
    }
    CFRelease(v);
    return ok;
}

static void read_string_prop(io_registry_entry_t node, CFStringRef key,
                             char *out, size_t osz)
{
    CFTypeRef value = IORegistryEntryCreateCFProperty(
        node, key, kCFAllocatorDefault, 0);
    if (value) {
        if (CFGetTypeID(value) == CFStringGetTypeID()) {
            CFStringGetCString((CFStringRef)value, out, (CFIndex)osz,
                               kCFStringEncodingUTF8);
        }
        CFRelease(value);
    }
}

/* Walk up the IOService tree from a serial node, filling vid/pid/product from
 * the first USB device ancestor that carries them. */
static void usb_ancestry(io_registry_entry_t svc, int *vid, int *pid,
                         char *prod, size_t psz, char *serial, size_t ssz)
{
    *vid = *pid = -1;
    prod[0] = 0;
    serial[0] = 0;
    io_registry_entry_t node = svc;
    IOObjectRetain(node);
    for (int depth = 0; depth < 8 && node; depth++) {
        if (read_int_prop(node, CFSTR("idVendor"), vid) &&
            read_int_prop(node, CFSTR("idProduct"), pid)) {
            read_string_prop(node, CFSTR("USB Product Name"), prod, psz);
            read_string_prop(node, CFSTR("USB Serial Number"), serial, ssz);
            IOObjectRelease(node);
            return;
        }
        io_registry_entry_t parent;
        if (IORegistryEntryGetParentEntry(node, kIOServicePlane, &parent)
                != KERN_SUCCESS) {
            break;
        }
        IOObjectRelease(node);
        node = parent;
    }
    if (node) {
        IOObjectRelease(node);
    }
}

int ports_scan(port_info_t *out, int max)
{
    CFMutableDictionaryRef match = IOServiceMatching(kIOSerialBSDServiceValue);
    if (!match) {
        return 0;
    }
    CFDictionarySetValue(match, CFSTR(kIOSerialBSDTypeKey),
                         CFSTR(kIOSerialBSDAllTypes));

    io_iterator_t it;
    if (IOServiceGetMatchingServices(MACH_PORT_NULL, match, &it)
            != KERN_SUCCESS) {
        return 0;
    }

    int count = 0;
    io_object_t svc;
    while ((svc = IOIteratorNext(it)) != 0 && count < max) {
        char dev[256] = {0};
        CFTypeRef path = IORegistryEntryCreateCFProperty(
            svc, CFSTR(kIOCalloutDeviceKey), kCFAllocatorDefault, 0);
        if (path) {
            if (CFGetTypeID(path) == CFStringGetTypeID()) {
                CFStringGetCString((CFStringRef)path, dev, sizeof(dev),
                                   kCFStringEncodingUTF8);
            }
            CFRelease(path);
        }
        /* Keep only USB serial callout nodes; skip Bluetooth / debug consoles. */
        if (strstr(dev, "/cu.usbmodem") || strstr(dev, "/cu.usbserial")) {
            int vid, pid;
            char prod[128], serial[128];
            usb_ancestry(svc, &vid, &pid, prod, sizeof(prod),
                         serial, sizeof(serial));
            port_info_t *pi = &out[count++];
            strlcpy(pi->device, dev, sizeof(pi->device));
            pi->vid = vid;
            pi->pid = pid;
            strlcpy(pi->serial, serial, sizeof(pi->serial));
            fingerprint(vid, pid, prod, pi->label, sizeof(pi->label), &pi->baud);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);

    /* Recognised boards first, then alphabetical by device (insertion sort). */
    for (int i = 1; i < count; i++) {
        port_info_t key = out[i];
        int key_known = strcmp(key.label, "unknown") != 0;
        int j = i - 1;
        while (j >= 0) {
            int j_known = strcmp(out[j].label, "unknown") != 0;
            int swap = (!j_known && key_known) ||
                       (j_known == key_known &&
                        strcmp(out[j].device, key.device) > 0);
            if (!swap) {
                break;
            }
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = key;
    }
    return count;
}

#ifdef PORTS_TEST
#include <stdio.h>
int main(void)
{
    port_info_t p[PORTS_MAX];
    int n = ports_scan(p, PORTS_MAX);
    if (n == 0) {
        printf("no USB serial ports found\n");
    }
    for (int i = 0; i < n; i++) {
        printf("%-26s %04x:%04x  %-22s baud=%u\n",
               p[i].device, p[i].vid < 0 ? 0 : p[i].vid,
               p[i].pid < 0 ? 0 : p[i].pid, p[i].label, p[i].baud);
    }
    return 0;
}
#endif
