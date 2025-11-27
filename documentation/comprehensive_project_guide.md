# ESP32 Thin Linux Compatibility Layer (TLCL): Comprehensive System Documentation

## 1. Executive Summary

The **ESP32 Thin Linux Compatibility Layer (TLCL)** is a specialized runtime environment that enables the execution of standard, dynamically linked Linux ELF (Executable and Linkable Format) binaries on the Espressif ESP32 microcontroller. 

Traditionally, embedded systems (RTOS) and general-purpose operating systems (Linux) occupy distinct worlds. RTOS offers determinism and low resource usage but requires monolithic firmware compilation. Linux offers dynamic application loading and rich APIs but requires powerful hardware with a Memory Management Unit (MMU) and megabytes of RAM.

This project bridges that gap by implementing a **Unikernel / Library OS** architecture. It allows developers to write standard C programs using POSIX system calls (`open`, `read`, `socket`, `printf`), compile them with standard GCC tools, and execute them on the ESP32 without reflashing the firmware.

### Key Capabilities
*   **Dynamic Loading:** Load `.elf` files from the filesystem (LittleFS) into RAM and execute them.
*   **POSIX Compatibility:** Provides a translation layer ("shims") that maps Linux system calls to ESP-IDF/FreeRTOS primitives.
*   **Standard I/O:** Supports `printf`, file I/O, and BSD sockets.
*   **Hardware Abstraction:** Exposes hardware sensors and actuators via the UNIX `/dev/` file model.
*   **No-MMU Architecture:** Runs in a single address space, using a "Spawn" model instead of `fork()`.

---

## 2. Architectural Background & Core Concepts

To understand this project, one must understand the fundamental differences between a full OS and an MCU environment.

### 2.1 The MMU Constraint
*   **Linux (Virtual Memory):** In a standard OS, every process believes it owns the entire memory space starting at address `0x00`. The hardware MMU translates these *Virtual Addresses* to *Physical RAM* addresses. This allows features like `fork()` (Copy-on-Write), where a child process shares the parent's memory until it writes to it.
*   **ESP32 (Physical Memory):** The ESP32 has no MMU for RAM remapping. It uses a flat physical address space.
    *   **Implication 1:** We cannot implement `fork()`. We cannot clone a process's address space because we can't remap the pointers.
    *   **Implication 2:** Binaries must be **Position Independent (PIE)**. They cannot assume they are loaded at a fixed address. They must be relocated at load time.
    *   **Implication 3:** No memory protection. A guest app writing to a null pointer creates a hardware exception that crashes the entire system (Firmware + App).

### 2.2 The Unikernel Model
Instead of a kernel managing isolated user-space processes, the TLCL operates as a **Unikernel** (or Library OS). 
*   The "Guest Application" is effectively a dynamically loaded library.
*   It runs with the same privileges as the kernel (Ring 0 equivalent).
*   System calls are not software interrupts (like `int 0x80` or `syscall` instruction). They are direct function calls to the firmware's "Shim" layer.

### 2.3 ELF (Executable and Linkable Format)
The ELF is the standard binary format for Unix systems. It contains:
*   **.text segment:** Machine code (Instructions). On ESP32, this MUST go into **IRAM** (Instruction RAM) to be executable.
*   **.data segment:** Initialized variables. Goes to **DRAM** (Data RAM).
*   **.bss segment:** Uninitialized variables. Goes to DRAM.
*   **Relocation Tables:** Instructions on how to patch the code once the load address is known.
*   **Symbol Tables:** Names of functions the binary needs (imports) or provides (exports).

---

## 3. System Architecture

### 3.1 High-Level Diagram

```
+---------------------------------------------------------------+
|                     Guest ELF Application                     |
|   (printf, socket, open, read, write, /dev/sensor logic)      |
+------------------------------+--------------------------------+
                               | Direct Function Calls (Dynamic)
                               v
+---------------------------------------------------------------+
|                      Syscall Shim Layer                       |
|  (shim_unistd.c, shim_socket.c, shim_process.c)               |
|  - Translates Paths (/ -> /linux/)                            |
|  - Translates Errno (LwIP -> POSIX)                           |
|  - Manages File Descriptors (dup2, redirects)                 |
+------------------------------+--------------------------------+
                               |
          +--------------------+---------------------+
          |                                          |
          v                                          v
+----------------------+                   +--------------------+
|   ESP-IDF Components |                   |    VFS Drivers     |
|  (LwIP, VFS, Driver) |                   | (/dev/c2, buzzer)  |
+----------------------+                   +--------------------+
          |                                          |
          v                                          v
+---------------------------------------------------------------+
|                        Hardware / QEMU                        |
+---------------------------------------------------------------+
```

### 3.2 The ELF Loader Engine
Located in `main/main.c` and utilizing `espressif/elf_loader`, this component is the heart of the system.
1.  **Allocates Memory:** Uses `heap_caps_malloc` to get IRAM for code and DRAM for data.
2.  **Loads Sections:** Copies bytes from the file to RAM.
3.  **Relocates:** Iterates through the ELF relocation tables.
    *   *R_XTENSA_RELATIVE:* Adds the load offset to pointers.
    *   *R_XTENSA_SLOT0_OP:* Patches machine instructions (calls/jumps) to point to the correct absolute address.
4.  **Resolves Symbols:** When the ELF calls `printf`, the loader looks up "printf" in the firmware's **Exported Symbol Table** (`esp_all_symbol.c`) to find the address of `shim_printf`.

### 3.3 The Shim Layer
Located in `main/syscalls/`. This adapts the Guest's expectations to the Host's reality.

*   **Filesystem (`shim_unistd.c`):**
    *   Guest asks for `/log.txt`. Shim translates to `/linux/log.txt` (LittleFS mount point).
    *   Guest calls `stat`. LittleFS has no permissions. Shim fakes `0777` (rwx) permissions so the app doesn't crash.
*   **Networking (`shim_socket.c`):**
    *   Wraps LwIP functions.
    *   Translates LwIP specific error codes to standard `errno` values.
*   **Process (`shim_process.c`):**
    *   **Spawn Model:** `execve` creates a new FreeRTOS task to run the ELF.
    *   **Wait:** `waitpid` uses FreeRTOS semaphores to block until the child task deletes itself.

---

## 4. Detailed File Structure Analysis

### 4.1 Project Root
*   **`CMakeLists.txt`**: Top-level build script. Sets up the project partition table.
*   **`sdkconfig.defaults`**: Critical build configuration.
    *   `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`: QEMU requirement.
    *   `CONFIG_PARTITION_TABLE_CUSTOM=y`: Enables our filesystem.
*   **`partitions.csv`**: Defines the flash layout.
    *   `factory`: The firmware code (1.5MB).
    *   `linux_fs`: The LittleFS data partition (1.5MB) where ELFs live.
*   **`GEMINI.md`**: Context file for the AI assistant.

### 4.2 `main/` Directory (The Firmware)
*   **`main.c`**: The kernel entry point.
    *   Initializes Drivers, WiFi, LittleFS.
    *   Registers VFS drivers.
    *   Can run a default ELF or enter C2 Server mode.
*   **`syscalls/`**: The translation layer.
    *   `shim_unistd.c`: `open`, `read`, `write`, `dup2`.
    *   `shim_socket.c`: BSD socket wrappers.
    *   `shim_process.c`: `execve`, `waitpid`, `exit`.
*   **`drivers/`**: Hardware abstraction.
    *   `drv_network.c`: Init OpenEth (QEMU) or EMAC (Hardware).
    *   `drv_fs_littlefs.c`: Mounts the filesystem.
    *   `drv_devices.c`: Initializes `/dev/` nodes.
*   **`vfs_drivers/`**: Custom "Device Files".
    *   `vfs_c2_pipe.c`: The logic behind `/dev/c2` (stdout redirection).

### 4.3 `apps/` Directory (The Guest Software)
These are standard C programs compiled independently of the firmware.
*   **`hello_world/`**: Simple test.
*   **`c2_payload/`**: Demo 1 payload. WiFi scanner simulation.
*   **`collision_guard/`**: Demo 2 payload. V2X vector math.
*   **`c2_bot/`**: The C2 server itself, refactored as a guest app.
*   **`map_reduce_worker/`**: Distributed computing worker.

### 4.4 `tools/` Directory (The Dev Environment)
*   **`build_and_run.py`**: The master automation script.
    *   Builds firmware.
    *   Builds filesystem image.
    *   Merges binaries.
    *   Pads to 4MB (QEMU requirement).
    *   Runs QEMU with timeouts and port forwarding.
*   **`export_symbols.py`**: Scans the firmware ELF, finds addresses of functions like `printf` or `shim_open`, and generates a C table so the loader can link against them.
*   **`c2_master.py`**: The "Attacker" console. Connects to the ESP32 to send payloads and view output.

---

## 5. Technical Deep Dives

### 5.1 Stdout Redirection (The `dup2` Hack)
In Linux, `dup2(socket, 1)` makes `stdout` write to a socket. On ESP32, `stdout` (FD 1) is hardwired to the UART driver in the C library (`_reent` struct). You can't easily close/swap it without breaking the system console.

**Our Solution:**
1.  **Global State:** We keep a pointer `g_c2_redirect_state` visible to the shim.
2.  **Shim Interception:** `shim_write` checks: "Is FD==1? Is redirection on?".
3.  **Dual Routing:** If yes, it sends the data to the UART (for debugging) AND to the socket stored in the state.
4.  **Shim dup2:** It doesn't actually manipulate the kernel FD table. It just updates the `g_c2_redirect_state` with the new socket FD.

### 5.2 The "Spawn" Model
Since `fork()` is impossible:
1.  **Parent:** Calls `execve("program.elf", ...)`.
2.  **Shim:**
    *   `xTaskCreate()`: Spawns a new FreeRTOS thread.
    *   Passes arguments to the thread.
    *   Returns `0` (Success) to the parent immediately (or waits if using `waitpid`).
3.  **Child:**
    *   Allocates memory.
    *   Loads ELF.
    *   Runs `main()`.
    *   When `main` returns, calls `vTaskDelete(self)`.

### 5.3 Driver Abstraction (/dev nodes)
We use the ESP-IDF VFS interface to register custom drivers.
*   **Open:** `open("/dev/buzzer", ...)` -> calls `buzzer_open`.
*   **Ioctl:** `ioctl(fd, CMD, arg)` -> calls `buzzer_ioctl` to set frequency.
*   **Read:** `read(fd, ...)` -> Blocking read. Uses a FreeRTOS queue to wait for interrupt data.

---

## 6. The Demos

### Demo 1: Distributed C2 Botnet
*   **Concept:** ESP32 as an ephemeral execution node.
*   **Workflow:**
    1.  PC runs `c2_master.py`.
    2.  Sends `c2_payload.elf` (WiFi Scanner logic) to ESP32 port 9000.
    3.  ESP32 saves it, loads it, runs it.
    4.  Payload output is streamed back to PC via the Redirection Shim.
    5.  Payload exits, ESP32 cleans up memory.

### Demo 2: Cyber-Physical System (Collision Avoidance)
*   **Concept:** Edge computing logic controlling hardware.
*   **Workflow:**
    1.  Web Dashboard (JS) sends GPS/Speed JSON to ESP32 HTTP server.
    2.  ESP32 Firmware bridges JSON to UDP packets (localhost).
    3.  **Guest App (`collision_guard.elf`)** reads UDP.
    4.  App performs vector math (relative velocity, Time-To-Collision).
    5.  If TTC < Threshold, App writes "1" to `/dev/buzzer`.
    6.  Firmware Driver toggles GPIO.

### Demo 3: Distributed Map-Reduce
*   **Concept:** Cluster computing on microcontrollers.
*   **Workflow:**
    1.  Host spins up 4 QEMU instances (Ports 9001-9004).
    2.  Master script generates data (1-1000).
    3.  Splits data into 4 chunks.
    4.  Sends `map_reduce_worker.elf` + Data Chunk to each node simultaneously.
    5.  Workers calculate Sum/Count and return results.
    6.  Master aggregates results.

---

## 7. Caveats and Nuances

1.  **QEMU Networking:** ESP32 QEMU uses SLIRP (User Mode networking).
    *   It provides a NAT. The ESP32 is behind a virtual router.
    *   You cannot connect *to* the ESP32 from the host unless you use `hostfwd` (Port Forwarding).
    *   ESP32 cannot connect to the host's `localhost`. It must use the gateway IP (`10.0.2.2`).
2.  **Memory Fragmentation:** Repeatedly loading/unloading ELFs can fragment the heap (especially IRAM). In a real production system, a custom allocator or frequent reboots might be needed.
3.  **Security:** This is a "Library OS". There is no kernel/user mode separation. A malicious ELF can read/write any memory address, crash the system, or steal WiFi keys from the firmware memory.
4.  **Floating Point:** The ESP32 FPU is single-precision. Double precision math is software-emulated and slow. Guest apps should use `float`, not `double`.

## 8. Build Instructions

### Prerequisites
*   ESP-IDF v5.4+
*   Python 3.8+
*   QEMU for Xtensa (via ESP-IDF tools)

### Building and Running
We use a wrapper script to handle the complex workflow (Build -> Export Symbols -> Rebuild -> Merge -> Pad -> Run).

**Standard Simulation:**
```bash
python tools/build_and_run.py
```

**C2 Demo Mode (Interactive):**
```bash
# Terminal 1: Start the Node
python tools/build_and_run.py --c2

# Terminal 2: Send Payload
python tools/c2_master.py build/guest_apps/c2_payload.elf localhost
```

**Clean Build:**
```bash
python tools/build_and_run.py --clean --build
```
