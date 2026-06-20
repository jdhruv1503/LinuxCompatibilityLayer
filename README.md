# ESP32 Linux Compatibility Layer (TLCL)

A **Unikernel / Library OS** architecture for the ESP32 microcontroller that enables the dynamic loading and execution of standard Linux ELF binaries — without reflashing the firmware.

## Overview

This project bridges the gap between the static nature of embedded RTOS and the dynamic flexibility of Linux. By implementing a POSIX compatibility layer and an ELF loader on top of ESP-IDF/FreeRTOS, it allows developers to run standard C applications on the ESP32 without reflashing the firmware.

### Key Features

- **ELF Loader:** Dynamic loading of relocatable binaries into IRAM/DRAM.
- **System Call Shims:** POSIX-compliant implementation of `open`, `read`, `write`, `socket`, `execve`, `pipe`, `clock_gettime`, etc. (20+ syscalls).
- **Virtual Filesystem:** UNIX-like `/dev/` nodes for hardware abstraction (`/dev/buzzer`, `/dev/c2`).
- **No-MMU Support:** Runs in a single address space using a "Spawn" model instead of `fork`.
- **Standard I/O:** Full support for `printf`, file operations, and BSD sockets.
- **Per-Guest Heap Tracking:** Memory accounting per loaded ELF (`lcl ps` command).

## Presentations & Reports

Pre-compiled PDFs are included in the repository root:

| File | Description |
|------|-------------|
| `presentation_full.pdf` | Full architecture presentation (14 slides) |
| `improvements_only.pdf` | Recent improvements presentation (16 slides) |
| `project_presentation.pdf` | Summary slides (8 slides) |

## Project Structure

- **`main/`**: Core firmware — ELF loader, syscall shims, VFS drivers.
- **`apps/`**: Guest ELF applications (`c2_payload`, `c2_bot`, `map_reduce_worker`, `collision_guard`, etc.).
- **`tools/`**: Build automation, demo controllers, and test scripts.
- **`components/`**: ESP-IDF components (ELF Loader, LittleFS).
- **`documentation/`**: Architecture docs, task outputs, and LaTeX sources.

## Quick Start

**All build and simulation tasks use `tools/build_and_run.py`.** It handles the two-pass build, symbol export, flash padding, and QEMU timeout automatically.

### Prerequisites

- ESP-IDF v5.4 (at `C:\Users\Dhruv\.esp-tools\esp-idf` on Windows)
- Python 3.8+
- QEMU for Xtensa (via `idf_tools.py`)

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

## Demos

### Demo 1: Distributed C2 Command & Control

**What it shows:** Send a compiled Linux ELF binary over TCP to an ESP32 node. The node loads and executes it immediately, streaming `stdout` back to the sender via a custom `dup2` redirection shim.

**Architecture:**
```
Master (PC) --[TCP: ELF bytes]--> ESP32 C2 Bot --> execve() --> Guest ELF runs
                                                                      |
Master receives stdout <----[TCP: printf output]-------- shim_write intercepts fd=1
```

**Run the demo:**

Terminal 1 — build and start the C2 bot firmware in QEMU:
```bash
python tools/build_and_run.py --build-guest c2_bot --set-elf c2_bot --timeout 120
```

Terminal 2 — build the payload and send it to the node:
```bash
python tools/build_and_run.py --build-guest c2_payload
python tools/c2_master.py build/guest_apps/c2_payload.elf localhost
```

The master script connects to port 9000, uploads the ELF, and displays the guest's `printf` output in real time.

---

### Demo 2: Distributed Map-Reduce

**What it shows:** Spin up 4 ESP32 QEMU instances, each running the C2 bot. The `c2_master.py` controller distributes a computation task (e.g., sum of squares) across all nodes, collects results, and aggregates them — a distributed map-reduce on microcontrollers.

**Architecture:**
```
c2_master.py
  ├── QEMU Node 0 (port 9001) --> map_reduce_worker ELF --> result
  ├── QEMU Node 1 (port 9002) --> map_reduce_worker ELF --> result
  ├── QEMU Node 2 (port 9003) --> map_reduce_worker ELF --> result
  └── QEMU Node 3 (port 9004) --> map_reduce_worker ELF --> result
         <-- aggregate -->
```

**Run the demo:**

Step 1 — build the firmware and the map-reduce worker:
```bash
python tools/build_and_run.py --build-guest map_reduce_worker --build
```

Step 2 — launch the multi-node cluster controller:
```bash
# Automated test (spawns 4 QEMU instances, runs map-reduce, exits)
python tools/c2_master.py --auto --timeout 40

# Interactive mode (split-screen UI; press r=run, m=math, c=clear, q=quit)
python tools/c2_master.py

# Custom: 2 nodes, run math benchmark
python tools/c2_master.py --nodes 2 --math
```

The controller opens a split-screen terminal view showing live output from all nodes simultaneously.

---

### Demo 3: Cyber-Physical Collision Avoidance (V2X)

**What it shows:** A Linux application running on ESP32 receives real-time vehicle telemetry (GPS position + speed) via UDP, computes Time-To-Collision using vector math, and triggers a physical hardware alarm by writing to `/dev/buzzer` — a custom VFS driver that controls a PWM GPIO.

**Architecture:**
```
test_collision.py --[UDP telemetry]--> collision_guard ELF (running on ESP32)
                                            |
                                    vector math: TTC = |P_rel| / |V_rel|
                                            |
                                    write("/dev/buzzer", ...) --> PWM GPIO
```

**Run the demo:**

Step 1 — build and run the collision guard firmware:
```bash
python tools/build_and_run.py --build-guest collision_guard --set-elf collision_guard --timeout 60
```

Step 2 — send simulated telemetry from the test script:
```bash
# Sends three scenarios: safe → warning → collision
python tools/test_collision.py --host localhost --port 9000
```

Watch the QEMU console for collision warnings and buzzer activations.

---

## Build System Details

The build uses a two-pass process to link the symbol table:

1. **Compile Firmware** — standard ESP-IDF build.
2. **Export Symbols** — `tools/export_symbols.py` scans the ELF to find addresses of shim functions (`open`, `socket`, `printf`, etc.) and writes `esp_all_symbol.c`.
3. **Recompile Firmware** — links the generated symbol table into the kernel.
4. **Compile Guest Apps** — builds guest ELFs with `-fPIC -nostdlib -shared`.
5. **Pack Filesystem** — creates a LittleFS image containing the guest ELFs.
6. **Merge & Pad** — combines bootloader, partition table, kernel, and FS into a 4 MB QEMU image.

### Guest App Compilation

```bash
# Build a specific guest app
python tools/build_and_run.py --build-guest <app_name>

# Build and set as the default ELF loaded at boot
python tools/build_and_run.py --build-guest <app_name> --set-elf <app_name>
```

Available guest apps: `hello_world`, `c2_bot`, `c2_payload`, `map_reduce_worker`, `collision_guard`.

## Documentation

- **[Comprehensive Project Guide](documentation/comprehensive_project_guide.md)** — deep dive into architecture, memory model, and syscall implementation.
- **[Master Thesis Reference](documentation/MASTER_THESIS_ARCHITECTURAL_REFERENCE.md)** — formal architectural reference.
- **`presentation_full.pdf`** — slide deck covering the full system design.
- **`improvements_only.pdf`** — slides covering recent additions (pipes, cross-platform build, heap tracking).
