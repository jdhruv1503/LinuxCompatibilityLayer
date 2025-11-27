# QEMU Multi-Instance Reference for ESP32

## Overview

This document covers running multiple QEMU ESP32 instances concurrently for distributed computing demos.

## Port Forwarding Syntax

### Single Port Forward
```bash
-nic user,model=open_eth,hostfwd=tcp::9001-:9000
```

This forwards host port 9001 to guest port 9000.

### Multiple Port Forwards
```bash
-nic user,model=open_eth,hostfwd=tcp::9001-:9000,hostfwd=tcp::8081-:80
```

### Syntax Breakdown
```
-nic user,model=open_eth,hostfwd=tcp::<HOST_PORT>-:<GUEST_PORT>
     │    │              │       │    │            │
     │    │              │       │    │            └── Guest port (ESP32 listens on)
     │    │              │       │    └────────────── Host port (connect from PC)
     │    │              │       └─────────────────── Protocol (tcp or udp)
     │    │              └─────────────────────────── Port forwarding option
     │    └────────────────────────────────────────── NIC model (must be open_eth for ESP32)
     └─────────────────────────────────────────────── User-mode networking (NAT)
```

## Full QEMU Command for Multi-Instance

### Node 1 (Port 9001)
```bash
C:\Users\Dhruv\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe ^
    -nographic ^
    -machine esp32 ^
    -drive file=build/merged-flash.bin,if=mtd,format=raw ^
    -no-reboot ^
    -nic user,model=open_eth,hostfwd=tcp::9001-:9000
```

### Node 2 (Port 9002)
```bash
C:\Users\Dhruv\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe ^
    -nographic ^
    -machine esp32 ^
    -drive file=build/merged-flash.bin,if=mtd,format=raw ^
    -no-reboot ^
    -nic user,model=open_eth,hostfwd=tcp::9002-:9000
```

(Continue for nodes 3 and 4 with ports 9003 and 9004)

## Python Subprocess Management

### Launching Multiple Instances
```python
import subprocess
import threading

QEMU_PATH = r"C:\Users\Dhruv\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe"
FLASH_IMAGE = "build/merged-flash.bin"

def start_node(port):
    cmd = [
        QEMU_PATH,
        "-nographic",
        "-machine", "esp32",
        "-drive", f"file={FLASH_IMAGE},if=mtd,format=raw",
        "-no-reboot",
        "-nic", f"user,model=open_eth,hostfwd=tcp::{port}-:9000"
    ]

    process = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1  # Line buffered
    )
    return process

# Start 4 nodes
nodes = {}
for port in [9001, 9002, 9003, 9004]:
    nodes[port] = start_node(port)
    time.sleep(0.5)  # Stagger startup
```

### Graceful Shutdown
```python
def stop_node(process):
    try:
        process.terminate()
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()

# Stop all nodes
for port, process in nodes.items():
    stop_node(process)
```

## Connection Wait Logic

```python
import socket
import time

def wait_for_node_ready(port, timeout=30):
    """Wait until a node's server is accepting connections."""
    start = time.time()
    while time.time() - start < timeout:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(1)
            s.connect(('localhost', port))
            s.close()
            return True
        except:
            time.sleep(0.5)
    return False

# Usage
for port in [9001, 9002, 9003, 9004]:
    if wait_for_node_ready(port, timeout=40):
        print(f"Node on port {port} ready")
    else:
        print(f"Node on port {port} failed to start")
```

## Resource Considerations

### Memory Usage
- Each QEMU instance uses ~200-300 MB RAM
- 4 instances require ~1-1.2 GB total

### Startup Time
- Single instance: ~5-10 seconds to boot
- 4 instances: 15-30 seconds total (staggered)
- Recommended timeout: 40 seconds

### Port Conflicts
- Ensure no other processes use ports 9001-9004
- Check with: `netstat -an | findstr "900"`

## Common Issues

### WinError 10054 - Connection Reset
**Cause:** Connecting before QEMU has fully initialized networking.
**Solution:** Increase connection timeout to 40+ seconds.

### Address Already in Use
**Cause:** Previous QEMU instance didn't shut down cleanly.
**Solution:** Kill orphaned processes:
```bash
taskkill /F /IM qemu-system-xtensa.exe
```

### Flash Image Locked
**Cause:** Multiple QEMU instances trying to access same flash file.
**Note:** This is actually OK - QEMU opens the file read-only by default.

## Windows-Specific Notes

### Path Escaping
Always use raw strings for Windows paths in Python:
```python
QEMU_PATH = r"C:\Users\Dhruv\.espressif\tools\qemu-xtensa\..."
```

### Process Creation
Use `creationflags` to prevent console windows:
```python
process = subprocess.Popen(
    cmd,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    creationflags=subprocess.CREATE_NO_WINDOW  # Windows only
)
```

### Signal Handling
Windows doesn't support SIGTERM well. Use `terminate()` then `kill()`:
```python
process.terminate()
try:
    process.wait(timeout=3)
except subprocess.TimeoutExpired:
    process.kill()
```
