# Task 07a: Demo1 Improvements - Distributed Map-Reduce System

## Overview

This task enhances the C2 Demo from Task 07 into a **Distributed Map-Reduce Computing System** that demonstrates:
1. Multi-node cluster management (4 ESP32 QEMU instances)
2. Bidirectional data streaming (stdin + stdout over network)
3. Parallel computation with result aggregation
4. Interactive terminal UI with real-time monitoring

## Architecture

### System Components

```
+----------------+     +------------------+     +------------------+
|                |     |   QEMU Node 1    |     |   QEMU Node 2    |
|                |     |   (Port 9001)    |     |   (Port 9002)    |
|   C2 Master    |<--->|   c2_bot.elf     |<--->|   c2_bot.elf     |
|   (Python)     |     |   map_reduce_    |     |   map_reduce_    |
|                |     |   worker.elf     |     |   worker.elf     |
+----------------+     +------------------+     +------------------+
        |
        |              +------------------+     +------------------+
        |              |   QEMU Node 3    |     |   QEMU Node 4    |
        +------------->|   (Port 9003)    |<--->|   (Port 9004)    |
                       |   c2_bot.elf     |     |   c2_bot.elf     |
                       |   map_reduce_    |     |   map_reduce_    |
                       |   worker.elf     |     |   worker.elf     |
                       +------------------+     +------------------+
```

### Key Changes from Task 07

1. **Stdin Redirection Support** - Added to shim layer
2. **Multi-Instance QEMU Management** - 4 concurrent nodes
3. **Map-Reduce Worker** - New guest application
4. **Interactive Terminal UI** - Split-screen with node logs
5. **Non-Interactive Mode** - `--auto` flag for CI testing

## Implementation Details

### 1. Stdin Redirection (`main/syscalls/shim_unistd.c`)

Added stdin redirection to complement existing stdout/stderr support:

```c
typedef struct {
    int socket_fd;
    bool redirect_stdout;
    bool redirect_stderr;
    bool redirect_stdin;  // NEW
} c2_redirect_state_t;

// In shim_read():
if (fd == STDIN_FILENO && g_c2_redirect_state->redirect_stdin &&
    g_c2_redirect_state->socket_fd >= 0) {
    // Read from socket instead of UART
    return recv(g_c2_redirect_state->socket_fd, buf, count, 0);
}

// In shim_dup2():
if (newfd == STDIN_FILENO) {
    g_c2_redirect_state->redirect_stdin = true;
}
```

### 2. Map-Reduce Worker (`apps/map_reduce_worker/main.c`)

A minimal guest ELF that:
- Reads numbers from stdin (redirected to socket)
- Computes local sum and count
- Outputs result to stdout (sent back to master)

```c
// Protocol:
// Input:  Numbers separated by newlines (via stdin/socket)
// Output: "RESULT: SUM=<sum> COUNT=<count>"

int app_main(int argc, char *argv[]) {
    puts("WORKER_STARTED");

    long long sum = 0;
    long count = 0;
    char buffer[64];

    // Read from stdin (fd=0, redirected to socket by c2_bot)
    while ((len = read_line(STDIN_FILENO, buffer, sizeof(buffer))) > 0) {
        long val = simple_atol(buffer);
        sum += val;
        count++;
    }

    printf("RESULT: SUM=%lld COUNT=%ld\n", sum, count);
    return 0;
}
```

### 3. C2 Master Enhancements (`tools/c2_master.py`)

Complete rewrite with new features:

**Multi-Instance QEMU Management:**
```python
NODES = [
    {"id": 1, "port": 9001, "name": "Node-1"},
    {"id": 2, "port": 9002, "name": "Node-2"},
    {"id": 3, "port": 9003, "name": "Node-3"},
    {"id": 4, "port": 9004, "name": "Node-4"},
]

# Each node runs with port forwarding:
# -nic user,model=open_eth,hostfwd=tcp::{port}-:9000
```

**Job Distribution Protocol:**
1. Connect to node's C2 bot
2. Send ELF size (4 bytes) + ELF binary
3. Wait for worker to start
4. Stream data (numbers as newline-separated text)
5. Close write end to signal EOF
6. Read result ("RESULT: SUM=X COUNT=Y")
7. Parse and aggregate

**Interactive Terminal UI:**
```
+==================================================+
|          C2 Distributed Map-Reduce Demo          |
+==================================================+
+----------------------+ +----------------------+
| [Node 1] Port 9001   | | [Node 2] Port 9002   |
+----------------------+ +----------------------+
| Starting on port...  | | Starting on port...  |
| C2 bot listening...  | | C2 bot listening...  |
|                      | |                      |
+----------------------+ +----------------------+
+----------------------+ +----------------------+
| [Node 3] Port 9003   | | [Node 4] Port 9004   |
+----------------------+ +----------------------+
| Starting on port...  | | Starting on port...  |
| C2 bot listening...  | | C2 bot listening...  |
+----------------------+ +----------------------+

=== Master Control Log ===
[12:50:18] Starting 4-node cluster...
[12:50:18] All 4 nodes ready!
[12:50:23] -> Sending job to Node 1...
...

Commands: [r] Run Map-Reduce  [c] Clear Logs  [q] Quit
```

## Usage

### Interactive Mode
```bash
python tools/c2_master.py

# Press 'r' to run map-reduce job
# Press 'c' to clear logs
# Press 'q' to quit
```

### Automated Test Mode
```bash
python tools/c2_master.py --auto --timeout 40
```

### Build Prerequisites
```bash
# Build the map-reduce worker
python tools/build_and_run.py --build-guest map_reduce_worker

# Build full system (includes worker in flash image)
python tools/build_and_run.py --build
```

## Demo Results

The map-reduce demo distributes computation of sum(1..1000) across 4 nodes:

```
=== Master Log ===
[12:50:23] Split into 4 chunks of ~250 items
[12:50:23] -> Sending job to Node 1...
[12:50:23] -> Sending job to Node 2...
[12:50:23] -> Sending job to Node 3...
[12:50:23] -> Sending job to Node 4...
[12:50:26] [OK] Node 3: sum=115610, count=250
[12:50:26] [OK] Node 4: sum=132122, count=250
[12:50:26] [OK] Node 2: sum=127113, count=250
[12:50:26] [OK] Node 1: sum=125655, count=250
[12:50:26] ========================================
[12:50:26] Job Complete!
[12:50:26]   Total Sum:    500500
[12:50:26]   Total Count:  1000
[12:50:26]   Expected Sum: 500500
[12:50:26] [OK] SUCCESS: Results match perfectly!

==================================================
MAP-REDUCE TEST RESULT
==================================================
[OK] TEST PASSED
```

## Technical Challenges Solved

### 1. Windows Unicode Encoding
ANSI box-drawing characters and emoji cause encoding errors on Windows cmd.exe.
**Solution:** ASCII fallback characters (`+-|` instead of `+----|`)

### 2. QEMU Multi-Instance
Multiple QEMU instances need unique port mappings.
**Solution:** Port forwarding with hostfwd (`-nic user,model=open_eth,hostfwd=tcp::{port}-:9000`)

### 3. Stdin Redirection on ESP32
ESP32's newlib stdin is hardcoded to UART.
**Solution:** Intercept read() on fd=0 in shim layer, redirect to socket when flag is set.

### 4. Connection Timing
Nodes need time to boot before accepting connections.
**Solution:** `wait_for_node_ready()` with configurable timeout polls port until connection succeeds.

## Files Modified/Created

| File | Change |
|------|--------|
| `main/syscalls/shim_unistd.c` | Added stdin redirection support |
| `apps/map_reduce_worker/main.c` | Created map-reduce worker |
| `tools/c2_master.py` | Complete rewrite with multi-node support |
| `tools/export_symbols.py` | Added fgets symbol export |

## Future Improvements

1. **Fault Tolerance**: Retry failed nodes, redistribute work
2. **Dynamic Scaling**: Add/remove nodes at runtime
3. **Progress Tracking**: Real-time progress bars for long jobs
4. **Different Algorithms**: Word count, matrix multiplication, etc.
5. **Real Hardware**: Test on physical ESP32 cluster via WiFi
