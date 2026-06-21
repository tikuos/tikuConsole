"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.ports - USB serial-port detection + platform fingerprinting.

pyserial only (no GTK), so the headless TIKUCONSOLE_SCAN dump needs no display.

SPDX-License-Identifier: Apache-2.0
"""

# Platform fingerprints: (vid, pid|None, label, default baud).  First match wins.
_USB_IDS = [
    (0x2E8A, 0x0009, "RP2350 (USB CDC)", 115200),
    (0x2E8A, None,   "RP2040/RP2350",    115200),
    (0x1366, None,   "Apollo (J-Link VCOM)", 115200),
    (0x0451, None,   "MSP430 (eZ-FET)",  9600),
    (0x0403, 0x6001, "MSP430 (FT232)",   9600),
]


def identify_port(p):
    """Map a pyserial ListPortInfo to (platform_label, default_baud)."""
    for vid, pid, name, baud in _USB_IDS:
        if p.vid == vid and (pid is None or p.pid == pid):
            return name, baud
    d = " ".join(filter(None, (p.description, getattr(p, "product", None),
                               getattr(p, "manufacturer", None)))).lower()
    if "j-link" in d or "jlink" in d or "segger" in d:
        return "Apollo (J-Link VCOM)", 115200
    if "ez-fet" in d or "msp" in d:
        return "MSP430 (eZ-FET)", 9600
    if "pico" in d or "rp2" in d:
        return "RP2 (Pico)", 115200
    return "unknown", 115200


def scan_ports():
    """USB serial ports only (drop the motherboard's legacy ttyS* clutter)."""
    try:
        from serial.tools import list_ports
        ports = [p for p in list_ports.comports()
                 if p.vid is not None or "ttyACM" in p.device
                 or "ttyUSB" in p.device]
    except Exception:
        ports = []
    return sorted(ports, key=lambda p: p.device)


def scan_and_print():
    """Headless port dump for TIKUCONSOLE_SCAN (no display).  Returns exit code."""
    found = scan_ports()
    if not found:
        print("no USB serial ports found")
    for p in found:
        plat, baud = identify_port(p)
        print("%-16s %04x:%04x  %-22s baud=%-6d %s" % (
            p.device, p.vid or 0, p.pid or 0, plat, baud, p.description or ""))
    return 0
