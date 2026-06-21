# TikuConsole

A branded GTK4 desktop **serial console for [TikuOS](http://tiku-os.org)
devices** — a friendlier `picocom`/`minicom`. It auto-detects the serial port,
the board (MSP430 / RP2350 / Apollo) and its baud, and drops you into a colour
console you can type straight into.

Flip on **Networking** and it brings up SLIP/IP over the very same wire: a TUN
interface (so the Linux kernel's own `ping`/`curl` ride it), a **rootless**
ICMP-over-SLIP board pinger, and board → internet NAT.

![TikuConsole](tikuConsole.png)

## Features

- **Auto-detect** — port dropdown with platform + baud guessed from USB VID/PID
  (Apollo J-Link, RP2350 USB-CDC, MSP430 eZ-FET/FT232…).
- **Colour console** — ANSI/SGR rendering, fixed-width font, type from anywhere
  in the window; greys out when disconnected.
- **Networking (optional)** over the one shared UART:
  - **SLIP** toggle on the board.
  - **Ping** the board — built in userspace (ICMP over SLIP), **no root, no
    TUN, no system `ping`**; live packet animation + RTT chart + stats.
  - **Internet (NAT)** — route the board to the internet via the host's WAN.
  - **TUN bridge** so the host kernel treats the board as a network peer.
- **Status lights** — `● USB` (link) · `● SLIP` (board SLIP on) · `● Internet`
  (NAT on).

The plain console, the SLIP toggle and the board ping all work **without root**.
Only the host **TUN/NAT bridge** needs `sudo`.

## Requirements

- Python 3, PyGObject with **GTK 4**, and **pyserial**.

```bash
# Debian / Ubuntu
sudo apt install python3-gi gir1.2-gtk-4.0 python3-serial
```

A TikuOS board built with the net stack (`slip`/`ping`) on the other end.

## Run

```bash
python3 tikuconsole.py          # plain console + rootless ping — no root needed
sudo python3 tikuconsole.py     # also enables the host TUN/NAT bridge
```

1. Plug in a board — the port, platform and baud auto-fill. Click **Connect**.
2. Type into the console.
3. For networking, flip **Networking**: toggle SLIP on the board, **Ping** it,
   and (as root) enable **NAT** to give the board internet.

### Headless / env vars

```bash
TIKUCONSOLE_SCAN=1     python3 tikuconsole.py   # list ports + guesses, then exit
TIKUCONSOLE_SMOKE_MS=1200 python3 tikuconsole.py # build the window, quit (CI smoke)
TIKUCONSOLE_NO_SPLASH=1 python3 tikuconsole.py  # skip the splash screen
```

## Addresses

Point-to-point SLIP link: device **172.16.7.2**, host **172.16.7.1**. The
device address is set on the firmware side (`make … IP=10.0.0.5`); keep both
ends in sync if you change it.

## Layout

`tikuconsole.py` is a thin launcher; the implementation lives in the **`tcon/`**
package, one module per subsystem:

| module | role |
|---|---|
| `app` | the `Gtk.Application`: state + window assembly + mixin composition |
| `console` | ANSI rendering, keyboard routing, console CSS |
| `connection` | serial link, SLIP/TUN bridge, connect, port picker |
| `ping` | rootless ICMP-over-SLIP pinger, RTT chart, animation |
| `nat` · `leds` · `splash` · `ui` | NAT, status lights, splash, side-pane |
| `packets` · `ports` | pure (GTK-free) ICMP/IP build-parse + port detection |

`slmux.py` is the command-line twin and the shared SLIP framing.

## License

Apache-2.0 © TikuOS · Ambuj Varshney &lt;ambuj@tiku-os.org&gt;
