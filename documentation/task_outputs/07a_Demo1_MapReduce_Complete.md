# Task 07a: Distributed Map-Reduce System - Complete Technical Documentation

## Executive Summary

This task extended the C2 Demo from Task 07 into a **Distributed Map-Reduce Computing System** demonstrating:
- Multi-node cluster management (4 ESP32 QEMU instances)
- Bidirectional data streaming (stdin + stdout over network)
- Parallel computation with result aggregation
- Interactive terminal UI with real-time monitoring
- Live output display during job execution

**Final Result:** Successfully distributed `sum(1..1000)=500500` across 4 ESP32 QEMU nodes with correct aggregation.

---

## Architecture

### System Topology

```
+----------------+     +------------------+     +------------------+
|                |     |   QEMU Node 1    |     |   QEMU Node 2    |
|                |     |   (Port 9001)    |     |   (Port 9002)    |
|   C2 Master    |<--->|   c2_bot.elf     |     |   c2_bot.elf     |
|   (Python)     |     | map_reduce_wkr   |     | map_reduce_wkr   |
|                |     +------------------+     +------------------+
+----------------+
        |              +------------------+     +------------------+
        |              |   QEMU Node 3    |     |   QEMU Node 4    |
        +------------->|   (Port 9003)    |     |   (Port 9004)    |
                       |   c2_bot.elf     |     |   c2_bot.elf     |
                       | map_reduce_wkr   |     | map_reduce_wkr   |
                       +------------------+     +------------------+
```

### Data Flow

```
1. Master generates dataset: [1, 2, 3, ..., 1000] (shuffled)
2. Master splits into 4 chunks of ~250 items each
3. For each node (parallel):
   a. Connect to C2 bot on port 900X
   b. Send map_reduce_worker.elf (ELF size header + binary)
   c. C2 bot spawns worker via execve()
   d. C2 bot redirects stdin/stdout to socket via dup2()
   e. Master sends numbers as newline-separated text
   f. Master closes write end (signals EOF)
   g. Worker computes sum and count
   h. Worker outputs: "RESULT: SUM=X COUNT=Y"
   i. Master parses result
4. Master aggregates all partial sums
5. Master verifies: total_sum == 500500, total_count == 1000
```

---

## Implementation Details

### 1. Stdin Redirection Support (Critical Addition)

**Problem:** The existing shim layer only supported stdout/stderr redirection. Map-reduce workers need to receive data via stdin from the socket.

**Solution:** Extended `c2_redirect_state_t` and `shim_read()` in `main/syscalls/shim_unistd.c`:

```c
// Extended state structure
typedef struct {
    int socket_fd;
    bool redirect_stdout;
    bool redirect_stderr;
    bool redirect_stdin;      // NEW: stdin redirection flag
} c2_redirect_state_t;

// Modified shim_read() to intercept stdin
ssize_t shim_read(int fd, void *buf, size_t count) {
    // Check if stdin is redirected to socket
    if (fd == STDIN_FILENO && g_c2_redirect_state->redirect_stdin &&
        g_c2_redirect_state->socket_fd >= 0) {
        // Read from the C2 socket instead of UART
        return recv(g_c2_redirect_state->socket_fd, buf, count, 0);
    }
    // Normal read path
    return read(fd, buf, count);
}

// Modified shim_dup2() to support STDIN_FILENO
int shim_dup2(int oldfd, int newfd) {
    // ... existing code ...
    if (newfd == STDIN_FILENO) {
        g_c2_redirect_state->redirect_stdin = true;
        ESP_LOGD(TAG, "stdin now redirected to C2 socket fd=%d", oldfd);
    }
    // ... rest of code ...
}
```

**Critical Order Issue Fixed:** The `c2_redirect_state_t` declaration was moved BEFORE `shim_read()` function to avoid "g_c2_redirect_state undeclared" compilation error.

### 2. Map-Reduce Worker (`apps/map_reduce_worker/main.c`)

A minimal guest ELF that:
- Uses raw `read()` on fd=0 (not `fgets()`) to avoid FILE* complications
- Implements custom `simple_atol()` for number parsing
- Implements custom `read_line()` for line-by-line socket reading

```c
int app_main(int argc, char *argv[]) {
    puts("WORKER_STARTED");

    long long sum = 0;
    long count = 0;
    char buffer[64];

    // Read lines from stdin (redirected to socket)
    int len;
    while ((len = read_line(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
        long val = simple_atol(buffer);
        sum += val;
        count++;
    }

    printf("RESULT: SUM=%lld COUNT=%ld\n", sum, count);
    return 0;
}
```

**Why raw read() instead of fgets():**
- `fgets()` uses internal FILE* buffering that may not work correctly with redirected stdin
- `read()` directly calls `shim_read()` which we control
- More predictable behavior with socket-based input

### 3. C2 Master Complete Rewrite (`tools/c2_master.py`)

**New Features:**

#### Multi-Instance QEMU Management
```python
NODES = [
    {"id": 1, "port": 9001, "name": "Node-1"},
    {"id": 2, "port": 9002, "name": "Node-2"},
    {"id": 3, "port": 9003, "name": "Node-3"},
    {"id": 4, "port": 9004, "name": "Node-4"},
]

# QEMU command with port forwarding:
cmd = [
    QEMU_PATH,
    "-nographic",
    "-machine", "esp32",
    "-drive", f"file={MERGED_FLASH},if=mtd,format=raw",
    "-no-reboot",
    "-nic", f"user,model=open_eth,hostfwd=tcp::{port}-:9000"
]
```

#### Interactive Terminal UI with ASCII (Windows-Compatible)
```python
# Cross-platform symbols (ASCII for Windows cmd.exe)
CHECK = "[OK]"
CROSS = "[X]"
ARROW = "->"

def draw_ui():
    # ASCII box drawing
    lines.append(f"+{'-' * (width - 2)}+")
    lines.append(f"| {title}{' ' * padding} |")
    # ... etc
```

#### Live UI Updates During Job Execution
```python
def run_mapreduce_demo(live_ui=True):
    # ... job setup ...

    # Live UI update loop - redraw while jobs are running
    if live_ui:
        while not all(job_done):
            draw_ui()
            time.sleep(0.3)  # Update 3 times per second
        # Final draw after completion
        draw_ui()
```

#### Detailed Job Status Logging
```python
def send_worker_job(node_id, port, data_chunk):
    log_node(node_id, "[CONN] Connecting...")
    log_node(node_id, f"[SEND] ELF ({len(elf_data)} bytes)...")
    log_node(node_id, "[SEND] ELF sent, waiting...")
    log_node(node_id, f"[DATA] Sending {len(data_chunk)} numbers...")
    log_node(node_id, "[DATA] Data sent!")
    log_node(node_id, "[WAIT] Computing...")
    log_node(node_id, f"[DONE] sum={result[0]}")
```

### 4. Debug Logging Optimization

**Problem:** Excessive debug output was cluttering the QEMU output and making demos hard to follow.

**Solution:** Changed log levels in `main/main.c` from DEBUG to WARN:

```c
// Before (noisy)
esp_log_level_set("shim_socket", ESP_LOG_DEBUG);

// After (clean)
esp_log_level_set("lwip", ESP_LOG_WARN);
esp_log_level_set("esp_netif", ESP_LOG_WARN);
esp_log_level_set("shim_socket", ESP_LOG_WARN);
esp_log_level_set("shim_unistd", ESP_LOG_WARN);
esp_log_level_set("drv_network", ESP_LOG_WARN);
esp_log_level_set("drv_fs_littlefs", ESP_LOG_WARN);
esp_log_level_set("drv_devices", ESP_LOG_WARN);
esp_log_level_set("kernel_main", ESP_LOG_INFO);  // Keep info for kernel
esp_log_level_set("c2_bot", ESP_LOG_INFO);       // Keep info for c2_bot
```

---

## Technical Challenges and Solutions

### Challenge 1: Windows Unicode Encoding Error

**Error:**
```
UnicodeEncodeError: 'charmap' codec can't encode character '\u2713' in position 0: character maps to <undefined>
```

**Cause:** Windows cmd.exe uses cp1252 encoding which cannot display Unicode symbols (✓, ✗, →).

**Solution:** Replace all Unicode symbols with ASCII alternatives:
```python
# Before
CHECK = "✓"
CROSS = "✗"
ARROW = "→"

# After (Windows-compatible)
CHECK = "[OK]"
CROSS = "[X]"
ARROW = "->"
```

### Challenge 2: g_c2_redirect_state Undeclared

**Error:**
```
error: 'g_c2_redirect_state' undeclared (first use in this function)
```

**Cause:** The struct declaration was placed AFTER the `shim_read()` function that uses it.

**Solution:** Move the `c2_redirect_state_t` typedef and `g_c2_redirect_state` declaration BEFORE the `shim_read()` function:
```c
// Line 69-86: Struct declaration (MUST be before shim_read at line 107)
typedef struct {
    int socket_fd;
    bool redirect_stdout;
    bool redirect_stderr;
    bool redirect_stdin;
} c2_redirect_state_t;

static c2_redirect_state_t s_c2_state = { ... };
c2_redirect_state_t *g_c2_redirect_state = &s_c2_state;

// Line 107+: Functions that use g_c2_redirect_state
ssize_t shim_read(int fd, void *buf, size_t count) {
    if (fd == STDIN_FILENO && g_c2_redirect_state->redirect_stdin ...
```

### Challenge 3: Connection Timeout on Nodes 3 and 4

**Error:**
```
[X] Node 3 failed: [WinError 10054] An existing connection was forcibly closed
[X] Node 4 failed: [WinError 10054] An existing connection was forcibly closed
```

**Cause:** 20-second default timeout was insufficient for 4 QEMU instances to fully initialize.

**Solution:** Increased default timeout to 40 seconds:
```bash
python tools/c2_master.py --auto --timeout 40
```

### Challenge 4: fgets() vs read() for Socket Stdin

**Problem:** Initial implementation used `fgets()` which didn't work reliably with socket-redirected stdin.

**Cause:** `fgets()` uses internal FILE* buffering that may not interact correctly with the socket-based stdin redirection.

**Solution:** Switched to raw `read()` syscall:
```c
// Custom read_line using raw read()
static int read_line(int fd, char *buf, int max_len) {
    int i = 0;
    while (i < max_len - 1) {
        char c;
        ssize_t ret = read(fd, &c, 1);  // Direct syscall
        if (ret <= 0) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}
```

---

## Files Modified/Created

| File | Action | Description |
|------|--------|-------------|
| `main/syscalls/shim_unistd.c` | Modified | Added stdin redirection support, moved struct declaration |
| `main/main.c` | Modified | Changed log levels from DEBUG to WARN |
| `apps/map_reduce_worker/main.c` | Created | Map-reduce worker guest ELF |
| `tools/c2_master.py` | Rewritten | Complete rewrite with multi-node support |
| `tools/export_symbols.py` | Modified | Added fgets symbol export |
| `data/map_reduce_worker.elf` | Generated | Compiled worker binary |

---

## Usage

### Build Prerequisites
```bash
# Build the map-reduce worker
python tools/build_and_run.py --build-guest map_reduce_worker

# Full system build (includes worker in flash image)
python tools/build_and_run.py --build
```

### Interactive Mode
```bash
python tools/c2_master.py

# Commands:
# [r] Run Map-Reduce job
# [c] Clear logs
# [q] Quit
```

### Automated Test Mode
```bash
python tools/c2_master.py --auto --timeout 40
```

---

## Demo Output

### Terminal UI Layout
```
+==================================================+
|          C2 Distributed Map-Reduce Demo          |
+==================================================+
+----------------------+ +----------------------+
| [Node 1] Port 9001   | | [Node 2] Port 9002   |
+----------------------+ +----------------------+
| [START] Job: 250 items | [START] Job: 250 items |
| [CONN] Connecting...  | | [CONN] Connecting...  |
| [CONN] Connected!     | | [CONN] Connected!     |
| [SEND] ELF (1068 b)   | | [SEND] ELF (1068 b)   |
| [DATA] Sending 250... | | [DATA] Sending 250... |
| [WAIT] Computing...   | | [WAIT] Computing...   |
| [DONE] sum=125655     | | [DONE] sum=127113     |
+----------------------+ +----------------------+
+----------------------+ +----------------------+
| [Node 3] Port 9003   | | [Node 4] Port 9004   |
+----------------------+ +----------------------+
| [DONE] sum=115610     | | [DONE] sum=132122     |
+----------------------+ +----------------------+

=== Master Control Log ===
[12:50:23] Split into 4 chunks of ~250 items
[12:50:26] [OK] Node 3: sum=115610, count=250
[12:50:26] [OK] Node 4: sum=132122, count=250
[12:50:26] [OK] Node 2: sum=127113, count=250
[12:50:26] [OK] Node 1: sum=125655, count=250
[12:50:26] Job Complete!
[12:50:26]   Total Sum:    500500
[12:50:26]   Total Count:  1000
[12:50:26]   Expected Sum: 500500
[12:50:26] [OK] SUCCESS: Results match perfectly!

Commands: [r] Run Map-Reduce  [c] Clear Logs  [q] Quit
```

### Final Test Result
```
==================================================
MAP-REDUCE TEST RESULT
==================================================
[OK] TEST PASSED
```

---

## Protocol Specification

### C2 Map-Reduce Protocol

**Phase 1: ELF Upload**
```
Client → Server: [4 bytes] ELF size (little-endian uint32)
Client → Server: [N bytes] ELF binary data
```

**Phase 2: Data Streaming**
```
Client → Server: "123\n456\n789\n..." (numbers as newline-separated text)
Client → Server: [shutdown SHUT_WR]  (signals EOF)
```

**Phase 3: Result Collection**
```
Server → Client: "WORKER_STARTED\n"
Server → Client: "RESULT: SUM=X COUNT=Y\n"
Server → Client: [connection close]
```

**Result Parsing Regex:**
```python
match = re.search(r"RESULT:\s*SUM=(\d+)\s+COUNT=(\d+)", text)
```

---

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Nodes | 4 QEMU instances |
| Startup timeout | 40 seconds |
| Items per node | ~250 |
| Total items | 1000 |
| Expected sum | 500500 |
| Job completion time | ~3-5 seconds |
| UI refresh rate | 3 Hz (300ms) |

---

## Future Improvements

1. **Fault Tolerance**: Retry failed nodes, redistribute work
2. **Dynamic Scaling**: Add/remove nodes at runtime
3. **Progress Tracking**: Real-time progress bars for long jobs
4. **Different Algorithms**: Word count, matrix multiplication, etc.
5. **Real Hardware**: Test on physical ESP32 cluster via WiFi
6. **Load Balancing**: Distribute work based on node performance
7. **Checkpoint/Recovery**: Save intermediate results for fault recovery

---

## References

- Task 05: Stdout Redirection (dup2/dup3 shims)
- Task 06: Driver Abstraction Layer (network driver)
- Task 07: Demo1 - C2 System (base implementation)
- ESP-IDF QEMU Documentation
- Python asyncio/threading for concurrent I/O
