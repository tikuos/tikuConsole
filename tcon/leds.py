"""
TikuConsole v0.01 -- Simple. Ubiquitous. Intelligence, Everywhere.
http://tiku-os.org  ·  Ambuj Varshney <ambuj@tiku-os.org>

tcon.leds - LedsMixin: the three main-window status lights.

Traffic-light dots (green = on, red = off) for the USB link, the board's SLIP
mode, and host NAT (Internet).  SLIP is driven by the board's own console
status lines via _set_slip_led(); the others by connect/NAT state.

SPDX-License-Identifier: Apache-2.0
"""
from tcon import GREEN


class LedsMixin:
    def _update_leds(self):
        """Refresh the main-window status lights: USB link, board SLIP, NAT."""
        self.usb_led.set_markup(self._led(self.ser is not None, "USB"))
        self.slip_led.set_markup(self._led(self.slip_on, "SLIP"))
        self.nat_led.set_markup(self._led(self.nat_on, "Internet"))

    @staticmethod
    def _led(on, text):
        return ("<span foreground='%s'>●</span> %s"
                % (GREEN if on else "#ff6b6b", text))

    def _set_slip_led(self, on):
        if on != self.slip_on:                     # driven by the board's own msgs
            self.slip_on = on
            self._update_leds()
