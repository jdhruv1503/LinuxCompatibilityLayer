# C Standard Library Requirements for Guest ELFs

This document analyzes what is required for guest ELF applications to use standard C library functions like `printf()`, `malloc()`, and file I/O.

---

## 1. Problem Statement

Currently (after Task 02), guest ELFs can only:
- Execute pure computation
- Return integer values
- Access passed arguments (argc/argv) if they understand the ABI

Guest ELFs CANNOT:
- Call `printf()` - crashes or no output
- Use `malloc()`/`free()` - undefined behavior
- Perform file I/O (`fopen`, `fread`)
- Use string functions (`strlen`, `strcpy`)

---

## 2. How printf() Works

Understanding the call chain reveals what needs to be implemented:

```
Guest ELF calls:    printf("Hello %d\n", 42)
                           ↓
Newlib's printf:    Formats string into buffer
                           ↓
_write_r():         Newlib's reentrant write syscall stub
                           ↓
write():            POSIX syscall (fd, buffer, length)
                           ↓
??:                 WHO HANDLES THIS? (currently nobody)
```

The ELF loader exports Newlib's `printf`, but `printf` internally calls `_write_r` → `write()`. Without a working `write()` syscall that routes fd=1 (stdout) somewhere useful, the output is lost or crashes.

---

## 3. Minimal Requirements for printf()

### 3.1 Standard File Descriptors

POSIX requires these file descriptors to exist:
- fd 0: stdin (input)
- fd 1: stdout (output)  ← printf writes here
- fd 2: stderr (error output)

**Implementation:** Before executing guest ELF, open pseudo-files or use VFS to create these descriptors.

### 3.2 write() Syscall Implementation

```c
ssize_t shim_write(int fd, const void *buf, size_t count)
{
    if (fd == 1 || fd == 2) {
        // Route stdout/stderr to ESP-IDF console
        // Option 1: Use uart_write_bytes()
        // Option 2: Use esp_rom_printf()
        // Option 3: Use fwrite(buf, 1, count, stdout)
        return esp_console_write(buf, count);
    }
    // For other fds, pass through to VFS
    return write(fd, buf, count);
}
```

### 3.3 Symbol Export

Ensure these symbols are exported to guest ELFs:
- `printf`, `sprintf`, `snprintf`, `fprintf`
- `puts`, `putchar`, `fputs`
- `write` (maps to shim_write)

With `CONFIG_ELF_LOADER_LIBC_SYMBOLS=y`, Newlib functions are exported. But we need to intercept the low-level syscall.

---

## 4. Minimal Requirements for malloc()

### 4.1 Heap Management

Newlib's `malloc` uses `_sbrk_r()` to request heap memory. On ESP32, this is already implemented by ESP-IDF's heap allocator.

With `CONFIG_ELF_LOADER_LIBC_SYMBOLS=y`, these should work:
- `malloc()`, `free()`, `realloc()`, `calloc()`

### 4.2 Verification Needed

Test if malloc works with symbol export alone, or if heap pointers need initialization.

---

## 5. File I/O Requirements

### 5.1 Low-Level Syscalls Needed

For `fopen()`/`fread()`/`fwrite()` to work:

| Function | Underlying Syscall | Shim Needed |
|----------|-------------------|-------------|
| fopen | open() | shim_open (path translation) |
| fread | read() | shim_read (pass-through) |
| fwrite | write() | shim_write (stdout routing) |
| fclose | close() | shim_close (pass-through) |
| fseek | lseek() | shim_lseek (pass-through) |
| stat | stat() | shim_stat (permission faking) |

### 5.2 Path Translation

Guest apps see paths like `/log.txt`, but ESP-IDF VFS needs `/linux/log.txt`.

```c
void translate_path(const char *guest_path, char *host_path, size_t len) {
    if (guest_path[0] == '/') {
        snprintf(host_path, len, "/linux%s", guest_path);
    } else {
        snprintf(host_path, len, "/linux/%s", guest_path);
    }
}
```

---

## 6. String Functions

### 6.1 Usually Work with Symbol Export

These Newlib functions should work with `CONFIG_ELF_LOADER_LIBC_SYMBOLS=y`:
- `strlen`, `strcpy`, `strncpy`, `strcmp`, `strncmp`
- `strcat`, `strncat`, `strchr`, `strrchr`, `strstr`
- `memcpy`, `memmove`, `memset`, `memcmp`

### 6.2 No Special Implementation Needed

These are pure functions with no syscall dependencies.

---

## 7. Implementation Priority for Task 03

### Phase 1: Console Output (Highest Priority)

**Goal:** `printf("Hello World\n")` produces output

1. Implement `shim_write()` that routes fd 1/2 to UART
2. Create fd 0/1/2 before guest execution (or use Newlib defaults)
3. Export write symbol mapping to shim

### Phase 2: File System Access

**Goal:** `fopen("/log.txt", "w")` creates file at `/linux/log.txt`

1. Implement `shim_open()` with path translation
2. Implement pass-through: `shim_read`, `shim_close`, `shim_lseek`
3. Implement `shim_stat` with permission faking (return 0777)

### Phase 3: Directory Operations

**Goal:** `opendir("/")` lists `/linux/` contents

1. Implement `shim_opendir`, `shim_readdir`, `shim_closedir`
2. Implement `shim_mkdir`, `shim_rmdir`

---

## 8. Symbol Resolution Strategy

### Option A: Use ELF Loader Built-in

Enable in sdkconfig:
```
CONFIG_ELF_LOADER_LIBC_SYMBOLS=y
CONFIG_ELF_LOADER_ESPIDF_SYMBOLS=y
```

This exports Newlib functions directly. BUT: low-level syscalls (write, open) might not do what we need.

### Option B: Custom Symbol Table (Recommended)

Create a custom symbol table that maps:
- `printf` → Newlib's printf (use built-in export)
- `write` → `shim_write` (our implementation)
- `open` → `shim_open` (our implementation)
- `read` → `shim_read` (our implementation)

This gives us control over syscall behavior while reusing Newlib's higher-level functions.

---

## 9. Newlib Reentrancy

Newlib uses `_REENT` structure for thread-safe operation. On ESP-IDF:
- Each FreeRTOS task has its own `_reent` structure
- The guest ELF runs in the same task as the loader
- Should inherit the task's reentrancy context automatically

**Potential Issue:** If guest and host share the same `_reent`, stdio state (buffers, FILE pointers) might conflict.

**Solution:** Consider running guest in a separate FreeRTOS task (optional, for isolation).

---

## 10. Testing Checklist

After implementing Task 03 syscalls:

- [ ] `printf("Hello World\n")` produces serial output
- [ ] `printf("Value: %d\n", 42)` formats correctly
- [ ] `puts("Simple string")` works
- [ ] `malloc(100)` returns valid pointer
- [ ] `free(ptr)` doesn't crash
- [ ] `fopen("/test.txt", "w")` creates `/linux/test.txt`
- [ ] `fwrite()` writes data to file
- [ ] `fread()` reads data back
- [ ] `stat("/test.txt", &st)` returns file info with 0777 permissions
- [ ] `strlen("test")` returns 4
- [ ] `strcpy(dst, src)` copies string

---

## 11. Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                     Guest ELF Application                    │
│  printf("Hello")  fopen("/log.txt")  malloc(100)            │
└──────────────┬──────────────┬──────────────┬────────────────┘
               │              │              │
               ▼              ▼              ▼
┌──────────────────────────────────────────────────────────────┐
│                  Newlib C Library (exported)                  │
│  printf() ───────► _write_r() ───────► write()              │
│  fopen()  ───────► _open_r()  ───────► open()               │
│  malloc() ───────► _malloc_r() (uses ESP-IDF heap)          │
└──────────────┬──────────────┬───────────────────────────────┘
               │              │
               ▼              ▼
┌──────────────────────────────────────────────────────────────┐
│                    Syscall Shim Layer                         │
│  shim_write()  ────► route stdout to UART                    │
│  shim_open()   ────► translate path + VFS open               │
│  shim_stat()   ────► VFS stat + fake 0777 perms              │
└──────────────┬──────────────┬───────────────────────────────┘
               │              │
               ▼              ▼
┌──────────────────────────────────────────────────────────────┐
│                    ESP-IDF VFS / Hardware                     │
│  LittleFS (/linux)    UART (console)    GPIO, SPI, etc.     │
└──────────────────────────────────────────────────────────────┘
```
