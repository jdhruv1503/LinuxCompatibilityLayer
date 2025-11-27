# Task 03: Filesystem Syscall Shim Implementation

**Status:** Completed
**Date:** 2025-11-27

## Executive Summary
Implemented the POSIX filesystem compatibility layer ("shims") allowing guest ELF binaries to perform file operations (`open`, `read`, `write`, `stat`, etc.). This layer handles the translation between the guest's root-relative paths (e.g., `/var/log`) and the host's LittleFS mount point (`/linux/var/log`).

## Technical Implementation

### 1. Syscall Shim Layer (`main/syscalls/shim_unistd.c`)
Created a translation layer that sits between the Guest ELF and the ESP-IDF VFS.

*   **Path Translation:** Intercepts paths starting with `/`.
    *   Guest: `/file.txt` -> Host: `/linux/file.txt`
    *   Guest: `file.txt` -> Host: `/linux/file.txt` (Relative paths rooted at mount point)
    *   Guest: `/dev/console` -> Host: `/dev/console` (Passthrough for devices)
*   **Permission Faking:** LittleFS does not support POSIX permission bits.
    *   `shim_stat`/`shim_fstat`: Manually ORs `0777` (rwxrwxrwx) into `st_mode`. This prevents Linux apps from crashing with "Permission Denied".
*   **Standard I/O Interception:**
    *   Implemented `shim_fopen`. Since guest ELFs link dynamically to the host's `fopen`, the host's `fopen` would receive raw guest paths and fail. We mapped guest `fopen` -> `shim_fopen` -> translate path -> host `fopen`.

### 2. Symbol Export Mechanism
The ELF loader requires a symbol table to resolve function calls from the guest.

*   **Component Override:** The `espressif/elf_loader` component has a built-in `g_customer_elfsyms` table in `src/esp_all_symbol.c`. Creating a separate `custom_symbols.c` in `main` caused symbol shadowing/linking issues.
*   **Solution:** Updated `tools/export_symbols.py` to generate the C file directly into `managed_components/espressif__elf_loader/src/esp_all_symbol.c`.
*   **Symbol Aliasing:** The generator now supports mapping internal names to external names (e.g., `shim_open` is exported as `open`).

**Exported Symbols:**
*   **File Ops:** `open`, `close`, `read`, `write`, `lseek`, `ioctl`, `unlink`, `rename`, `access`.
*   **Dir Ops:** `mkdir`, `rmdir`, `opendir`, `readdir`, `closedir`.
*   **Env:** `getcwd`, `chdir`.
*   **Stdio:** `fopen` (shimmed), `fprintf`, `fread`, `fwrite`, `fclose`, `fflush`, `fseek`, `ftell`.
*   **Process:** `getpid`, `getppid`, `execve`, `execv`, `execvp`, `waitpid`, `wait`, `exit`, `_exit`, `abort`, `signal`, `raise`, `kill`.

### 3. Build System Configuration
*   **Linker Stripping:** The linker removes functions that are not called by the firmware itself. Since shims are *only* called by guest ELFs (looked up via symbol table), they were being stripped.
*   **Solution:** Added `-u <symbol>` flags to `target_link_libraries` in `main/CMakeLists.txt` to force inclusion.
    ```cmake
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u shim_open" "-u shim_read" ...)
    ```

## Verification

**Test Payload:** `apps/test_fs`
*   **Operation:**
    1.  Opens `/guest_log.txt` using `fopen` (High-level API).
    2.  Writes "Hello..." string.
    3.  Closes file.
    4.  Re-opens `/guest_log.txt` using `open` (Low-level API).
    5.  Reads content and prints to stdout.

**Simulation Output (QEMU):**
```
I (3495) kernel_main: Loading ELF: /linux/test_fs.elf
...
Guest: Attempting to write to filesystem...
Guest: Read back -> 'Hello from the Guest ELF via Shim!'
I (4125) kernel_main: ELF execution completed, return value: 0
```

### 4. Process Management (waitpid/wait)

**Implementation:** Added POSIX-compliant process waiting mechanisms to support the spawn model.

*   **Process Tracking:** Maintains a linked list of child processes spawned via `execve`, tracking:
    *   Process ID (PID) - derived from FreeRTOS task handle
    *   Task handle for the spawned process
    *   Completion semaphore for synchronization
    *   Exit status when process completes
*   **waitpid(pid, status, options):** Waits for a specific child process to complete
    *   Supports `pid == -1` to wait for any child (delegates to `wait`)
    *   Blocks until child process signals completion
    *   Returns exit status in `status` parameter
    *   Cleans up child process entry after waiting
*   **wait(status):** Simplified version that waits for any child process
    *   Finds first available child (preferring completed ones)
    *   Calls `waitpid` internally

**Key Design:**
- Child processes are tracked from creation in `execve` until completion
- Completion is signaled via semaphore when the ELF task finishes
- Thread-safe with mutex protection for the child process list
- Supports proper parent-child synchronization in the spawn model

**Usage in C2 Bot:**
The C2 bot uses `waitpid(-1, &status, 0)` to wait for payload execution to complete before closing the socket connection, ensuring all output is captured.

## Lessons Learned
1.  **Host `fopen` Trap:** You cannot simply export `open`. If the guest uses `fopen`, it calls the host's `fopen` directly (dynamic linking), bypassing the `open` shim. You must shim `fopen` as well.
2.  **QEMU Automation:** QEMU does not exit automatically if the guest is idle. A Python wrapper with a timeout is essential for automated testing.
3.  **LittleFS Limits:** LittleFS has no concept of users/groups. Permission checks must be spoofed in `stat`.
4.  **Process Synchronization:** The spawn model requires explicit waiting mechanisms. `waitpid` enables proper parent-child synchronization without true process isolation.
