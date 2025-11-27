#!/usr/bin/env python3
"""
Collision Detection Test Script

Sends mock telemetry data to collision_guard via UDP to test collision detection.
The telemetry packet format matches what collision_server sends.

Usage:
    python tools/test_collision.py [--host HOST] [--port PORT]

The script simulates two vehicles:
- Ego vehicle (car_id=0): stationary at origin
- Target vehicle (car_id=1): moving toward ego

Test scenarios:
1. Safe: vehicles far apart
2. Warning: vehicles moderately close
3. Collision: vehicles close and approaching
"""

import socket
import struct
import time
import argparse
import math

# Telemetry packet format (must match collision_guard/collision_server)
# float lat, float lon, float speed_mps, float heading, int car_id
TELEMETRY_FORMAT = '<ffffI'  # Little-endian: 4 floats + 1 unsigned int


def send_telemetry(sock, addr, lat, lon, speed, heading, car_id):
    """Send a telemetry packet."""
    packet = struct.pack(TELEMETRY_FORMAT, lat, lon, speed, heading, car_id)
    sock.sendto(packet, addr)


def meters_to_degrees_lat(meters):
    """Convert meters to degrees latitude."""
    return meters / 111000.0


def meters_to_degrees_lon(meters, ref_lat):
    """Convert meters to degrees longitude at given latitude."""
    return meters / (111000.0 * math.cos(math.radians(ref_lat)))


def run_test(host='localhost', port=8000):
    """Run collision detection test scenarios."""
    print("=" * 60)
    print("  Collision Detection Test Script")
    print("=" * 60)
    print(f"Target: {host}:{port}")
    print()

    # Create UDP socket
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = (host, port)

    # Reference point (San Francisco)
    REF_LAT = 37.7749
    REF_LON = -122.4194

    print("[Test 1] Safe scenario - vehicles 50m apart, stationary")
    print("-" * 60)

    # Ego at origin, stationary
    send_telemetry(sock, addr, REF_LAT, REF_LON, 0.0, 0.0, 0)
    time.sleep(0.1)

    # Target 50m north, stationary
    target_lat = REF_LAT + meters_to_degrees_lat(50)
    send_telemetry(sock, addr, target_lat, REF_LON, 0.0, 0.0, 1)
    time.sleep(0.5)

    # Send a few more updates
    for _ in range(5):
        send_telemetry(sock, addr, REF_LAT, REF_LON, 0.0, 0.0, 0)
        time.sleep(0.05)
        send_telemetry(sock, addr, target_lat, REF_LON, 0.0, 0.0, 1)
        time.sleep(0.05)

    print("Sent: Ego at origin, Target 50m north")
    print("Expected: SAFE (distance=50m, TTC=inf)")
    print()
    time.sleep(1)

    print("[Test 2] Warning scenario - vehicles approaching")
    print("-" * 60)

    # Target 20m north, moving south at 5 m/s
    target_lat = REF_LAT + meters_to_degrees_lat(20)
    for _ in range(10):
        send_telemetry(sock, addr, REF_LAT, REF_LON, 0.0, 0.0, 0)
        time.sleep(0.05)
        # Heading 180 = moving south
        send_telemetry(sock, addr, target_lat, REF_LON, 5.0, 180.0, 1)
        time.sleep(0.05)

    print("Sent: Ego stationary, Target 20m north moving south at 5m/s")
    print("Expected: WARNING (distance=20m, TTC=4s)")
    print()
    time.sleep(1)

    print("[Test 3] Collision imminent - vehicles very close and fast")
    print("-" * 60)

    # Target 8m north, moving south at 10 m/s
    target_lat = REF_LAT + meters_to_degrees_lat(8)
    for i in range(20):
        send_telemetry(sock, addr, REF_LAT, REF_LON, 0.0, 0.0, 0)
        time.sleep(0.05)
        # Moving target closer
        current_offset = 8 - (i * 0.3)  # Decrease by 0.3m each update
        if current_offset < 3:
            current_offset = 3
        target_lat = REF_LAT + meters_to_degrees_lat(current_offset)
        send_telemetry(sock, addr, target_lat, REF_LON, 10.0, 180.0, 1)
        time.sleep(0.05)

    print("Sent: Ego stationary, Target 8m->3m north moving south at 10m/s")
    print("Expected: COLLISION ALERT (distance<10m, TTC<2s)")
    print()
    time.sleep(1)

    print("[Test 4] Collision cleared - vehicles separating")
    print("-" * 60)

    # Target moving away
    for i in range(15):
        send_telemetry(sock, addr, REF_LAT, REF_LON, 0.0, 0.0, 0)
        time.sleep(0.05)
        # Moving target further
        current_offset = 3 + (i * 2)  # Increase by 2m each update
        target_lat = REF_LAT + meters_to_degrees_lat(current_offset)
        send_telemetry(sock, addr, target_lat, REF_LON, 5.0, 0.0, 1)  # Moving north
        time.sleep(0.05)

    print("Sent: Target moving away north")
    print("Expected: Collision cleared message")
    print()

    print("=" * 60)
    print("Test complete! Check console output from collision_guard.")
    print("=" * 60)

    sock.close()


def main():
    parser = argparse.ArgumentParser(description='Test collision detection system')
    parser.add_argument('--host', default='localhost', help='Host address (default: localhost)')
    parser.add_argument('--port', type=int, default=8000, help='UDP port (default: 8000)')
    args = parser.parse_args()

    run_test(args.host, args.port)


if __name__ == '__main__':
    main()
