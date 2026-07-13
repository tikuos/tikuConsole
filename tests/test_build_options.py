import re
import sys
from pathlib import Path
from types import SimpleNamespace
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tcon.build_options import (BuildFeatures, board_key_for_platform,
                                feature_support, invoker_prefix, make_flags,
                                profile_name)
from tcon.ports import identify_port


def board(key, family, baud=115200):
    return SimpleNamespace(key=key, mcu=key, family=family,
                           default_baud=baud)


class BuildOptionTests(unittest.TestCase):
    def test_nordic_https_matches_worker_profile(self):
        b = board("nrf54l15", "nordic")
        f = BuildFeatures(shell=False, networking=False, web=True,
                          pkhw=True, flpr=True)
        flags = make_flags(b, f, 115200)
        for expected in ("TIKU_SHELL_ENABLE=1",
                         "TIKU_SHELL_BASIC_ENABLE=1",
                         "TIKU_THREADS_ENABLE=1",
                         "TIKU_CRACEN_PK_ENABLE=1",
                         "TIKU_KIT_CRYPTO_ENABLE=1",
                         "TIKU_FLPR_ENABLE=1",
                         "MCU=nrf54l15", "UART_BAUD=115200"):
            self.assertIn(expected, flags)
        self.assertEqual(len(flags), len(set(flags)))
        self.assertEqual(profile_name(b, f),
                         "shell+web+flpr+pk-hw+BASIC+colour")

    def test_rp2350_wifi_bluetooth_usb_profile(self):
        b = board("rp2350", "rp2350")
        f = BuildFeatures(shell=False, networking=True, wifi=True, usb=True,
                          bluetooth=True)
        flags = make_flags(b, f, 460800)
        for expected in ("TIKU_SHELL_ENABLE=1",
                         "TIKU_DRV_WIFI_CYW43_ENABLE=1",
                         "TIKU_DRV_WIFI_CYW43_BT_ENABLE=1",
                         "TIKU_KITS_NET_WIFI_ENABLE=1",
                         "TIKU_CONSOLE=usb", "UART_BAUD=460800"):
            self.assertIn(expected, flags)
        self.assertNotIn("TIKU_SHELL_NET_TEST=1", flags)
        extra = next(v for v in flags if v.startswith("EXTRA_CFLAGS="))
        self.assertIn("-DTIKU_KITS_NET_TCP_ENABLE=0", extra)
        self.assertEqual(len(flags), len(set(flags)))

    def test_apollo_blue_ble_forces_shell(self):
        b = board("apollo510b", "ambiq")
        f = BuildFeatures(shell=False, networking=False, bluetooth=True,
                          color=False)
        flags = make_flags(b, f, 115200)
        self.assertIn("TIKU_DRV_BLE_EM9305_ENABLE=1", flags)
        self.assertIn("TIKU_SHELL_ENABLE=1", flags)
        self.assertEqual(profile_name(b, f), "shell+ble")

    def test_unsupported_choices_are_removed(self):
        b = board("msp430fr5994", "msp430", 9600)
        f = BuildFeatures(networking=False, wifi=True, usb=True, web=True,
                          bluetooth=True, pkhw=True, flpr=True, basic=True)
        flags = make_flags(b, f, 9600)
        joined = " ".join(flags)
        for forbidden in ("CYW43", "HAS_TLS", "CRACEN", "FLPR", "TIKU_CONSOLE"):
            self.assertNotIn(forbidden, joined)
        self.assertIn("MEMORY_MODEL=large", flags)
        support = feature_support(b)
        self.assertFalse(any(support.values()))

    def test_platform_to_board_mapping_includes_nordic(self):
        self.assertEqual(board_key_for_platform("nRF54L15-DK (J-Link)"),
                         "nrf54l15")
        self.assertEqual(board_key_for_platform("Apollo (J-Link VCOM)"),
                         "apollo510")
        self.assertEqual(board_key_for_platform("RP2350 (USB CDC)"), "rp2350")
        self.assertEqual(board_key_for_platform("MSP430 (FT232)"),
                         "msp430fr5994")
        self.assertIsNone(board_key_for_platform("unknown"))

    def test_sudo_builds_are_demoted_to_invoker(self):
        env = {"SUDO_USER": "ambuj"}
        self.assertEqual(invoker_prefix(env, 0),
                         ["sudo", "-u", "ambuj", "-H"])
        self.assertEqual(invoker_prefix(env, 1000), [])
        self.assertEqual(invoker_prefix({}, 0), [])

    def test_nordic_usb_fingerprint_precedes_apollo_wildcard(self):
        p = SimpleNamespace(vid=0x1366, pid=0x1069, description="J-Link",
                            product="J-Link", manufacturer="SEGGER")
        self.assertEqual(identify_port(p),
                         ("nRF54L15-DK (J-Link)", 115200))


class FrontendParityTests(unittest.TestCase):
    def test_macos_board_table_matches_tikubench(self):
        tb = ROOT.parent / "TikuBench"
        sys.path.insert(0, str(tb))
        try:
            from tikubench.core.board import BOARDS
        finally:
            sys.path.pop(0)
        source = (ROOT / "mac" / "gui_build.c").read_text()
        table = source.split("static const board_t BOARDS[] = {", 1)[1]
        table = table.split("};", 1)[0]
        mac_keys = set(re.findall(r'^\s*\{"([^"]+)"', table, re.MULTILINE))
        self.assertEqual(mac_keys, set(BOARDS))

    def test_macos_has_current_specialized_flags(self):
        source = (ROOT / "mac" / "gui_build.c").read_text()
        for flag in ("TIKU_THREADS_ENABLE=1", "TIKU_CRACEN_PK_ENABLE=1",
                     "TIKU_FLPR_ENABLE=1",
                     "TIKU_DRV_WIFI_CYW43_BT_ENABLE=1",
                     "TIKU_DRV_BLE_EM9305_ENABLE=1"):
            self.assertIn(flag, source)


if __name__ == "__main__":
    unittest.main()
