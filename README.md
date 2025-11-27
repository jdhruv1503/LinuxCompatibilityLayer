# ESP32 Linux Compatibility Layer (TLCL)

A **Unikernel / Library OS** architecture for the ESP32 microcontroller that enables the dynamic loading and execution of standard Linux ELF binaries.

## 🚀 Overview

This project bridges the gap between the static nature of embedded RTOS and the dynamic flexibility of Linux. By implementing a POSIX compatibility layer and an ELF loader on top of ESP-IDF/FreeRTOS, it allows developers to run standard C applications on the ESP32 without reflashing the firmware.

### Key Features

-   **ELF Loader:** Dynamic loading of relocatable binaries into IRAM/DRAM.
-   **System Call Shims:** POSIX-compliant implementation of `open`, `read`, `write`, `socket`, `execve`, etc.
-   **Virtual Filesystem:** UNIX-like `/dev/` nodes for hardware abstraction (e.g., `/dev/buzzer`, `/dev/c2`).
-   **No-MMU Support:** Runs in a single address space using a "Spawn" model instead of `fork`.
-   **Standard I/O:** Full support for `printf`, file operations, and BSD sockets.

## 📂 Project Structure

-   **`main/`**: The core firmware (Kernel) containing the ELF loader, syscall shims, and drivers.
-   **`apps/`**: Guest applications (Payloads) like `c2_payload` and `collision_guard`.
-   **`tools/`**: Build automation, symbol export scripts, and the C2 Master console.
-   **`components/`**: ESP-IDF components (LittleFS, ELF Loader).
-   **`documentation/`**: Detailed architectural docs and reports.

## 🛠️ Build & Run

### Prerequisites
*   ESP-IDF v5.4+
*   Python 3.8+
*   QEMU for Xtensa

### Quick Start (Simulation)

We provide a comprehensive wrapper tool `tools/build_and_run.py` to handle the multi-step build process (Build Firmware -> Export Symbols -> Rebuild Firmware -> Build Guest Apps -> Merge Binaries -> Pad Flash -> Run QEMU).

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

## 🎮 Demos

### Demo 1: Distributed Command & Control
Demonstrates the ability to send compiled code over the network to an edge node for immediate execution. The payload streams its stdout back to the master via a custom `dup2` redirection shim.

### Demo 2: Cyber-Physical Collision Avoidance
A "V2X" scenario where a Linux application receives simulated GPS/Speed telemetry via UDP, performs vector math to calculate Time-To-Collision, and triggers a physical hardware alarm (GPIO buzzer) via the `/dev/buzzer` kernel driver.

### Demo 3: Distributed Map-Reduce
Simulates a cluster of 4 ESP32 nodes performing a distributed calculation task, orchestrated by a central master script.

## 📚 Documentation

For a deep dive into the architecture, memory models, and syscall implementation, please see the **[Comprehensive Project Guide](documentation/comprehensive_project_guide.md)**.
