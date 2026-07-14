"""Pure firmware build-option policy shared by the TikuConsole GTK frontend.

This module deliberately has no GTK dependency.  The UI collects choices; this
module masks choices that a board cannot support and produces the exact make
variables.  Keeping that policy pure makes command construction testable
without a display or attached hardware.

SPDX-License-Identifier: Apache-2.0
"""

from dataclasses import dataclass, replace
import os


@dataclass(frozen=True)
class BuildFeatures:
    shell: bool = True
    networking: bool = True
    wifi: bool = False
    basic: bool = False
    color: bool = True
    usb: bool = False
    web: bool = False
    bluetooth: bool = False
    pkhw: bool = False
    flpr: bool = False


def feature_support(board):
    """Return the optional build features supported by *board*."""
    is_rp = board.family == "rp2350"
    is_nordic = board.family == "nordic"
    is_blue = board.key == "apollo510b"
    return {
        "wifi": is_rp,
        "usb": is_rp,
        "web": board.family in ("rp2350", "ambiq", "nordic"),
        "bluetooth": is_rp or is_blue,
        "pkhw": is_nordic,
        "flpr": is_nordic,
    }


def normalize_features(board, features):
    """Clear stale UI choices that are unsupported on the selected board."""
    support = feature_support(board)
    return replace(features, **{
        name: bool(getattr(features, name) and supported)
        for name, supported in support.items()
    })


def _add(flags, value):
    if value not in flags:
        flags.append(value)


def make_flags(board, features, baud):
    """Translate a board and UI choices into stable make argv elements."""
    f = normalize_features(board, features)
    is_rp = board.family == "rp2350"
    is_nordic = board.family == "nordic"
    is_blue = board.key == "apollo510b"

    # Every network/radio/coprocessor profile is operated through the shell.
    # Force it on when a dependent feature is selected rather than producing a
    # firmware image the console cannot actually control.
    shell = (f.shell or f.networking or f.wifi or f.web or f.bluetooth
             or f.flpr)
    basic = f.basic or f.web
    flags = [
        "HAS_TESTS=0",
        "HAS_EXAMPLES=0",
        "TIKU_SHELL_ENABLE=%d" % int(shell),
        "TIKU_SHELL_BASIC_ENABLE=%d" % int(basic),
        "TIKU_SHELL_COLOR=%d" % int(f.color),
    ]
    extra = []

    if f.web:
        for value in (
                "TIKU_KIT_NET_ENABLE=1", "TIKU_KIT_NET_MIN=1",
                "TIKU_KITS_NET_DNS_ENABLE=1",
                "TIKU_KITS_NET_HTTP_ENABLE=1",
                "TIKU_KIT_CRYPTO_ENABLE=1", "HAS_TLS=1",
                "TIKU_KIT_TIME_ENABLE=1"):
            _add(flags, value)
        if f.wifi:
            for value in ("TIKU_DRV_WIFI_CYW43_ENABLE=1",
                          "TIKU_KITS_NET_WIFI_ENABLE=1",
                          "TIKU_KITS_NET_DHCP_ENABLE=1"):
                _add(flags, value)
        if is_nordic:
            # Match TikuBench's proven Nordic HTTPS profile: keep the network
            # pump alive while expensive certificate work runs on a worker.
            _add(flags, "TIKU_THREADS_ENABLE=1")
    elif f.wifi:
        for value in ("TIKU_DRV_WIFI_CYW43_ENABLE=1",
                      "TIKU_KITS_NET_WIFI_ENABLE=1",
                      "TIKU_KIT_NET_ENABLE=1", "TIKU_KIT_NET_MIN=1",
                      "TIKU_KITS_NET_DHCP_ENABLE=1",
                      "TIKU_KITS_NET_DNS_ENABLE=1",
                      "TIKU_KIT_TIME_ENABLE=1"):
            _add(flags, value)
        extra += ["-DTIKU_KITS_NET_TCP_ENABLE=0",
                  "-DTIKU_SHELL_CMD_SYSLOG=0",
                  "-DTIKU_SHELL_CMD_MQTT=0",
                  "-DTIKU_SHELL_CMD_COAP=0",
                  "-DTIKU_SHELL_CMD_TFTP=0"]
    elif f.networking:
        _add(flags, "TIKU_KIT_NET_ENABLE=1")
        _add(flags, "TIKU_SHELL_NET_TEST=1")

    if f.bluetooth:
        if is_rp:
            _add(flags, "TIKU_DRV_WIFI_CYW43_ENABLE=1")
            _add(flags, "TIKU_DRV_WIFI_CYW43_BT_ENABLE=1")
        elif is_blue:
            _add(flags, "TIKU_DRV_BLE_EM9305_ENABLE=1")
    if f.pkhw:
        _add(flags, "TIKU_CRACEN_PK_ENABLE=1")
        _add(flags, "TIKU_KIT_CRYPTO_ENABLE=1")
    if f.flpr:
        _add(flags, "TIKU_FLPR_ENABLE=1")
    if basic and board.family == "msp430":
        _add(flags, "MEMORY_MODEL=large")
    if is_rp and f.usb:
        _add(flags, "TIKU_CONSOLE=usb")

    flags += ["MCU=%s" % board.mcu, "UART_BAUD=%d" % int(baud)]
    if extra:
        flags.append("EXTRA_CFLAGS=%s" % " ".join(extra))
    return flags


def profile_name(board, features):
    """Human-readable effective profile, including implicit dependencies."""
    f = normalize_features(board, features)
    shell = (f.shell or f.networking or f.wifi or f.web or f.bluetooth
             or f.flpr)
    names = []
    if shell:
        names.append("shell")
    if f.web:
        names.append("web")
    elif f.wifi:
        names.append("wifi")
    elif f.networking:
        names.append("net")
    if f.bluetooth:
        names.append("bt" if board.family == "rp2350" else "ble")
    if f.flpr:
        names.append("flpr")
    if f.pkhw:
        names.append("pk-hw")
    if f.basic or f.web:
        names.append("BASIC")
    if f.color:
        names.append("colour")
    if f.usb:
        names.append("usb")
    return "+".join(names) or "bare"


def board_key_for_platform(label):
    """Map a USB fingerprint label to TikuBench's default board key."""
    platform = (label or "").lower()
    if "nrf54" in platform or "nordic" in platform:
        # The nRF54L15-DK and nRF54LM20-DK share one J-Link USB id, so USB
        # alone can't tell them apart -- default to the L15 and let the user
        # pass --board nrf54lm20a for the LM20-DK.
        return "nrf54l15"
    if "apollo" in platform:
        return "apollo510"
    if "rp2" in platform or "pico" in platform:
        return "rp2350"
    if "msp" in platform:
        return "msp430fr5994"
    return None


def invoker_prefix(environ=None, euid=None):
    """Run make as the sudo invoker so build output never becomes root-owned."""
    env = os.environ if environ is None else environ
    if euid is None:
        euid = os.geteuid() if hasattr(os, "geteuid") else -1
    user = env.get("SUDO_USER")
    return ["sudo", "-u", user, "-H"] if user and euid == 0 else []
