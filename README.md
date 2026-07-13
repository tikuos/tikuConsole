# TikuConsole

TikuConsole is a serial console and network bridge for
[TikuOS](http://tiku-os.org) computers. Over the board's serial link it
gives you an interactive shell, and bridges that same wire onto IP — connecting
the board to the internet and to the host's network.

![TikuConsole](tikuConsole.png)

## Requirements

- Python 3, PyGObject with **GTK 4**, and **pyserial**.

```bash
# Debian / Ubuntu
sudo apt install python3-gi gir1.2-gtk-4.0 python3-serial
```

A TikuOS board built with the net stack (`slip`/`ping`) on the other end.
The firmware bar discovers the canonical board list from the sibling
`TikuBench` checkout and supports MSP430 FR5994/FR6989, RP2350, Apollo4/510,
Apollo510 Blue, and nRF54L15-DK builds.

## Run

```bash
python3 tikuconsole.py          # plain console + rootless ping — no root needed
sudo python3 tikuconsole.py     # also enables the host TUN/NAT bridge
```

Build and flash subprocesses are automatically returned to `SUDO_USER`, so
using `sudo` for TUN/NAT does not create root-owned firmware artifacts or hide
per-user toolchains and nrfutil plugins.

1. Plug in a board — the port, platform and baud auto-fill. Click **Connect**.
2. Type into the console.
3. Click **Files…** to browse, upload, download and delete files in the board's
   `/data` store (uses the device `send`/`recv` commands; binary-safe).
4. For networking, flip **Networking**: toggle SLIP on the board, **Ping** it,
   and (as root) enable **NAT** to give the board internet.

### Options

```bash
TIKUCONSOLE_SCAN=1     python3 tikuconsole.py   # list ports + guesses, then exit
TIKUCONSOLE_SMOKE_MS=1200 python3 tikuconsole.py # build the window, quit (CI smoke)
TIKUCONSOLE_NO_SPLASH=1 python3 tikuconsole.py  # skip the splash screen
```

The build bar exposes only features supported by the selected board: RP2350
WiFi/USB/Bluetooth, Apollo510 Blue BLE, HTTPS on Cortex-M boards, and nRF54L15
CRACEN public-key and FLPR options. Network/radio profiles visibly force the
shell dependency, and the selected toolbar baud is used for both firmware and
the post-flash reconnect.

Command construction and Linux/macOS board parity can be checked headlessly:

```bash
python3 -m unittest discover -s tests -p 'test_*.py'
```

The board's IP defaults to **172.16.7.2** (host **172.16.7.1**); set it on the
firmware side with `make … IP=10.0.0.5` and keep both ends in sync.

## Wireless shell over BLE (tikuble.py)

`tikuble.py` is the same tikuOS shell with no wire at all: boards with a BLE
radio (Apollo510 Blue EVB) expose the console over the Nordic UART Service.
Run `ble uart` on the board, then:

```bash
pip install bleak            # macOS / Linux BLE client library
python3 tikuble.py           # scan for "tikuOS", connect, interactive shell
python3 tikuble.py --cmd info --cmd free   # scripted: run commands, print output
python3 tikuble.py --scan    # list nearby BLE devices
```

Quit the interactive session with **Ctrl-]** (Ctrl-C passes through to the
board).

## License

Apache-2.0 © TikuOS · Ambuj Varshney &lt;ambuj@tiku-os.org&gt;
