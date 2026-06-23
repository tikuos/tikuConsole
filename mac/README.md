# tikuConsole (macOS)

The macOS edition of the TikuOS host console + network bridge, written in C.
Same idea as the Linux Python tool: **one USB-serial cable carries both the
board's interactive shell _and_ real IP networking at the same time.**

When the board is in `slip` mode it interleaves, on a single UART, the shell's
ASCII text and SLIP/IP frames (`0xC0`-delimited). This tool demultiplexes that
one wire into an interactive **console** and a **`utun` network interface** at
once, so the Mac kernel can route to the board (`ping 172.16.7.2`,
`curl http://172.16.7.2`, …) while you still type commands.

Two front ends, one shared bridge core (`bridge.c`):

| binary         | what                                                        |
| -------------- | ----------------------------------------------------------- |
| `tikuconsole`  | the **GTK4 GUI** — port picker, ANSI-colour console, LEDs   |
| `slmux`        | the **command-line** bridge (a `select()` loop + raw tty)   |

## Why a separate macOS version

The Linux tool uses `/dev/net/tun` + `TUNSETIFF` and `iptables`; macOS has
neither. This uses a **`utun` device** (a `PF_SYSTEM` / `SYSPROTO_CONTROL`
control socket), and IOKit to fingerprint USB serial ports. utun packets carry
a 4-byte address-family header that SLIP frames don't, which the bridge
adds/strips. Plain C against libc, GTK4, and the macOS frameworks.

## Build

```sh
make              # both tikuconsole (GUI) + slmux (CLI)
make slmux        # just the CLI (libc only, no dependencies)
make tikuconsole  # just the GUI (needs GTK4)
```

GTK4 from Homebrew: `brew install gtk4 pkg-config`. Make sure `/opt/homebrew/bin`
is on `PATH` so `pkg-config` resolves `gtk4`.

## Run — the GUI

```sh
./tikuconsole
```

1. Plug in a board — the port and platform auto-fill (Apollo / MSP430 / RP2350
   are fingerprinted by USB id), and the baud follows. Click **Connect**.
2. Type into the console — keystrokes go to the board from anywhere in the
   window; the boot log is ANSI-coloured like `picocom`.
3. The three lights track the **USB** link, the board's **SLIP** mode, and host
   **Internet** (NAT). Flip **Networking** to toggle SLIP on the board.

Console mode needs no root. (The host `utun` bridge — full ping/NAT — is the
next GUI phase; for it today use `slmux` below.)

## Run — the CLI

Needs root, because it creates a `utun` device and configures the interface:

```sh
sudo ./slmux                       # auto-detects the first /dev/cu.usbmodem*
sudo ./slmux /dev/cu.usbmodemXXXX  # …or name the port
```

Then type `slip` once to put the board in SLIP mode. From another terminal the
board is a host on your Mac: `ping 172.16.7.2`. Quit with **Ctrl-]**.

### slmux options

| flag           | default                   | meaning                          |
| -------------- | ------------------------- | -------------------------------- |
| `PORT`         | first `/dev/cu.usbmodem*` | serial device                    |
| `--baud N`     | `115200`                  | baud rate                        |
| `--host-ip A`  | `172.16.7.1`              | this Mac's address on the link   |
| `--board-ip B` | `172.16.7.2`              | the board's address              |

## Status

- **Done:** the shared bridge core (SLIP / serial / utun), the `slmux` CLI, and
  the `tikuconsole` GUI's console + connection (port fingerprinting, ANSI
  colour, focus-independent key routing, always-on SLIP demux).
- **Next:** the GUI networking side-panel (host `utun` bridge, in-app ping with
  the RTT chart, NAT switch) and the firmware build/flash bar — the rest of the
  Linux app's panels. Board → internet NAT on macOS becomes a `pf` anchor or a
  small userspace relay (the Linux `iptables MASQUERADE` equivalent).
