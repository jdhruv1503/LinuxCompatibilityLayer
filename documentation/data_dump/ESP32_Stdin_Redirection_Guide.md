# ESP32 Stdin/Stdout/Stderr Redirection Guide

## Overview

ESP32 newlib hardcodes stdin/stdout/stderr to UART. This guide documents how to redirect standard I/O streams to network sockets for distributed computing and C2 scenarios.

## The Challenge

### Why Traditional dup2() Doesn't Work

On standard Linux:
```c
int sock = socket(...);
connect(sock, ...);
dup2(sock, STDOUT_FILENO);  // stdout now goes to socket
printf("Hello");            // Goes to socket
```

On ESP32:
- `dup2()` exists but doesn't truly duplicate file descriptors
- stdout is hardcoded in newlib to UART
- Closing FD 1 breaks console output entirely
- FD table modifications don't affect libc's internal FILE* structures

## The Solution: Global State Approach

Instead of modifying the FD table, use a global state structure that shim functions check.

### State Structure

```c
typedef struct {
    int socket_fd;           // Target socket for redirection
    bool redirect_stdout;    // stdout -> socket
    bool redirect_stderr;    // stderr -> socket
    bool redirect_stdin;     // socket -> stdin
} c2_redirect_state_t;

// Static instance with stable memory address
static c2_redirect_state_t s_c2_state = {
    .socket_fd = -1,
    .redirect_stdout = false,
    .redirect_stderr = false,
    .redirect_stdin = false,
};

// Public pointer exported to guest ELFs
c2_redirect_state_t *g_c2_redirect_state = &s_c2_state;
```

### Shim Implementation

#### shim_dup2() - Set Redirection
```c
int shim_dup2(int oldfd, int newfd) {
    // Store the socket FD
    g_c2_redirect_state->socket_fd = oldfd;

    // Mark which stream is being redirected
    if (newfd == STDIN_FILENO) {
        g_c2_redirect_state->redirect_stdin = true;
    }
    if (newfd == STDOUT_FILENO) {
        g_c2_redirect_state->redirect_stdout = true;
    }
    if (newfd == STDERR_FILENO) {
        g_c2_redirect_state->redirect_stderr = true;
    }

    return newfd;  // POSIX-compliant return
}
```

#### shim_read() - Intercept Stdin
```c
ssize_t shim_read(int fd, void *buf, size_t count) {
    // Check if stdin is redirected
    if (fd == STDIN_FILENO &&
        g_c2_redirect_state->redirect_stdin &&
        g_c2_redirect_state->socket_fd >= 0) {
        // Read from socket instead of UART
        return recv(g_c2_redirect_state->socket_fd, buf, count, 0);
    }

    // Normal read path
    return read(fd, buf, count);
}
```

#### shim_write() - Intercept Stdout/Stderr
```c
ssize_t shim_write(int fd, const void *buf, size_t count) {
    bool is_redirect = (fd == STDOUT_FILENO && g_c2_redirect_state->redirect_stdout) ||
                       (fd == STDERR_FILENO && g_c2_redirect_state->redirect_stderr);

    if (is_redirect && g_c2_redirect_state->socket_fd >= 0) {
        // Write to UART for local debugging
        write(fd, buf, count);

        // Also send to socket (non-blocking)
        send(g_c2_redirect_state->socket_fd, buf, count, MSG_DONTWAIT);

        return count;
    }

    // Normal write
    return write(fd, buf, count);
}
```

#### shim_printf() / shim_puts() - Intercept Formatted Output
```c
int shim_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    // Format into buffer
    char buf[1024];
    int ret = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    if (g_c2_redirect_state->redirect_stdout && g_c2_redirect_state->socket_fd >= 0) {
        // Send to socket
        send(g_c2_redirect_state->socket_fd, buf, ret, MSG_DONTWAIT);
        // Also to UART for debugging
        fwrite(buf, 1, ret, stderr);
        return ret;
    }

    // Normal output
    fwrite(buf, 1, ret, stdout);
    return ret;
}
```

## Critical Implementation Notes

### 1. Declaration Order Matters

The state structure MUST be declared before any functions that use it:

```c
// WRONG - will cause "undeclared" error
ssize_t shim_read(int fd, void *buf, size_t count) {
    if (g_c2_redirect_state->redirect_stdin) ...  // ERROR!
}

typedef struct { ... } c2_redirect_state_t;
c2_redirect_state_t *g_c2_redirect_state;

// CORRECT
typedef struct { ... } c2_redirect_state_t;
static c2_redirect_state_t s_c2_state = { ... };
c2_redirect_state_t *g_c2_redirect_state = &s_c2_state;

ssize_t shim_read(int fd, void *buf, size_t count) {
    if (g_c2_redirect_state->redirect_stdin) ...  // OK
}
```

### 2. Use MSG_DONTWAIT for Socket Sends

Always use non-blocking sends to prevent app crashes if socket disconnects:

```c
send(socket_fd, buf, count, MSG_DONTWAIT);
```

### 3. Dual-Route Output for Debugging

Always write to both UART and socket for debugging visibility:

```c
// Local UART output (visible in QEMU console)
write(fd, buf, count);
// Network output (to C2 master)
send(socket_fd, buf, count, MSG_DONTWAIT);
```

### 4. Use Raw read() Instead of fgets() for Stdin

`fgets()` uses internal FILE* buffering that may not work with redirected stdin:

```c
// AVOID - may not work with socket stdin
char line[64];
fgets(line, sizeof(line), stdin);

// PREFER - direct syscall that goes through shim_read
char c;
while (read(STDIN_FILENO, &c, 1) > 0) {
    // process character
}
```

## Usage Example

### Guest ELF Code (c2_bot)
```c
int client_sock = accept(listen_sock, ...);

// Redirect stdin/stdout to socket
dup2(client_sock, STDIN_FILENO);
dup2(client_sock, STDOUT_FILENO);
dup2(client_sock, STDERR_FILENO);

// Spawn worker
execve("/linux/worker.elf", argv, NULL);
```

### Worker ELF Code (map_reduce_worker)
```c
// stdin now comes from socket (via dup2 in parent)
char buffer[64];
while (read_line(STDIN_FILENO, buffer, sizeof(buffer)) > 0) {
    // Process input from network
}

// stdout goes to socket
printf("RESULT: SUM=%lld COUNT=%ld\n", sum, count);
```

## Symbol Export Requirements

These symbols must be exported in `esp_all_symbol.c`:

```c
{ "read", &shim_read },
{ "write", &shim_write },
{ "printf", &shim_printf },
{ "puts", &shim_puts },
{ "dup2", &shim_dup2 },
{ "fgets", &shim_fgets },  // Optional, if using fgets
```

## Resetting Redirection

To disable redirection and restore normal I/O:

```c
void reset_c2_redirection(void) {
    g_c2_redirect_state->socket_fd = -1;
    g_c2_redirect_state->redirect_stdin = false;
    g_c2_redirect_state->redirect_stdout = false;
    g_c2_redirect_state->redirect_stderr = false;
}
```

Call this when the socket connection closes or the session ends.
