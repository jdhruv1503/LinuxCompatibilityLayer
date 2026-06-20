# ESP32 Linux Compatibility Layer (TLCL)

A **Unikernel / Library OS** architecture for the ESP32 microcontroller that enables the dynamic loading and execution of standard Linux ELF binaries — without reflashing the firmware.

## Overview

This project bridges the gap between the static nature of embedded RTOS and the dynamic flexibility of Linux. By implementing a POSIX compatibility layer and an ELF loader on top of ESP-IDF/FreeRTOS, it allows developers to run standard C applications on the ESP32 without reflashing the firmware.

### Key Features

- **ELF Loader:** Dynamic loading of relocatable binaries into IRAM/DRAM.
- **System Call Shims:** POSIX-compliant implementation of `open`, `read`, `write`, `socket`, `execve`, `pipe`, `clock_gettime`, and 20+ more syscalls.
- **Virtual Filesystem:** UNIX-like `/dev/` nodes for hardware abstraction (`/dev/buzzer`, `/dev/c2`).
- **No-MMU Support:** Runs in a single address space using a "Spawn" model instead of `fork`.
- **Standard I/O:** Full support for `printf`, file operations, and BSD sockets.
- **Per-Guest Heap Tracking:** Memory accounting per loaded ELF (`lcl ps` command).

## Presentations & Reports

Pre-compiled PDFs are in the repository root:

| File | Description |
|------|-------------|
| `presentation_full.pdf` | Full architecture presentation (14 slides) |
| `improvements_only.pdf` | Recent improvements: pipes, cross-platform build, heap tracking (16 slides) |
| `project_presentation.pdf` | Summary slides (8 slides) |

## Project Structure

- **`main/`**: Core firmware — ELF loader, syscall shims, VFS drivers.
- **`apps/`**: Guest ELF applications (see full list below).
- **`tools/`**: Build automation, demo controllers, and test scripts.
- **`components/`**: ESP-IDF components (ELF Loader, LittleFS).
- **`documentation/`**: Architecture docs, task outputs, and LaTeX sources.

## Quick Start

**All build and simulation tasks use `tools/build_and_run.py`.** It handles the two-pass build, symbol export, flash padding, and QEMU timeout automatically.

### Prerequisites

- ESP-IDF v5.4
- Python 3.8+
- QEMU for Xtensa (installed via `idf_tools.py`)

### Build & Run

```bash
# Full workflow: build everything and start QEMU simulation
python tools/build_and_run.py

# Build only (no QEMU)
python tools/build_and_run.py --build

# Simulate only (requires prior build)
python tools/build_and_run.py --sim

# Clean build + simulate
python tools/build_and_run.py --clean

# Custom QEMU timeout (default: 20 seconds)
python tools/build_and_run.py --timeout 60
```

---

## Guest Applications

All apps in `apps/` are compiled as Position-Independent ELFs and loaded at runtime by the firmware. Build any of them with:

```bash
python tools/build_and_run.py --build-guest <app_name> --set-elf <app_name>
```

| App | Description |
|-----|-------------|
| `hello_world` | Minimal loader test — returns exit code 42. |
| `c2_bot` | TCP server on port 9000; accepts incoming ELF payloads, saves and executes them, redirects stdout back to the sender. |
| `c2_payload` | Demo C2 payload — simulates a WiFi network scanner (mock SSID table). Designed to be pushed by `c2_bot`. |
| `c2_redirect` | Tests `dup2` stdout redirection by connecting to `10.0.2.100:12345` and verifying both UART and network receive output. |
| `collision_guard` | V2X safety app — listens on UDP 8000 for vehicle telemetry, computes Time-To-Collision, triggers `/dev/buzzer` when TTC < 2s. |
| `collision_server` | HTTP/UDP bridge — serves a web dashboard on port 80, accepts `POST /api/telemetry` JSON, forwards to `collision_guard` over UDP, spawns it via `execve`. |
| `map_reduce_worker` | Map-reduce worker — reads integers from stdin, returns `RESULT: SUM=<n> COUNT=<n>` to the master. |
| `data_logger` | Industrial data acquisition simulator — logs 20 samples of temperature/humidity/pressure to `/log.txt`, prints statistics. |
| `energy_mgmt` | PID-controlled HVAC simulation for 20 iterations, targeting 23°C setpoint, streams JSON telemetry to `10.0.2.2:8080`. |
| `modbus_gateway` | Modbus register-map simulator — polls 16 holding registers across 4 virtual field devices, prints a formatted register map with change detection. |
| `mqtt_publisher` | MQTT 3.1.1 client — hand-built CONNECT/PUBLISH packets, connects to `10.0.2.2:1883`, publishes 10 temperature readings to `sensor/temp`. |
| `tcp_client` | Basic TCP smoke test — connects to example.com:80, sends HTTP GET, prints first 100 bytes to verify socket shim. |
| `test_fs` | Filesystem shim test — writes and reads back a file via both `fopen`/`fprintf` and `open`/`read` to verify VFS paths. |
| `time_demo` | Real-time syscall benchmark — tests `nanosleep` precision (10–500ms), jitter measurement, and `clock_gettime`/`gettimeofday` resolution. |
| `watchdog_monitor` | System health monitor — tests memory allocation, network stack, and filesystem integrity across 5 cycles, reports per-subsystem `[OK]/[FAIL]` status. |
| `dup2_test` | Unit test for `dup2` redirection mechanics — verifies UART-mirror behavior on unconnected socket. |

---

## Demos

### Demo 1: Distributed C2 Command & Control

**What it shows:** Send a compiled Linux ELF binary over TCP to an ESP32 node. The node loads and executes it immediately, streaming `stdout` back via a custom `dup2` redirection shim.

```
Master (PC) --[TCP: ELF bytes]--> c2_bot (ESP32) --> execve() --> Guest ELF
                                                                       |
Master receives stdout <----[TCP: printf output]---- shim_write intercepts fd=1
```

**Terminal 1** — build and start the C2 bot firmware in QEMU:
```bash
python tools/build_and_run.py --build-guest c2_bot --set-elf c2_bot --timeout 120
```

**Terminal 2** — build the payload and send it:
```bash
python tools/build_and_run.py --build-guest c2_payload
python tools/c2_master.py build/guest_apps/c2_payload.elf localhost
```

The master connects to port 9000, uploads the ELF, and prints the guest's `printf` output in real time.

---

### Demo 2: Distributed Map-Reduce

**What it shows:** Spin up 4 ESP32 QEMU instances running `c2_bot`. The `c2_master.py` controller distributes a computation task across all nodes and aggregates results — distributed map-reduce on microcontrollers.

```
c2_master.py
  ├── QEMU Node 0 (port 9001) --> map_reduce_worker ELF --> result
  ├── QEMU Node 1 (port 9002) --> map_reduce_worker ELF --> result
  ├── QEMU Node 2 (port 9003) --> map_reduce_worker ELF --> result
  └── QEMU Node 3 (port 9004) --> map_reduce_worker ELF --> result
                    <-- aggregate -->
```

**Step 1** — build the firmware and worker:
```bash
python tools/build_and_run.py --build-guest map_reduce_worker --build
```

**Step 2** — launch the cluster controller:
```bash
# Automated: spawns 4 QEMU instances, runs map-reduce, exits
python tools/c2_master.py --auto --timeout 40

# Interactive split-screen UI (r=run, m=math, c=clear, q=quit)
python tools/c2_master.py

# Custom: 2 nodes, math benchmark
python tools/c2_master.py --nodes 2 --math
```

---

### Demo 3: Cyber-Physical Collision Avoidance (V2X)

**What it shows:** A Linux application on ESP32 receives vehicle telemetry (GPS + speed) via UDP, computes Time-To-Collision with vector math, and triggers `/dev/buzzer` — a custom VFS driver mapped to PWM GPIO.

```
test_collision.py --[UDP telemetry]--> collision_guard ELF (ESP32)
                                              |
                                      TTC = |P_rel| / |V_rel|
                                              |
                                      write("/dev/buzzer") --> PWM GPIO
```

**Step 1** — build and run the collision guard firmware:
```bash
python tools/build_and_run.py --build-guest collision_guard --set-elf collision_guard --timeout 60
```

**Step 2** — send simulated telemetry (safe → warning → collision scenarios):
```bash
python tools/test_collision.py --host localhost --port 9000
```

---

### Demo 4: Full V2X Stack with Web Dashboard

**What it shows:** The complete V2X stack — `collision_server` runs as a guest ELF, serves an HTTP dashboard on port 80, accepts JSON telemetry from a browser or script, spawns `collision_guard` via `execve`, and logs collision events to the filesystem.

```
Browser/curl --[POST /api/telemetry]--> collision_server (port 80)
                                               |
                                        UDP --> collision_guard
                                               |
                                        /dev/buzzer + collisions.log
```

**Run:**
```bash
python tools/build_and_run.py --build-guest collision_server --set-elf collision_server --timeout 60
# Then navigate to http://localhost:8080 (with QEMU port forwarding) or:
curl -X POST http://localhost:80/api/telemetry -d '{"lat":0,"lon":0,"speed":10,"heading":90,"car_id":1}'
```

---

### Demo 5: MQTT Sensor Publisher

**What it shows:** A guest ELF on ESP32 acts as an MQTT 3.1.1 client, hand-crafting raw protocol packets and publishing sensor data to a broker — demonstrating that standard IoT protocols work through the socket shim with no library changes.

**Run:**
```bash
# Start a local MQTT broker (e.g. mosquitto on port 1883)
python tools/build_and_run.py --build-guest mqtt_publisher --set-elf mqtt_publisher --timeout 30
```

Publishes 10 JSON temperature readings to topic `sensor/temp`.

---

### Demo 6: Industrial Modbus Gateway

**What it shows:** Modbus TCP register polling across 4 virtual field devices (temperature sensor, pressure transmitter, flow meter, motor drive) — simulating an IIoT gateway running standard Linux code on an MCU.

```bash
python tools/build_and_run.py --build-guest modbus_gateway --set-elf modbus_gateway --timeout 30
```

---

### Demo 7: Energy Management (PID Control)

**What it shows:** A PID-controlled HVAC simulation running as a guest ELF, targeting 23°C, streaming JSON telemetry to `10.0.2.2:8080` — demonstrating real-time control loops with network output.

```bash
python tools/build_and_run.py --build-guest energy_mgmt --set-elf energy_mgmt --timeout 30
```

---

### Demo 8: Filesystem & Time Syscall Verification

**What it shows:** Validates the filesystem shim (`fopen`, `fprintf`, `open`, `read`) and real-time clock shims (`nanosleep`, `clock_gettime`, `gettimeofday`) with measurable precision benchmarks.

```bash
# Filesystem test
python tools/build_and_run.py --build-guest test_fs --set-elf test_fs

# Time syscall benchmark (nanosleep precision, jitter measurement)
python tools/build_and_run.py --build-guest time_demo --set-elf time_demo
```

---

### Demo 9: System Health Monitor

**What it shows:** A watchdog-style health monitor that verifies all three subsystems (memory, network, filesystem) are functional in 5 diagnostic cycles.

```bash
python tools/build_and_run.py --build-guest watchdog_monitor --set-elf watchdog_monitor
```

---

## Build System Details

The build uses a two-pass process to link the symbol table:

1. **Compile Firmware** — standard ESP-IDF build.
2. **Export Symbols** — `tools/export_symbols.py` scans the ELF to find addresses of shim functions and writes `esp_all_symbol.c`.
3. **Recompile Firmware** — links the generated symbol table into the kernel.
4. **Compile Guest Apps** — builds guest ELFs with `-fPIC -nostdlib -shared`.
5. **Pack Filesystem** — creates a LittleFS image containing the guest ELFs.
6. **Merge & Pad** — combines bootloader, partition table, kernel, and FS into a 4 MB QEMU image.

### Cross-Platform Guest Build (tools/Makefile.guest)

```bash
# Linux/macOS/Windows
make -f tools/Makefile.guest APP=apps/c2_payload

# Custom compiler
CC=xtensa-esp32-elf-gcc make -f tools/Makefile.guest APP=apps/c2_payload
```

## Documentation

- **[Comprehensive Project Guide](documentation/comprehensive_project_guide.md)** — deep dive into architecture, memory model, and syscall implementation.
- **[Master Thesis Reference](documentation/MASTER_THESIS_ARCHITECTURAL_REFERENCE.md)** — formal architectural reference.
- **`presentation_full.pdf`** — full system design slide deck.
- **`improvements_only.pdf`** — slides covering recent additions (pipes, cross-platform build, heap tracking).
