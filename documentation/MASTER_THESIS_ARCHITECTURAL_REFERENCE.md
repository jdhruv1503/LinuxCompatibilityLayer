# ESP32 Thin Linux Compatibility Layer (TLCL): Architectural Dissertation & Implementation Reference

**Version:** 2.1.0-BuildSystemUpdated
**Target Architecture:** Xtensa LX6/LX7 (ESP32/S2/S3)
**Kernel Model:** Unikernel / Library OS
**Filesystem:** LittleFS
**Network Stack:** LwIP

---

## 1. Abstract & Theoretical Foundation

This document serves as a comprehensive architectural reference for the **Thin Linux Compatibility Layer (TLCL)**. It explores the implementation of a POSIX-compliant runtime environment on a microcontroller lacking a Memory Management Unit (MMU). This project fundamentally challenges the dichotomy between static embedded firmware and dynamic general-purpose operating systems by implementing a **Library OS** (or Unikernel) architecture.

### 1.1 The OS Taxonomy: Where TLCL Fits

To understand the architectural decisions, one must locate TLCL within the broader operating system taxonomy:

*   **Monolithic Kernel (Linux/Windows):** Kernel and drivers share a privileged address space. Applications run in isolated virtual address spaces (Ring 3). Interactions occur via software interrupts (syscalls). **Requires MMU.**
*   **Microkernel (Minix/L4):** Kernel provides minimal IPC. Drivers and filesystems run as user-space processes. **Requires MMU.**
*   **Real-Time Operating System (FreeRTOS/Zephyr):** Flat address space. Static linkage. No distinction between "Kernel" and "Application" memory. Deterministic scheduling.
*   **Library OS / Unikernel (MirageOS/IncludeOS/TLCL):** The operating system functionality is linked directly into the application (or provided as a shared service in a single address space). There is no protection boundary. System calls are function calls.

**TLCL Design Choice:** TLCL adopts the **Library OS** model on top of FreeRTOS. It provides the *API* of Linux (POSIX) without the *Architecture* of Linux (Virtual Memory).

---

## 2. Memory Architecture & The No-MMU Constraint

The fundamental constraint of the ESP32 is the lack of an MMU capable of supporting full virtual memory (paging, swapping, address remapping).

### 2.1 Harvard Architecture Implications
The Xtensa core utilizes a modified Harvard architecture with separate buses for Instructions and Data.
*   **IRAM (Instruction RAM):** `0x40080000` - `0x400A0000`.
    *   **Constraint:** Instructions *must* be fetched from here.
    *   **Access:** Accessible via Instruction Bus (R/X) and Data Bus (R/W 32-bit only).
    *   **Loader Complexity:** The ELF loader must parse section headers to identify `SHF_EXECINSTR`. These sections must be allocated via `heap_caps_malloc(..., MALLOC_CAP_EXEC)`.
2.  **DRAM (Data RAM):** `0x3FFB0000` - `0x3FFFFFFF`.
    *   **Constraint:** Non-executable. Used for `.data`, `.bss`, stack, and heap.
    *   **Access:** Accessible via Data Bus (R/W).

### 2.2 The "Address Space" Problem
In a Linux process, the application expects to start at a fixed Virtual Address (e.g., `0x8048000`).
*   **Scenario:** A pointer in the code references a global variable at `0x8049000`.
*   **ESP32 Reality:** That physical address does not exist or maps to a peripheral. We cannot map `0x8049000` to physical RAM.
*   **Solution:** **Position Independent Code (PIC)**.
    *   Binaries must be compiled with `-fPIC`.
    *   All data references use relative addressing or a Global Offset Table (GOT).
    *   The Loader performs **Load-Time Relocation**, adjusting pointers based on the actual physical base address where `malloc` placed the segment.

---

## 3. The ELF Loader Engine: Granular Analysis

The Loader acts as the dynamic linker and program initiator.

### 3.1 Parsing & Allocation Strategy
1.  **Header Verification:**
    *   Magic: `0x7F 'E' 'L' 'F'`
    *   Class: `ELFCLASS32` (32-bit)
    *   Data: `ELFDATA2LSB` (Little Endian)
    *   Machine: `EM_XTENSA` (`0x5E`)
    *   Type: `ET_DYN` (Shared Object). *Note: We cannot use `ET_EXEC` because it requires fixed absolute addresses.*

2.  **Segment Loading (`PT_LOAD`):**
    *   The loader iterates Program Headers.
    *   Calculates total memory required for contiguous segments to minimize fragmentation (advanced) or allocates per-segment (basic).
    *   **Critical Check:** If `p_flags & PF_X` (Executable), allocate from **IRAM**. Else, **DRAM**.
    *   **Zeroing:** If `p_memsz > p_filesz`, the remaining bytes (`.bss`) are `memset` to 0.

### 3.2 Xtensa-Specific Relocations
This is the most complex part of the implementation. The ESP32 uses the Xtensa ISA, which has specific relocation types handled in `elf_loader.c`.

*   **`R_XTENSA_RELATIVE` (Type 17):**
    *   **Use:** Global pointers initialized to addresses within the binary.
    *   **Math:** `*addr = *addr + load_bias`
    *   **Implementation:** Simple addition. The value at the relocation address is the compile-time offset; we add the physical base address.

*   **`R_XTENSA_SLOT0_OP` (Type 20):**
    *   **Use:** Direct `CALL`, `J`, `L32R` instructions.
    *   **Constraint:** Xtensa instructions use variable-length encoding (24-bit or 16-bit). The target offset is embedded *inside* the opcode bits.
    *   **Algorithm:**
        1.  Fetch instruction word (handle alignment).
        2.  Decode instruction format (using masking).
        3.  Extract the immediate field (e.g., top 18 bits signed).
        4.  Calculate: `Target_PA = (PC + Immediate) + load_bias`.
        5.  Recalculate immediate: `New_Immediate = Target_PA - Current_PC`.
        6.  Check for overflow (if the target is too far for the jump range).
        7.  Mask and insert the new immediate back into the opcode.
    *   **Caveat:** Requires `longcalls` (`-mlongcalls` flag) if the heap is fragmented and the jump distance exceeds the instruction's range (usually ±512KB).

### 3.3 Cache Coherency (The Trap)
The ESP32 pipeline has a hazard:
*   CPU I-Cache fetches instructions.
*   CPU Load/Store Unit writes data.
*   When the Loader writes the program code to IRAM, it goes through the Store buffer. The I-Cache may still hold "stale" lines (garbage or previous code) for that address range.
*   **Safety Mechanism:** `esp_cache_msync(addr, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M)` and `esp_cache_flush()`. This forces data from Cache to Main Memory and invalidates I-Cache tags, ensuring the CPU fetches the newly loaded instructions.

---

## 4. System Call Shim Layer: Implementation & Caveats

This layer maps the Guest's POSIX expectations to the Host's FreeRTOS/Newlib reality.

### 4.1 File System Shims (`shim_unistd.c`)

#### `open` / `fopen`
*   **Mapping:** Guest `open("/file")` -> Host `esp_vfs_open("/linux/file")`.
*   **Path Translation:**
    *   A buffer is allocated on the stack.
    *   If path starts with `/`, `/linux` is prepended.
    *   If path starts with `/dev/`, it is passed through (for drivers).
    *   Relative paths (no leading `/`) are prepended with the current working directory (CWD) maintained in a task-local variable.
*   **Caveat:** `fopen` in the guest dynamically links to `fopen` in the firmware. We must intercept this symbol and route it through `shim_fopen`, which performs the path translation before calling the real `fopen`.

#### `stat` / `fstat`
*   **The Problem:** LittleFS is a flat filesystem designed for microcontrollers. It stores file size and name, but **not** owners, groups, or permission bits (RWX).
*   **The Crash:** Standard C `fopen` often calls `fstat` to check if it has permissions. If `st_mode` returns 0, `fopen` fails with `EACCES`.
*   **The Fix:** The Shim "lies".
    ```c
    int shim_stat(const char *path, struct stat *st) {
        int ret = esp_vfs_stat(translated_path, st);
        if (ret == 0) {
            // Force Read/Write/Execute for User/Group/Other (0777)
            st->st_mode |= S_IRWXU | S_IRWXG | S_IRWXO;
        }
        return ret;
    }
    ```

### 4.2 Network Shims (`shim_socket.c`)

#### Error Code Translation
LwIP uses a distinct set of error codes internally. While modern ESP-IDF defines `errno.h` to match LwIP, discrepancies can occur depending on configuration.
*   **Mechanism:** After every LwIP call (`lwip_socket`, `lwip_recv`), the shim checks the global `errno`.
*   **Mapping:**
    *   `LwIP EWOULDBLOCK` -> `POSIX EAGAIN` (Critical for non-blocking I/O loops).
    *   `LwIP ENOTCONN` (118) -> `POSIX ENOTCONN`.

### 4.3 Stdout Redirection (`dup2`) - The "Global Socket" Approach

This is the most complex shim due to the static nature of the UART driver.

*   **The Goal:** Redirect `printf` (FD 1) to a TCP socket.
*   **The Obstacle:** FD 0, 1, 2 are hardcoded in the Newlib `_reent` structure to the UART driver. Closing FD 1 via `close(1)` often destabilizes the ESP-IDF logging system.
*   **The Architecture:**
    1.  **Shared State:** A global structure `c2_redirect_state_t` holds:
        *   `int socket_fd`: The target socket.
        *   `bool active`: Is redirection enabled?
    2.  **`shim_dup2(sock, 1)`:**
        *   Does **not** touch the VFS table.
        *   Updates `c2_redirect_state` with the `sock` FD.
        *   Returns `1` to satisfy POSIX API.
    3.  **`shim_write(fd, buf, len)`:**
        *   Intercepts **all** writes.
        *   Logic:
            ```c
            if (fd == STDOUT_FILENO && c2_state->active) {
                // Mirror to UART (so we can debug locally)
                real_write(fd, buf, len);
                // Send to Network (Non-blocking)
                send(c2_state->socket_fd, buf, len, MSG_DONTWAIT);
                return len;
            }
            ```
    4.  **`MSG_DONTWAIT`:** Critical. If the network drops, we don't want `printf` to block the entire system forever. We drop the packet and continue.

### 4.4 Process Control (`shim_process.c`)

#### The Spawn Model (`execve`)
Since `fork` is impossible:
1.  **Memory:** `execve` calls `xTaskCreate` to spawn a new FreeRTOS task.
2.  **Stack:** A fresh stack (default 8KB) is allocated for the new task.
3.  **Concurrency:** The parent task continues immediately (unless it calls `waitpid`). This mimics `posix_spawn`.

---

## 9. Build System & Toolchain Architecture

This project employs a complex, multi-stage build system to reconcile the embedded host firmware with dynamic guest applications.

### 9.1 The Two-Pass Build Problem
To allow Guest Applications to call Host API functions (`open`, `printf`) without linking the entire C library into every small ELF:
1.  **Symbol Table Generation:** We must know the *exact* physical address of `shim_open` in the final firmware binary.
2.  **Circular Dependency:** The symbol table (`esp_all_symbol.c`) is part of the firmware source. But we can't know the addresses until we link. And if we change the source code (add the table), the addresses change!

### 9.2 The Solution: `build_and_run.py` Workflow
We automate a multi-stage process:

1.  **Stage 1: Firmware Build (Preliminary):**
    *   Compile Host Firmware using `idf.py build`.
    *   Generates `linux_compat_layer.elf` (Host).
    *   This ELF contains the function addresses, but lacks the populated symbol table.

2.  **Stage 2: Symbol Extraction:**
    *   Script `tools/export_symbols.py` runs `xtensa-esp32-elf-nm`.
    *   It extracts addresses for whitelisted symbols (`shim_open`, `shim_socket`, `printf`).
    *   It generates C code: `const struct { char* name; void* addr; } symbols[] = { {"open", 0x400D1234}, ... };`
    *   This file overwrites `components/.../esp_all_symbol.c`.

3.  **Stage 3: Firmware Build (Final):**
    *   Re-run `idf.py build`. The compiler now links the populated symbol table.
    *   *Note:* Adding the table shifts addresses slightly, but since the table is essentially data, code alignment usually preserves function pointers. If critical shift happens, a third pass might be needed (rare).

4.  **Stage 4: Guest Application Build:**
    *   Uses `tools/build_guest_app.bat`.
    *   **Compiler:** `xtensa-esp32-elf-gcc`.
    *   **Flags:** `-fPIC -mlongcalls -nostdlib`.
    *   **Linker:** `-shared` (ET_DYN).
    *   **Result:** A small `.elf` file with undefined references to `printf` etc.

5.  **Stage 5: Filesystem Packing:**
    *   Tool `mklittlefs` takes the Guest ELFs and packs them into `linux_fs.bin`.

6.  **Stage 6: Merging & Padding:**
    *   `esptool` combines Bootloader + Partition Table + Firmware + Filesystem into `merged-flash.bin`.
    *   Padded to exactly 4MB for QEMU.

---

## 10. Runtime Execution Model (The Main Loop)

The "Kernel" is not a `while(1)` loop in the traditional OS sense. It is a set of FreeRTOS tasks managed by the preemptive scheduler.

### 10.1 `app_main` Initialization Sequence
1.  **Hardware Init:** NVS Flash (storage), Event Loop (signals).
2.  **Filesystem Mount:** `esp_vfs_littlefs_register`. Maps the flash partition to `/linux`.
3.  **Driver Init:** `vfs_collision_register`, `vfs_buzzer_register`. Creates the `/dev` nodes.
4.  **Network Init:** Connects to WiFi or initializes OpenEth (if QEMU). Waits for DHCP IP.
5.  **Service Start:** Launches the **C2 Server Task**.

### 10.2 The C2 Server Task (PID 0 equivalent)
This task acts as the `init` process.
*   Listens on TCP Port 9000.
*   **Accept:** On connection, receives ELF payload.
*   **Write:** Streams payload to `/linux/payload.elf`.
*   **Execute:** Calls `elf_loader_run("/linux/payload.elf")`.

### 10.3 Task Lifecycle during `execve`
When the C2 Server calls `elf_loader_run`:
1.  **Loader:** Allocates IRAM/DRAM. Loads code. Relocates.
2.  **Invocation:** Calls the Guest's `app_main`.
3.  **Context:** The Guest runs *in the context* of the C2 Server Task (or a spawned child task).
4.  **Termination:** When `app_main` returns, the Loader frees the memory and the task deletes itself.

---

## 11. Conclusion

The TLCL successfully demonstrates that the rigid boundaries of embedded systems can be softened. By stripping the Operating System down to its API (POSIX) and implementing the loader and scheduling logic in the user-space of an RTOS, we achieve a hybrid system: **The dynamism of Linux with the hardware footprint of a Microcontroller.**
