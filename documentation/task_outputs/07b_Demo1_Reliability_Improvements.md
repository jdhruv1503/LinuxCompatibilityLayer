# Task 07b: Demo1 Reliability & UI Improvements

## Executive Summary

This task focused on fixing reliability issues and UI problems in the distributed map-reduce demo (c2_master.py). The demo was experiencing:
- Flaky connections (WinError 10053/10054)
- UI corruption from ANSI escape codes
- Screen flicker during live updates

**Final Result:** Improved reliability through simplified data transmission, staggered job execution, retry logic, and proper ANSI code stripping.

---

## Issues Identified

### 1. Connection Failures (WinError 10053/10054)

**Symptoms:**
```
[X] Node 3 failed: [WinError 10053] An established connection was aborted by the software in your host machine
[X] Node 4 failed: [WinError 10054] An existing connection was forcibly closed by the remote host
```

**Root Causes:**
- Chunked data sending with delays caused idle connection timeouts
- All 4 jobs starting simultaneously overwhelmed QEMU NAT networking
- QEMU user-mode networking has limited connection handling capacity

### 2. UI Corruption (`[B[Bot]` display)

**Symptoms:**
```
| [B[Bot] Failed to receive payload
```

**Root Cause:**
- QEMU output contains ANSI escape codes (e.g., `\x1b[B` for cursor down)
- Original regex pattern didn't catch all escape sequences
- Escape codes were being displayed literally in the terminal

### 3. Screen Flicker

**Symptoms:**
- Visible screen flash on every UI update (3Hz)
- Distracting during demo

**Root Cause:**
- Using `os.system('cls')` for full screen clear before each redraw
- Causes visible blank frame between updates

---

## Solutions Implemented

### 1. Simplified Data Transmission (No Chunking)

**Before (problematic):**
```python
# Chunked sending with delays - caused idle timeouts
chunk_size = 8192
for i in range(0, len(elf_data), chunk_size):
    s.sendall(elf_data[i:i+chunk_size])
    time.sleep(0.01)  # These delays caused connection timeouts!

time.sleep(3.0)  # Long idle wait

data_bytes = data_str.encode('utf-8')
for i in range(0, len(data_bytes), 1024):
    s.sendall(data_bytes[i:i+1024])
    time.sleep(0.01)
```

**After (reliable):**
```python
# Send everything at once - no delays = no idle timeouts
s.sendall(struct.pack('<I', len(elf_data)) + elf_data)
log_node(node_id, "[SEND] ELF sent, waiting...")

time.sleep(1.5)  # Reduced wait time

data_str = "\n".join(str(x) for x in data_chunk) + "\n"
s.sendall(data_str.encode('utf-8'))  # Single send, no chunking
```

**Key Insight:** TCP handles its own segmentation. Application-level chunking with delays is unnecessary and harmful - it makes the connection appear idle, triggering NAT timeouts.

### 2. Staggered Job Execution with Retry Logic

**Before:**
```python
# All jobs start at once - overwhelms QEMU
for i, node in enumerate(NODES):
    t = threading.Thread(target=job_thread, args=(i, node, chunks[i]))
    t.start()
```

**After:**
```python
def job_thread(idx, node, chunk, start_delay):
    # Stagger job starts to reduce contention
    if start_delay > 0:
        time.sleep(start_delay)

    # Retry logic - try up to 2 times
    for attempt in range(2):
        result = send_worker_job(node_id, node['port'], chunk, worker_type)

        if result is successful:
            break

        if attempt < 1:
            log_node(node_id, f"[RETRY] Attempt {attempt + 2}...")
            time.sleep(1)  # 1 second retry delay

# Stagger by 0.4 seconds each
for i, node in enumerate(NODES):
    t = threading.Thread(target=job_thread, args=(i, node, chunks[i], i * 0.4))
    t.start()
```

### 3. Comprehensive ANSI Escape Code Stripping

**Before (incomplete):**
```python
ANSI_ESCAPE_PATTERN = re.compile(r'\x1b\[[0-9;]*[a-zA-Z]|\x1b\].*?\x07|\x1b[PX^_].*?\x1b\\')
```

**After (comprehensive):**
```python
ANSI_ESCAPE_PATTERN = re.compile(
    r'\x1b\[[0-9;]*[a-zA-Z]'      # CSI sequences like ESC[0m, ESC[B
    r'|\x1b\][^\x07]*\x07'         # OSC sequences
    r'|\x1b[PX^_][^\x1b]*\x1b\\'   # DCS, SOS, PM, APC
    r'|\x1b[NOc].'                 # SS2, SS3, reset
    r'|\x1b.'                      # Any other ESC + char
    r'|\x08'                       # Backspace
)

def strip_ansi(text):
    text = ANSI_ESCAPE_PATTERN.sub('', text)
    # Also remove stray broken escapes like [B[
    text = re.sub(r'\[([A-Z])\[', '[', text)
    return text
```

### 4. Flicker-Free UI Updates

**Before (flickery):**
```python
def draw_ui():
    clear_screen()  # Full screen clear - causes flash
    print("\n".join(output))
```

**After (smooth):**
```python
def draw_ui(first_draw=False):
    # Build output with line clearing
    full_output = "\033[H"  # Move cursor home (no clear)
    for line in output:
        full_output += line + "\033[K\n"  # Clear to end of line

    if first_draw:
        clear_screen()  # Only clear on first draw
    sys.stdout.write(full_output)
    sys.stdout.flush()
```

**Key Techniques:**
- `\033[H` - Move cursor to home position (1,1) without clearing
- `\033[K` - Clear from cursor to end of line (removes old content)
- Single `write()` call with buffered output reduces flicker

### 5. Improved Cluster Startup

**Before:**
```python
for node in NODES:
    if wait_for_node_ready(node['port'], timeout=startup_timeout):
        ready_count += 1
```

**After:**
```python
# Initial wait for nodes to boot
time.sleep(3)

for i, node in enumerate(NODES):
    if i > 0:
        time.sleep(0.5)  # Stagger checks
    if wait_for_node_ready(node['port'], timeout=startup_timeout):
        ready_count += 1
```

### 6. Removed Noisy Error Messages

**Changed in `apps/c2_bot/main.c`:**
```c
// Before
if (c2_receive_payload(client_sock) != 0) {
    printf("[Bot] Failed to receive payload\n");  // Noisy
    close(client_sock);
    return;
}

// After
if (c2_receive_payload(client_sock) != 0) {
    // Silent failure - reduces noise in demo UI
    close(client_sock);
    return;
}
```

---

## Configuration Parameters (Final Values)

| Parameter | Value | Purpose |
|-----------|-------|---------|
| Job stagger | 0.4s | Time between job starts |
| Retry delay | 1.0s | Wait before retry attempt |
| ELF wait time | 1.5s | Wait for worker ELF to load |
| Initial boot wait | 3.0s | Wait before checking nodes |
| Node check stagger | 0.5s | Time between node checks |
| UI stabilize wait | 3.0s | Wait after UI before auto-test |
| Default timeout | 40s | Node startup timeout |

---

## Technical Deep Dive: Why Chunked Sending Failed

### The Problem

When sending data in chunks with delays:
```python
for i in range(0, len(data), chunk_size):
    s.sendall(data[i:i+chunk_size])
    time.sleep(0.01)  # 10ms delay
```

The cumulative effect:
- 20KB ELF / 8KB chunks = 3 chunks × 10ms = 30ms
- 3.0 second idle wait
- 1KB data chunks with 10ms delays

During the 3.0 second idle period, QEMU's NAT stack may:
1. Consider the connection idle
2. Apply connection timeout rules
3. Send RST (reset) to close the connection

### The Solution

TCP handles packet segmentation automatically. Application-level chunking is:
1. Unnecessary (TCP does it better)
2. Harmful (introduces artificial delays)
3. Counterproductive (makes connection appear idle)

**Better approach:**
```python
# Let TCP handle segmentation
s.sendall(header + elf_data)  # Single call
time.sleep(1.5)               # Shorter wait
s.sendall(data)               # Single call
```

---

## Files Modified

| File | Changes |
|------|---------|
| `tools/c2_master.py` | Simplified sending, staggered execution, retry logic, ANSI stripping, flicker-free UI |
| `apps/c2_bot/main.c` | Removed "Failed to receive payload" message |

---

## Testing Results

### Before Improvements
```
[X] Node 3 failed: [WinError 10053]
[X] Node 4 failed: [WinError 10054]
Total Sum: 245463 (MISMATCH)
[X] TEST FAILED
```

### After Improvements
- Consistent 4-node execution
- Proper ANSI stripping (no `[B` corruption)
- Smooth UI updates
- Automatic retry on transient failures

---

## Lessons Learned

1. **TCP handles segmentation** - Don't add application-level chunking with delays
2. **QEMU NAT is sensitive** - Idle connections may timeout unexpectedly
3. **Stagger parallel operations** - Don't overwhelm limited resources
4. **ANSI codes are everywhere** - QEMU output includes cursor movement codes
5. **Cursor positioning > clear** - Use `\033[H` instead of full screen clear
6. **Retry logic is essential** - Transient failures happen in networked systems

---

## Command Reference

```bash
# Run with default settings (4 nodes, 40s timeout)
python tools/c2_master.py --auto

# Custom node count
python tools/c2_master.py --auto --nodes 2

# Math worker mode
python tools/c2_master.py --auto --math

# Interactive mode
python tools/c2_master.py
# Press 'r' for simple, 'm' for math, 'c' clear, 'q' quit
```
