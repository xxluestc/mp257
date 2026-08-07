#!/usr/bin/env python3
"""Minimal MP257 UART sender/receiver using only the Python standard library."""

import argparse
import os
import select
import subprocess
import sys
import termios
import time


def console_conflict(device: str) -> bool:
    tty_name = os.path.basename(device)
    try:
        with open("/sys/class/tty/console/active", encoding="ascii") as stream:
            if tty_name in stream.read().split():
                return True
    except OSError:
        pass

    try:
        with open("/proc/cmdline", encoding="ascii") as stream:
            for item in stream.read().split():
                if item.startswith("console=" + tty_name):
                    return True
    except OSError:
        pass

    result = subprocess.run(
        ["systemctl", "is-active", "--quiet",
         "serial-getty@" + tty_name + ".service"],
        check=False,
    )
    return result.returncode == 0


def configure_uart(fd: int) -> None:
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = termios.CLOCAL | termios.CREAD | termios.CS8
    attrs[3] = 0
    attrs[4] = termios.B115200
    attrs[5] = termios.B115200
    attrs[6][termios.VMIN] = 0
    attrs[6][termios.VTIME] = 1
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Send data through the MP257 external-Bluetooth UART."
    )
    parser.add_argument("--device", default="/dev/ttySTM0")
    parser.add_argument("--send", help="UTF-8 text to send")
    parser.add_argument(
        "--line",
        action="store_true",
        help="append a newline to --send (needed by LED ON/OFF and PING)",
    )
    parser.add_argument("--hex", dest="hex_data",
                        help="hex bytes to send, for example: 01ff0a")
    parser.add_argument("--listen", type=float, default=10.0,
                        help="receive duration in seconds (default: 10)")
    parser.add_argument(
        "--force",
        action="store_true",
        help="run even if the UART is an active console (unsafe; lab use only)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if console_conflict(args.device) and not args.force:
        print(
            f"REFUSED: {args.device} is still used by the Linux console/getty. "
            "Release the console first, or use --force only for a controlled test.",
            file=sys.stderr,
        )
        return 2

    if args.send is not None and args.hex_data is not None:
        print("--send and --hex are mutually exclusive", file=sys.stderr)
        return 2

    payload = b""
    if args.send is not None:
        payload = args.send.encode("utf-8")
        if args.line:
            payload += b"\n"
    elif args.hex_data is not None:
        if args.line:
            print("--line can only be used with --send", file=sys.stderr)
            return 2
        try:
            payload = bytes.fromhex(args.hex_data)
        except ValueError as error:
            print(f"invalid --hex value: {error}", file=sys.stderr)
            return 2

    try:
        fd = os.open(args.device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    except OSError as error:
        print(f"cannot open {args.device}: {error}", file=sys.stderr)
        return 1

    try:
        configure_uart(fd)
        if payload:
            os.write(fd, payload)
            print(f"TX {len(payload)} byte(s): {payload.hex(' ')}")

        deadline = time.monotonic() + max(args.listen, 0.0)
        while time.monotonic() < deadline:
            ready, _, _ = select.select([fd], [], [], min(0.2, deadline - time.monotonic()))
            if ready:
                data = os.read(fd, 4096)
                if data:
                    print(f"RX {len(data)} byte(s): {data.hex(' ')} | "
                          f"{data.decode('utf-8', errors='replace')}")
    finally:
        os.close(fd)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
