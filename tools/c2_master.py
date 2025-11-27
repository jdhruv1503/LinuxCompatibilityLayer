#!/usr/bin/env python3
"""
C2 Master - Command & Control client for ESP32 Linux Compatibility Layer

Sends ELF payloads to the ESP32 and receives execution output.

Usage:
    python c2_master.py <payload.elf> <ESP_IP> [port]

Example:
    python c2_master.py apps/c2_payload/payload.elf 192.168.1.105
    python c2_master.py build/guest_apps/c2_payload.elf localhost    # QEMU with port forward
"""

import socket
import struct
import sys
import os
import argparse
import time

DEFAULT_PORT = 9000
RECV_TIMEOUT = 30  # seconds


def send_payload(ip: str, port: int, filepath: str, verbose: bool = True) -> int:
    """Send an ELF payload to the ESP32 and receive output."""

    # Validate file
    if not os.path.exists(filepath):
        print(f"[Master] Error: File not found: {filepath}")
        return 1

    # Read ELF binary
    with open(filepath, "rb") as f:
        elf_data = f.read()

    if verbose:
        print(f"[Master] Loaded payload: {len(elf_data)} bytes")
        print(f"[Master] Connecting to {ip}:{port}...")

    # Connect to ESP32
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)  # Connection timeout

    try:
        sock.connect((ip, port))
    except socket.timeout:
        print(f"[Master] Error: Connection timeout")
        return 1
    except ConnectionRefusedError:
        print(f"[Master] Error: Connection refused. Is the ESP32 running?")
        return 1
    except OSError as e:
        print(f"[Master] Error: {e}")
        return 1

    if verbose:
        print(f"[Master] Connected!")

    try:
        # Send size header (4 bytes, little-endian unsigned int)
        header = struct.pack('<I', len(elf_data))
        sock.sendall(header)

        # Send ELF data
        sock.sendall(elf_data)

        if verbose:
            print(f"[Master] Payload sent. Waiting for execution output...\n")
            print("=" * 60)

        # Receive output
        sock.settimeout(RECV_TIMEOUT)

        try:
            while True:
                data = sock.recv(1024)
                if not data:
                    break

                # Print received data (stdout from ESP32)
                text = data.decode('utf-8', errors='replace')
                sys.stdout.write(text)
                sys.stdout.flush()

        except socket.timeout:
            if verbose:
                print("\n[Master] Receive timeout (this may be normal)")
        except KeyboardInterrupt:
            if verbose:
                print("\n[Master] Interrupted by user")

    finally:
        sock.close()

    if verbose:
        print("=" * 60)
        print("[Master] Connection closed")

    return 0


def main():
    parser = argparse.ArgumentParser(
        description="C2 Master - Send ELF payloads to ESP32",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python c2_master.py payload.elf 192.168.1.105           # Real hardware
  python c2_master.py payload.elf localhost               # QEMU with port forward
  python c2_master.py payload.elf localhost -p 9000       # Explicit port
  python c2_master.py payload.elf 10.0.2.15 -q            # Quiet mode
"""
    )
    parser.add_argument("payload", help="Path to ELF payload file")
    parser.add_argument("ip", help="ESP32 IP address (or localhost for QEMU)")
    parser.add_argument(
        "-p", "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"TCP port (default: {DEFAULT_PORT})"
    )
    parser.add_argument(
        "-q", "--quiet",
        action="store_true",
        help="Quiet mode (only show payload output)"
    )

    args = parser.parse_args()

    return send_payload(args.ip, args.port, args.payload, not args.quiet)


if __name__ == "__main__":
    sys.exit(main())
