# ESP32 Linux Compatibility Layer (TLCL)

A **Unikernel / Library OS** architecture for the ESP32 microcontroller that enables the dynamic loading and execution of standard Linux ELF binaries.

## Overview

This project bridges the gap between the static nature of embedded RTOS and the dynamic flexibility of Linux. By implementing a POSIX compatibility layer and an ELF loader on top of ESP-IDF/FreeRTOS, it allows developers to run standard C applications on the ESP32 without reflashing the firmware.

### Key Features

-   **ELF Loader:** Dynamic loading of relocatable binaries into IRAM/DRAM.
-   **System Call Shims:** POSIX-compliant implementation of `open`, `read`, `write`, `socket`, `execve`, etc.
-   **Virtual Filesystem:** UNIX-like `/dev/` nodes for hardware abstraction (e.g., `/dev/buzzer`, `/dev/c2`).
-   **No-MMU Support:** Runs in a single address space using a "Spawn" model instead of `fork`.
-   **Standard I/O:** Full support for `printf`, file operations, and BSD sockets.

## Project Structure

-   **`main/`**: The core firmware (Kernel) containing the ELF loader, syscall shims, and drivers.
-   **`apps/`**: Guest applications (Payloads) like `c2_payload` and `collision_guard`.
-   **`tools/`**: Build automation, symbol export scripts, and the C2 Master console.
-   **`components/`**: ESP-IDF components (LittleFS, ELF Loader).
-   **`documentation/`**: Detailed architectural docs and reports.

## Build System & Toolchain

The project utilizes a hybrid build system to handle the distinct requirements of the Host Firmware and the Guest Applications.

### 1. The Host Firmware (Kernel)
Built using the standard **ESP-IDF** build system (CMake + Ninja).
-   **Compiler:** `xtensa-esp32-elf-gcc` provided by Espressif.
-   **Linker:** Links against the FreeRTOS kernel, LwIP, and hardware abstraction layers.
-   **Output:** `linux_compat_layer.elf` (used for symbol generation) and `linux_compat_layer.bin` (flashed).

### 2. The Guest Applications (Payloads)
Built using a custom lightweight build script (`tools/build_guest_app.bat`).
-   **Compiler Flags:** `-fPIC` (Position Independent Code), `-mlongcalls` (Long jumps), `-nostdlib` (No standard library).
-   **Linker Flags:** `-shared` (Create shared object/DYN), `-e app_main` (Entry point).
-   **Linking Strategy:** Guest apps are *not* linked against the firmware. They contain undefined references (e.g., `printf`) which are resolved at *load time* by the ELF Loader against the Host's exported symbol table.

### 3. The Build Workflow
The `tools/build_and_run.py` script orchestrates the process:
1.  **Compile Firmware:** Standard build.
2.  **Export Symbols:** `tools/export_symbols.py` scans the Firmware ELF to find addresses of API functions (`open`, `socket`) and generates `esp_all_symbol.c`.
3.  **Recompile Firmware:** Links the generated symbol table into the kernel.
4.  **Compile Guest Apps:** Builds payload ELFs.
5.  **Pack Filesystem:** Creates a LittleFS image containing the Guest ELFs.
6.  **Merge & Pad:** Combines Bootloader, Partition Table, Kernel, and Filesystem into a single 4MB binary for QEMU.

## Execution Model (The Main Loop)

The ESP32 does not have a traditional OS kernel loop. Instead, it relies on the FreeRTOS scheduler.

1.  **Bootloader:** Loads the partition table and jumps to the Factory App (`main.c`).
2.  **`app_main` Task:**
    *   Initializes Hardware (NVS, WiFi, LittleFS).
    *   Registers VFS Drivers (`/dev/c2`, `/dev/collision`).
    *   Starts the C2 Server Task (listens on Port 9000).
3.  **Guest Execution:**
    *   When a payload is received, `execve` is called.
    *   **`shim_execve`**: Calls `xTaskCreate` to spawn a new FreeRTOS task.
    *   **New Task:** Allocates memory, loads ELF, performs relocations, and jumps to `app_main` of the Guest.
    *   **Concurrency:** The Guest runs as a standard RTOS task, preemptively scheduled alongside WiFi and System tasks.

## Build & Run

### Prerequisites
*   ESP-IDF v5.4+
*   Python 3.8+
*   QEMU for Xtensa

### Quick Start (Simulation)

**1. Standard Run (Builds everything and starts QEMU):**
```bash
python tools/build_and_run.py
```

**2. C2 Demo Mode (Interactive):**
Terminal 1 (The Node):
```bash
python tools/build_and_run.py --c2
```

Terminal 2 (The Master):
```bash
# Send the WiFi Scanner payload
python tools/c2_master.py build/guest_apps/c2_payload.elf localhost
```

## Demos

### Demo 1: Distributed Command & Control
Demonstrates the ability to send compiled code over the network to an edge node for immediate execution. The payload streams its stdout back to the master via a custom `dup2` redirection shim.

### Demo 2: Cyber-Physical Collision Avoidance
A "V2X" scenario where a Linux application receives simulated GPS/Speed telemetry via UDP, performs vector math to calculate Time-To-Collision, and triggers a physical hardware alarm (GPIO buzzer) via the `/dev/buzzer` kernel driver.

### Demo 3: Distributed Map-Reduce
Simulates a cluster of 4 ESP32 nodes performing a distributed calculation task, orchestrated by a central master script.

## Documentation

For a deep dive into the architecture, memory models, and syscall implementation, please see the **[Comprehensive Project Guide](documentation/comprehensive_project_guide.md)**.