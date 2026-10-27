#!/usr/bin/env python3
"""Listen for XC_BOT UDP command packets and print them.

Usage:
    python tools/udp_listen.py --port 8888
"""

from __future__ import annotations

import argparse
import socket
import struct
import time

MAGIC = b"XC"
PACKET_SIZE = 8
FLAG_LIGHT = 0x01
FLAG_BUZZER = 0x02
FLAG_BRAKE = 0x04


def decode_flags(flags: int) -> str:
    names = []
    if flags & FLAG_LIGHT:
        names.append("LIGHT")
    if flags & FLAG_BUZZER:
        names.append("BUZZER")
    if flags & FLAG_BRAKE:
        names.append("BRAKE")
    return "|".join(names) if names else "-"


def checksum(packet: bytes) -> int:
    value = 0
    for byte in packet[:7]:
        value ^= byte
    return value


def main() -> int:
    parser = argparse.ArgumentParser(description="XC_BOT UDP command listener")
    parser.add_argument("--bind", default="0.0.0.0", help="bind address (default 0.0.0.0)")
    parser.add_argument("--port", type=int, default=8888, help="UDP port (default 8888)")
    args = parser.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))

    print(f"Listening on {args.bind}:{args.port} ...")
    packet_count = 0
    window_start = time.monotonic()

    try:
        while True:
            data, address = sock.recvfrom(2048)
            now = time.monotonic()

            if len(data) != PACKET_SIZE or data[:2] != MAGIC:
                print(f"BAD packet from {address[0]}:{address[1]} len={len(data)} data={data.hex()}")
                continue

            _, _, version, seq, left, right, flags, received = struct.unpack("8B", data)
            expected = checksum(data)
            status = "OK" if received == expected else f"BAD({received:02X}!={expected:02X})"

            print(
                f"{time.strftime('%H:%M:%S')} {address[0]}:{address[1]} "
                f"v{version} seq={seq:3d} L={left:3d} R={right:3d} "
                f"flags={flags:02X} [{decode_flags(flags)}] chk={status}"
            )

            packet_count += 1
            elapsed = now - window_start
            if elapsed >= 2.0:
                print(f"  -- {packet_count / elapsed:.1f} packets/s --")
                packet_count = 0
                window_start = now
    except KeyboardInterrupt:
        print("\nStopped.")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
