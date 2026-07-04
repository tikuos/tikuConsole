#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
tikuble.py - wireless tikuOS shell over Bluetooth Low Energy.

A friendly terminal for the tikuOS shell exposed over the Nordic UART Service
(NUS) by the Apollo510 Blue EVB (EM9305 radio). Run `ble uart` on the board,
then run this: it scans for the advertised name, connects, subscribes to the TX
characteristic (device -> host notifications), and forwards your keystrokes to
the RX characteristic (host -> device writes). It is the BLE transport twin of
the serial tikuConsole.

    python3 tikuble.py                 # scan for "tikuOS", connect, open a shell
    python3 tikuble.py --name mydev    # a differently-named board
    python3 tikuble.py --address <id>  # connect straight to a known address
    python3 tikuble.py --scan          # list nearby BLE devices and exit

Needs bleak (`pip install bleak`). Quit the session with Ctrl-] (Ctrl-C is
passed through to the board so the wireless shell can use it).
"""
import argparse
import asyncio
import sys
import termios
import tty

from bleak import BleakClient, BleakScanner

# Nordic UART Service UUIDs (must match arch/ambiq/tiku_ble_nus.c on the device).
NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # host writes -> device in
NUS_TX      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device notifies -> host

QUIT_KEY = 0x1D          # Ctrl-] leaves the session (Ctrl-C goes to the board)
WRITE_CHUNK = 180        # keep a single GATT write within one ATT MTU


async def find_device(name, address, timeout):
    """Locate the board by explicit address, else by advertised name/NUS UUID."""
    if address:
        dev = await BleakScanner.find_device_by_address(address, timeout=timeout)
        if dev is None:
            print(f"tikuble: no BLE device at address {address}", file=sys.stderr)
        return dev

    want = name.lower()

    def match(d, adv):
        if d.name and want in d.name.lower():
            return True
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        return NUS_SERVICE in uuids

    print(f"tikuble: scanning for \"{name}\" (up to {timeout:.0f}s)...",
          file=sys.stderr)
    dev = await BleakScanner.find_device_by_filter(match, timeout=timeout)
    if dev is None:
        print(f"tikuble: \"{name}\" not found -- is `ble uart` running on the "
              f"board?", file=sys.stderr)
    return dev


async def run_session(dev):
    """Bridge the local terminal to the board's NUS characteristics."""
    loop = asyncio.get_running_loop()
    outq = asyncio.Queue()
    done = loop.create_future()

    def on_tx(_char, data: bytearray):
        # Device -> host: write straight to the terminal.
        sys.stdout.buffer.write(bytes(data))
        sys.stdout.buffer.flush()

    def on_stdin():
        data = sys.stdin.buffer.raw.read(256)
        if not data:
            return
        if QUIT_KEY in data:
            if not done.done():
                done.set_result(None)
            return
        outq.put_nowait(bytes(data))

    async with BleakClient(dev) as client:
        print(f"tikuble: connected to {dev.name or dev.address} "
              f"-- Ctrl-] to quit\r", file=sys.stderr)
        await client.start_notify(NUS_TX, on_tx)

        loop.add_reader(sys.stdin.fileno(), on_stdin)

        async def writer():
            while True:
                data = await outq.get()
                for i in range(0, len(data), WRITE_CHUNK):
                    await client.write_gatt_char(NUS_RX, data[i:i + WRITE_CHUNK],
                                                 response=False)

        wtask = asyncio.ensure_future(writer())
        # Nudge the board so it reprints its prompt into our fresh terminal.
        outq.put_nowait(b"\r")
        try:
            await done
        finally:
            loop.remove_reader(sys.stdin.fileno())
            wtask.cancel()
            try:
                await client.stop_notify(NUS_TX)
            except Exception:
                pass


async def run_batch(dev, cmds, collect):
    """Non-interactive: send each command, print notifications, then exit.
    Scriptable, and the way to smoke-test the link without a real terminal."""
    async with BleakClient(dev) as client:
        print(f"# connected to {dev.name or dev.address}", file=sys.stderr)

        def on_tx(_char, data: bytearray):
            sys.stdout.buffer.write(bytes(data))
            sys.stdout.buffer.flush()

        await client.start_notify(NUS_TX, on_tx)
        await asyncio.sleep(1.0)                     # let the banner/prompt land
        for c in cmds:
            await client.write_gatt_char(NUS_RX, (c + "\r").encode(),
                                         response=False)
            await asyncio.sleep(collect)             # collect this cmd's output
        try:
            await client.stop_notify(NUS_TX)
        except Exception:
            pass
    return 0


async def amain(args):
    if args.scan:
        print("scanning 8s...", file=sys.stderr)
        for d in await BleakScanner.discover(timeout=8.0):
            print(f"  {d.address}  {d.name or ''}")
        return 0

    dev = await find_device(args.name, args.address, args.timeout)
    if dev is None:
        return 1

    if args.cmd:
        return await run_batch(dev, args.cmd, args.collect)

    # Put the terminal in raw mode so keystrokes stream to the board immediately.
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        await run_session(dev)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)
        print("\r\ntikuble: session closed.", file=sys.stderr)
    return 0


def main():
    ap = argparse.ArgumentParser(description="Wireless tikuOS shell over BLE.")
    ap.add_argument("--name", default="tikuOS", help="advertised name to find")
    ap.add_argument("--address", help="connect directly to this BLE address")
    ap.add_argument("--scan", action="store_true", help="list devices and exit")
    ap.add_argument("--timeout", type=float, default=12.0, help="scan timeout (s)")
    ap.add_argument("--cmd", action="append",
                    help="non-interactive: send this command (repeatable)")
    ap.add_argument("--collect", type=float, default=3.0,
                    help="seconds to collect output after each --cmd")
    args = ap.parse_args()
    try:
        return asyncio.run(amain(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
