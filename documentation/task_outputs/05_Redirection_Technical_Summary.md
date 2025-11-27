# Task 05: Stdout Redirection - Technical Summary

## Overview

This task implemented a stdout/stderr redirection mechanism for ESP32 guest ELF applications. The primary use case is C2 (Command and Control) scenarios where a guest application needs to redirect its standard output to a network socket.

## Architecture

### The Problem

Standard POSIX `dup2()` implementation requires:
1. Close the target FD (e.g., stdout = FD 1)
2. Duplicate the source FD (socket) to the target FD number

On ESP32, this approach **FAILS** because:
- `STDOUT_FILENO` (FD 1) and `STDERR_FILENO` (FD 2) share the same UART driver
- Closing FD 1 breaks the UART console entirely
- ESP32's VFS doesn't support true FD table manipulation

### The Solution: Global Socket Variable Approach

Instead of manipulating the FD table, we:
1. Maintain global state tracking which streams are "redirected"
2. Store the target socket FD in a global variable
3. Intercept writes in `shim_write()` and dual-route them

```
Guest calls: dup2(socket_fd, STDOUT_FILENO)
           │
           ▼
┌─────────────────────────────────┐
│       shim_dup2()               │
│ - Store socket_fd in global     │
│ - Set s_c2_redirect_stdout=true │
│ - Return STDOUT_FILENO (success)│
└─────────────────────────────────┘
           │
           ▼
Guest calls: printf("Hello")
           │
           ▼
┌─────────────────────────────────┐
│       shim_write(fd=1, ...)     │
│ - Check: is fd stdout AND       │
│          s_c2_redirect_stdout?  │
│ - If yes:                       │
│   1. write() to UART            │
│   2. send() to socket           │
│ - Return success                │
└─────────────────────────────────┘
```

## Implementation Details

### Files Modified/Created

| File | Purpose |
|------|---------|
| `main/vfs_drivers/vfs_c2_pipe.c` | VFS driver for `/dev/c2` (optional, not used in final design) |
| `main/vfs_drivers/vfs_c2_pipe.h` | Header with C2 pipe API |
| `main/syscalls/shim_unistd.c` | `dup/dup2/dup3` shims and modified `shim_write()` |
| `tools/build_and_run.py` | Added `--capture PORT` for socket capture testing |
| `apps/c2_redirect/main.c` | Test application demonstrating the mechanism |

### Key Data Structures

```c
// In shim_unistd.c
static bool s_c2_redirect_stdout = false;  // Is stdout redirected?
static bool s_c2_redirect_stderr = false;  // Is stderr redirected?

// In vfs_c2_pipe.c
typedef struct {
    int target_socket_fd;   // Socket to forward output to
    bool active;            // Whether pipe is open
    bool mirror_to_uart;    // Also send to UART (default: true)
} c2_pipe_ctx_t;
```

### shim_write() Implementation

```c
ssize_t shim_write(int fd, const void *buf, size_t count) {
    bool is_c2_redirect = (fd == STDOUT_FILENO && s_c2_redirect_stdout) ||
                          (fd == STDERR_FILENO && s_c2_redirect_stderr);

    if (is_c2_redirect) {
        int c2_socket = c2_pipe_get_socket();
        if (c2_socket >= 0) {
            // Always write to UART (for local debugging)
            write(fd, buf, count);

            // Also send to socket (non-blocking)
            int sent = send(c2_socket, buf, count, MSG_DONTWAIT);
            // Error handling: log at DEBUG level, don't fail
        }
        return count;  // Success regardless of socket status
    }

    // Normal write path
    return write(fd, buf, count);
}
```

### shim_dup2() Implementation

```c
int shim_dup2(int oldfd, int newfd) {
    // Only support stdout/stderr redirection
    if (newfd != STDOUT_FILENO && newfd != STDERR_FILENO) {
        errno = ENOTSUP;
        return -1;
    }

    // Configure C2 pipe with the socket
    c2_pipe_set_socket(oldfd);

    // Mark stream as redirected
    if (newfd == STDOUT_FILENO) s_c2_redirect_stdout = true;
    if (newfd == STDERR_FILENO) s_c2_redirect_stderr = true;

    // Return newfd (POSIX-compliant)
    return newfd;
}
```

## Symbols Exported

Added to ELF loader symbol table:
- `dup` -> `shim_dup`
- `dup2` -> `shim_dup2`
- `dup3` -> `shim_dup3`
- `vfs_c2_pipe_register`
- `c2_pipe_set_socket`
- `c2_pipe_get_socket`
- `c2_pipe_set_mirror`
- `c2_pipe_is_active`

## Testing

### Test Application: c2_redirect

The test app demonstrates:
1. Creating a TCP socket
2. Attempting to connect (fails in QEMU due to networking limitations)
3. Using `dup2()` to redirect stdout
4. Printing messages that go to both UART and socket

### Socket Capture Feature

**Note:** Network capture options were investigated but are not supported by ESP32 QEMU:
- Standard QEMU's `-net dump,file=capture.pcap` is not compiled into Espressif's QEMU build
- Espressif's QEMU only supports `-net [user|tap|bridge|socket]`
- guestfwd for guest-to-host connections doesn't work reliably

For network traffic verification, use real hardware or tap networking with Wireshark.

### Verification

The mechanism is verified working by observing:
1. `dup2()` returns `STDOUT_FILENO` (success)
2. `printf()` output appears on UART
3. `send()` is called (fails with ENOTCONN if socket not connected)

## Limitations

### QEMU User-Mode Networking

Guest-to-host TCP connections in QEMU user-mode networking are problematic:
- `hostfwd` is for host->guest, not guest->host
- `guestfwd` has limited support and doesn't work reliably
- The 10.0.2.2 gateway doesn't automatically proxy to host localhost

To verify end-to-end socket transmission:
1. Use real hardware with actual network
2. Connect to an external server the guest can reach
3. Use QEMU's tap networking instead of user-mode

### No True FD Duplication

ESP32's VFS doesn't support true POSIX file descriptor duplication. The implementation is a "best effort" emulation that:
- Works for the C2 use case (redirect to socket)
- Doesn't support arbitrary FD duplication
- Doesn't affect actual FD table

## Error Handling

### Design Decisions

1. **Socket errors don't fail writes**: If `send()` fails, `shim_write()` still returns success. This prevents the application from crashing if the C2 connection drops.

2. **Non-blocking sends**: `MSG_DONTWAIT` flag prevents blocking on socket operations.

3. **UART mirroring enabled by default**: Allows debugging even when redirected.

4. **DEBUG-level logging**: Runtime logs are at DEBUG level to avoid noise.

## Future Improvements

1. **Bidirectional C2**: The `/dev/c2` VFS driver supports `read()` for receiving commands.

2. **Multiple sockets**: Current design supports one socket; could extend to multiple.

3. **Restore functionality**: Add `shim_dup2(original_stdout, STDOUT_FILENO)` to restore.

4. **Connection management**: Auto-reconnect on socket failure.

## Lessons Learned

### ESP32 UART Behavior
- FD 1 and FD 2 share the UART driver
- Closing either breaks console output
- Must preserve original FDs while intercepting writes

### QEMU Networking
- User-mode networking is limited for guest-to-host connections
- guestfwd syntax and behavior varies by QEMU version
- ESP32 QEMU (Espressif build) doesn't support `-net dump` for pcap capture
- Standard QEMU's pcap feature requires `-net dump,file=capture.pcap` but it's not in ESP32 QEMU
- For reliable testing, use real hardware or tap networking with Wireshark

### Symbol Export
- New symbols must be added to `export_symbols.py`
- Must rebuild twice (export -> rebuild) for symbols to link
- Use `__attribute__((used))` to prevent stripping

## Files Reference

```
main/
├── syscalls/
│   └── shim_unistd.c      # dup/dup2/dup3 + modified shim_write
├── vfs_drivers/
│   ├── vfs_c2_pipe.c      # /dev/c2 VFS driver
│   └── vfs_c2_pipe.h      # Header
└── main.c                 # Registers vfs_c2_pipe_register()

apps/
└── c2_redirect/
    └── main.c             # Test application

tools/
├── build_and_run.py       # Added --capture PORT
└── export_symbols.py      # Updated with new symbols
```

## Commit Summary

```
feat(redirect): Implement stdout/stderr redirection to socket

- Add dup/dup2/dup3 shims using global socket variable approach
- Create VFS pipe driver for /dev/c2
- Modify shim_write to dual-route to UART and socket
- Add socket capture to build_and_run.py (--capture PORT)
- Create c2_redirect test application

Note: ESP32 UART shares FD 1/2, cannot close stdout without
breaking console. Solution: intercept writes, don't modify FD table.
```
