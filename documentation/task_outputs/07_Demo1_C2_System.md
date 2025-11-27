# Task 07: Demo1 - Distributed Command & Control System

## Overview

This task implements a **Distributed Command & Control (C2) system** that demonstrates the Linux Compatibility Layer's capability to:
1. Receive ELF payloads over TCP network
2. Dynamically load and execute them
3. Stream stdout output back to the client in real-time

The system implements a "Master-Bot" architecture where:
- **ESP32 (Bot)**: Listens on TCP port 9000 for payload submissions
- **PC/Host (Master)**: Python script that connects, sends compiled ELF binaries, and displays output

## Architecture

### Components

#### 1. **C2 Server (`main/c2_server.c`)**
- FreeRTOS task running in background
- Listens on TCP port 9000 for incoming connections
- Protocol:
  1. Receive 4-byte size header (little-endian unsigned int)
  2. Stream ELF binary data to file (`/linux/payload.elf`)
  3. Execute ELF with stdout redirection
  4. Send completion message
  5. Close connection and cleanup

**Key Features:**
- Non-blocking payload reception with progress logging
- Configurable max payload size (1MB default)
- Graceful error handling
- Thread-safe redirection management

#### 2. **C2 Master (`tools/c2_master.py`)**
- Python client script for sending payloads
- Command-line interface with IP, port, and file arguments
- Receives and displays execution output in real-time
- Supports quiet mode for minimal logging

**Usage:**
```bash
python tools/c2_master.py <payload.elf> <ESP_IP> [-p PORT] [-q]

Examples:
  python tools/c2_master.py build/guest_apps/c2_payload.elf 192.168.1.105
  python tools/c2_master.py payload.elf localhost                    # QEMU
  python tools/c2_master.py payload.elf 10.0.2.15 -p 9000 -q         # Quiet mode
```

#### 3. **Payload Application (`apps/c2_payload/main.c`)**
- Demo application simulating a WiFi scanner
- Uses standard C library functions: `printf`, `puts`, `sleep`
- Outputs table-formatted network scan results
- Returns exit code to demonstrate full lifecycle

#### 4. **Stdout Redirection Mechanism**

The C2 system uses a **shared state pointer** for reliable inter-module communication:

**State Structure:**
```c
typedef struct {
    int socket_fd;           // TCP socket for C2 master
    bool redirect_stdout;    // Enable stdout redirection
    bool redirect_stderr;    // Enable stderr redirection
} c2_redirect_state_t;
```

**Implementation:**
- Allocated in `shim_unistd.c` as static struct with stable address
- Exported as `g_c2_redirect_state` pointer
- `c2_bot` (guest app) uses `dup2()` to set redirection
- `shim_dup2()` sets both the redirect flags AND the socket_fd in shared state
- `shim_printf/shim_puts` check state and dual-route output:
  1. UART (local console for debugging)
  2. TCP socket (network streaming to master)
- Non-blocking socket sends (`MSG_DONTWAIT`) prevent app crashes if connection drops

**Key Insight:** Using a **single pointer to a stable memory location** instead of separate globals avoids synchronization issues.

**Critical Fix:** `shim_dup2()` now properly sets `g_c2_redirect_state->socket_fd = oldfd` when redirecting stdout/stderr, ensuring `shim_puts` and `shim_printf` can access the socket for output redirection.

### Data Flow

```
Master (PC)
  |
  | TCP port 9000
  |
  v
ESP32 C2 Server Task
  |
  +---> Receive Header (4 bytes)
  |
  +---> Stream to /linux/payload.elf
  |
  +---> Enable Stdout Redirection
  |        |
  |        +---> Set g_c2_socket_fd = client_socket
  |        |
  |        +---> Set s_c2_redirect_stdout = true
  |
  +---> Execute ELF via execve()
  |        |
  |        +---> execve spawns new task (spawn model)
  |        |
  |        +---> App calls printf/puts
  |        |
  |        +---> shim_write()/shim_puts() intercepts
  |        |
  |        +---> Dual-route to UART + TCP
  |
  +---> Wait for payload completion via waitpid()
  |        |
  |        +---> Blocks until payload task finishes
  |        |
  |        +---> Ensures all output is captured
  |
  +---> Cleanup & Close
```

## Implementation Details

### Modified Files

#### `apps/c2_bot/main.c` (Guest ELF Application)
```c
// C2 Bot is now a guest ELF application (not firmware task)
// Uses POSIX APIs exclusively

static void c2_handle_client(int client_sock)
{
    // 1. Receive payload and save to /linux/payload.elf
    c2_receive_payload(client_sock);
    
    // 2. Redirect stdout/stderr to socket using POSIX dup2()
    dup2(client_sock, STDOUT_FILENO);
    dup2(client_sock, STDERR_FILENO);
    
    // 3. Execute payload using execve (spawn model)
    char *argv[] = { "payload.elf", 0 };
    execve("/linux/payload.elf", argv, 0);
    
    // 4. Wait for payload to complete using waitpid
    int status = 0;
    waitpid(-1, &status, 0);  // Wait for any child
    
    // 5. Close socket after payload completes
    close(client_sock);
}

int app_main(int argc, char *argv[])
{
    // Create listening socket on port 9000
    // Accept clients in loop
    // Handle each client with c2_handle_client()
}
```

**Key Changes from Firmware-Based Version:**
- C2 bot is now a guest ELF application, not a FreeRTOS task
- Uses POSIX `execve()` and `waitpid()` instead of direct ELF loader calls
- Uses `dup2()` for stdout redirection (POSIX-compliant)
- Can be updated independently of firmware

#### `main/syscalls/shim_unistd.c` (MODIFIED)
```c
// Shared C2 redirection state (allocated once, pointer is stable)
typedef struct {
    int socket_fd;
    bool redirect_stdout;
    bool redirect_stderr;
} c2_redirect_state_t;

static c2_redirect_state_t s_c2_state = {
    .socket_fd = -1,
    .redirect_stdout = false,
    .redirect_stderr = false,
};

// Public pointer to state (guest apps modify via pointer)
c2_redirect_state_t *g_c2_redirect_state = &s_c2_state;

// shim_dup2() sets both flags AND socket_fd
int shim_dup2(int oldfd, int newfd)
{
    // Configure C2 pipe
    c2_pipe_set_socket(oldfd);
    
    // CRITICAL: Set socket_fd in shared state
    g_c2_redirect_state->socket_fd = oldfd;
    
    // Mark which stream is redirected
    if (newfd == STDOUT_FILENO) {
        g_c2_redirect_state->redirect_stdout = true;
    }
    if (newfd == STDERR_FILENO) {
        g_c2_redirect_state->redirect_stderr = true;
    }
    return newfd;
}

// shim_write() checks state and dual-routes output
ssize_t shim_write(int fd, const void *buf, size_t count)
{
    bool is_c2_redirect = (fd == STDOUT_FILENO && g_c2_redirect_state->redirect_stdout) ||
                          (fd == STDERR_FILENO && g_c2_redirect_state->redirect_stderr);

    if (is_c2_redirect && g_c2_redirect_state->socket_fd >= 0) {
        // Write to UART
        write(fd, buf, count);
        // Also send to C2 socket
        send(g_c2_redirect_state->socket_fd, buf, count, MSG_DONTWAIT);
        return count;
    }
    // ... normal write path
}
```

#### `main/syscalls/shim_process.c` (MODIFIED)
```c
// Process tracking for waitpid support
typedef struct child_process {
    pid_t pid;
    TaskHandle_t task_handle;
    SemaphoreHandle_t completion_sem;
    int exit_status;
    bool completed;
    struct child_process *next;
} child_process_t;

// execve() now tracks spawned processes
int shim_execve(const char *path, char *const argv[], char *const envp[])
{
    // ... create task ...
    // Add to child process tracking list
    add_child_process(child_pid, task_handle);
    // ...
}

// waitpid() waits for child to complete
pid_t shim_waitpid(pid_t pid, int *status, int options)
{
    // Find child process
    // Wait on completion semaphore
    // Return exit status
    // Clean up child entry
}
```

#### `main/CMakeLists.txt` (MODIFIED)
- Added `c2_server.c` to SRCS list

#### `tools/build_and_run.py` (MODIFIED)
- Added `--c2` flag for interactive C2 mode
- Added `--port` flag for custom port forwarding
- Modified `run_simulation()` to accept port forwarding options
- C2 mode uses interactive stdin/stdout (Ctrl+C to exit)
- Standard mode uses timeout capture as before

**New CLI options:**
```bash
python tools/build_and_run.py --c2              # C2 mode with port 9000
python tools/build_and_run.py --c2 --port 8080 # Custom port
python tools/build_and_run.py --sim --c2        # Simulation only in C2 mode
```

#### `tools/c2_master.py` (NEW - 120 lines)
- Python 3 script for sending payloads
- Supports both real hardware (IP address) and QEMU (localhost)
- Proper error handling and timeout management
- Real-time output streaming with UTF-8 error handling

#### `apps/c2_payload/main.c` (NEW - 90 lines)
- Demo application showing payload execution
- Simulates WiFi network scanner
- Outputs formatted table of scan results
- Uses only standard C library functions for portability

#### `components/espressif__elf_loader/src/esp_all_symbol.c` (MODIFIED)
- Added `printf` and `puts` to exported symbol table
- Maps `"printf"` → `&shim_printf`, `"puts"` → `&shim_puts`
- These are required by the c2_payload application

#### `components/espressif__elf_loader/src/esp_elf_symbol.c` (MODIFIED - CRITICAL)

**Problem:** ELF loader had 3 symbol tables (libc, espidf, customer) searched in order. Libc symbols were checked **first**, so `printf` resolved to real libc (at 0x40116a30) instead of shim_printf (at 0x400dadb4).

**Solution:** Reordered symbol search to prioritize **customer symbols first**:
```c
uintptr_t elf_find_sym(const char *sym_name)
{
    // Check customer symbols FIRST (allows overriding libc functions)
    #ifdef CONFIG_ELF_LOADER_CUSTOMER_SYMBOLS
        [search g_customer_elfsyms...]
    #endif

    // Then check libc and espidf symbols as fallback
    #ifdef CONFIG_ELF_LOADER_LIBC_SYMBOLS
        [search g_esp_libc_elfsyms...]
    #endif

    #ifdef CONFIG_ELF_LOADER_ESPIDF_SYMBOLS
        [search g_esp_espidf_elfsyms...]
    #endif
}
```

**Impact:** Now `printf` resolves to `shim_printf` which intercepts and redirects to socket.

## Testing Procedure

### Quick Start (QEMU)

**Terminal 1 - Start QEMU with C2 server:**
```bash
cd C:\Users\Dhruv\Documents\Projects\LinuxCompatibilityLayer
python tools/build_and_run.py --c2
```

Wait for output like:
```
[INFO] ============================================
[INFO]   C2 Server Mode Active
[INFO]   Listening on TCP port 9000
[INFO] ============================================
[INFO] To send a payload:
[INFO]   python tools/c2_master.py <payload.elf> <IP>
```

**Terminal 2 - Build and send payload:**
```bash
cd C:\Users\Dhruv\Documents\Projects\LinuxCompatibilityLayer

# Build the c2_payload guest ELF
tools\build_guest_app.bat c2_payload

# Send it to the bot
python tools/c2_master.py build/guest_apps/c2_payload.elf localhost
```

**Expected Output (Master):**
```
[Master] Loaded payload: 4096 bytes
[Master] Connecting to localhost:9000...
[Master] Connected!
[Master] Payload sent. Waiting for execution output...
============================================================

========================================
  ESP32 C2 Payload - WiFi Scanner
========================================

Command: SCAN_WIFI
Initializing wireless interface...

[*] WiFi interface ready
[*] Starting passive scan...

[+] Scanning complete!

----------------------------------------------------------
SSID                     | RSSI  | CHAN | SECURITY
----------------------------------------------------------
FBI_Surveillance_Van     | -45   | 6    | WPA2-PSK
Linksys_Home             | -80   | 1    | OPEN
...

[+] Scan complete. 5 networks found.

Payload execution complete.
============================================================
[Master] Connection closed
```

### Real Hardware Test (Optional)

1. Find ESP32's IP address from serial monitor
2. Run: `python tools/c2_master.py payload.elf <ESP_IP>`
3. Monitor UART output in serial terminal

## Protocol Specification

### C2 Protocol

**Connection:** TCP port 9000 (configurable)

**Request:**
```
[0:4]     Payload size (4 bytes, little-endian unsigned int)
[4:4+N]   ELF binary data (N bytes as specified in header)
```

**Response:**
```
[Streamed stdout from executed ELF]
[Bot acknowledgment and completion messages]
```

### Example Protocol Sequence

```
1. Client connects to port 9000
2. Client sends:
   - 0x1000 0x0000 0x0000 0x0000  (4096 bytes little-endian)
   - [4096 bytes of ELF data]
3. Bot responds:
   - "[Bot] Payload received. Executing...\n"
4. Bot executes ELF, streams output
5. Bot sends:
   - "\n[Bot] Execution complete. Return code: 0\n"
6. Connection closed by bot
```

## Configuration

### Compile-Time Configuration

**In `main/main.c`:**
```c
#define ENABLE_C2_SERVER true               // Enable/disable C2 mode
#define USE_SDCARD_FILESYSTEM false         // Use LittleFS for QEMU
#define DEFAULT_ELF_PATH "/linux/c2_payload.elf"
```

### Runtime Configuration

**Network:**
- Requires working network stack (initialized by drivers)
- DHCP auto-configuration
- QEMU: Use `--c2` flag to enable port forwarding

**Filesystem:**
- Payload saved to `/linux/payload.elf` (LittleFS mount point)
- Requires at least 1MB free space

**Socket:**
- Max payload size: 1MB (configurable in c2_server.c)
- Receive buffer: 512 bytes
- Socket timeout: 30 seconds (in c2_master.py)

## Limitations & Notes

### QEMU Specific
- Port forwarding required: `hostfwd=tcp::9000-:9000`
- Configured automatically by `build_and_run.py --c2`
- Single client at a time (server is blocking)

### Spawn Model with waitpid
- C2 payload runs in separate FreeRTOS task (spawned by `execve`)
- Parent process (C2 bot) uses `waitpid()` to wait for completion
- Socket connection remains open until payload finishes
- Ensures all output is captured before connection closes

### Network Stack
- Requires LwIP and NVS initialization
- DHCP must complete before C2 server ready
- Localhost connects via OpenEth MAC (QEMU)

## Future Enhancements

1. **Multi-client Support**: Use separate task per connection
2. **Bidirectional I/O**: stdin from master to payload
3. **Progress Callbacks**: Client-side progress reporting
4. **Compressed Payloads**: gzip support in protocol
5. **Payload Verification**: CRC32 checksums
6. **Persistent Logging**: Save execution logs to filesystem
7. **Scheduled Execution**: Cron-like task scheduling
8. **Payload Library**: Store multiple payloads and list/select

## Files Summary

| File | Lines | Purpose |
|------|-------|---------|
| `main/c2_server.c` | 200 | C2 server implementation |
| `main/c2_server.h` | 35 | C2 server API |
| `tools/c2_master.py` | 120 | Python C2 client |
| `apps/c2_payload/main.c` | 90 | Demo payload app |
| `main/main.c` | +30 | C2 server startup |
| `main/syscalls/shim_unistd.c` | +2 | Remove static from flags |
| `main/CMakeLists.txt` | +1 | Add c2_server.c |
| `tools/build_and_run.py` | +50 | C2 mode support |
| `components/.../esp_all_symbol.c` | +2 | printf/puts symbols |

## Verification Checklist

- [x] C2 server compiles without errors
- [x] C2 master Python script created
- [x] C2 payload application defined
- [x] Stdout redirection verified in shim layer
- [x] Build system updated with c2_server.c
- [x] Port forwarding added to build_and_run.py
- [x] Documentation complete
- [x] Full QEMU test - SUCCESS! WiFi scanner output streams correctly to master

## References

- Task 05: Stdout Redirection (dup2/dup3 shims)
- Task 06: Driver Abstraction Layer (network driver)
- Protocol: Custom binary protocol over raw TCP
- Compression: None (plain binary)
- Serialization: Little-endian unsigned integers
