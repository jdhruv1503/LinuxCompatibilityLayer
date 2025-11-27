#!/usr/bin/env python3
"""
C2 Master - Distributed Map-Reduce Demo Controller
===================================================

Manages a cluster of 4 ESP32 QEMU instances, each running the C2 bot.
Orchestrates distributed computing tasks (map-reduce) across the cluster.

Features:
- Spawns and manages 4 concurrent QEMU instances
- Split-screen terminal UI showing all node outputs
- Interactive mode with menu-driven interface
- Non-interactive mode (--auto) for automated testing
- Map-reduce demo: distributes data across nodes, aggregates results

Usage:
    python c2_master.py              # Interactive mode
    python c2_master.py --auto       # Automated test mode (run and exit)
    python c2_master.py --timeout 60 # Custom startup timeout
"""

import sys
import os
import time
import socket
import struct
import subprocess
import threading
import random
import signal
import argparse
import shutil
import re
from datetime import datetime

# Windows-specific imports
try:
    import msvcrt
    HAS_MSVCRT = True
except ImportError:
    HAS_MSVCRT = False

# Configuration paths
QEMU_PATH = r"C:\Users\Dhruv\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe"
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MERGED_FLASH = os.path.join(PROJECT_ROOT, "build", "merged-flash.bin")
WORKER_ELF = os.path.join(PROJECT_ROOT, "data", "map_reduce_worker.elf")
MATH_WORKER_ELF = os.path.join(PROJECT_ROOT, "data", "math_worker.elf")

# Worker mode (set by command line)
g_worker_mode = "simple"  # "simple" or "math"

# Node configuration - 4 ESP32 instances with different ports
NODES = [
    {"id": 1, "port": 9001, "name": "Node-1"},
    {"id": 2, "port": 9002, "name": "Node-2"},
    {"id": 3, "port": 9003, "name": "Node-3"},
    {"id": 4, "port": 9004, "name": "Node-4"},
]

# ANSI escape codes for terminal colors
RESET = "\033[0m"
BOLD = "\033[1m"
RED = "\033[31m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
BLUE = "\033[34m"
MAGENTA = "\033[35m"
CYAN = "\033[36m"
WHITE = "\033[37m"
DIM = "\033[2m"

# Cross-platform symbols (ASCII for Windows compatibility)
CHECK = "[OK]"
CROSS = "[X]"
ARROW = "->"

# Regex to strip ANSI escape codes from QEMU output
# Matches: ESC[...X, ESC ## \S+ sequences, and control chars
ANSI_ESCAPE_PATTERN = re.compile(
    r'\x1b\[[0-9;]*[a-zA-Z]'  # CSI sequences like ESC[0m, ESC[B
    r'|\x1b\][^\x07]*\x07'     # OSC sequences
    r'|\x1b[PX^_][^\x1b]*\x1b\\'  # DCS, SOS, PM, APC
    r'|\x1b[NOc].'             # SS2, SS3, reset
    r'|\x1b.'                  # Any other ESC + char
    r'|\x08'                   # Backspace
)


def strip_ansi(text):
    """Remove ANSI escape codes from text."""
    # First strip escape sequences
    text = ANSI_ESCAPE_PATTERN.sub('', text)
    # Also remove any stray [ followed by single letter (broken escape)
    text = re.sub(r'\[([A-Z])\[', '[', text)
    return text


# Global state
g_running = True
g_node_processes = {}  # node_id -> subprocess.Popen
g_node_threads = {}    # node_id -> thread
g_node_logs = {1: [], 2: [], 3: [], 4: []}  # Rolling log buffers
g_node_ready = {1: False, 2: False, 3: False, 4: False}  # Track if "Waiting for payload" seen
g_master_log = []
g_log_lock = threading.Lock()
g_job_results = {}

MAX_LOG_LINES = 8


def log_master(msg, color=WHITE):
    """Log a message from the master controller."""
    timestamp = datetime.now().strftime("%H:%M:%S")
    formatted = f"[{timestamp}] {msg}"
    with g_log_lock:
        g_master_log.append((formatted, color))
        if len(g_master_log) > 15:
            g_master_log.pop(0)


def log_node(node_id, msg):
    """Log a message from a node."""
    # Strip ANSI escape codes from QEMU output
    msg = strip_ansi(msg)

    # Filter out noisy debug messages
    if any(skip in msg for skip in ['memory_layout', 'heap_init', 'cpu_start',
                                      'esp_netif', 'event:', 'intr_alloc',
                                      'efuse:', 'spi_flash', 'esp_eth']):
        return

    # Skip empty messages
    if not msg.strip():
        return

    # Check for ready message (before any filtering)
    if "Waiting for payload" in msg:
        g_node_ready[node_id] = True

    with g_log_lock:
        g_node_logs[node_id].append(msg)
        if len(g_node_logs[node_id]) > MAX_LOG_LINES:
            g_node_logs[node_id].pop(0)


def clear_screen():
    """Clear the terminal screen."""
    os.system('cls' if os.name == 'nt' else 'clear')


def move_cursor_home():
    """Move cursor to home position without clearing (reduces flicker)."""
    # ANSI escape: move to position (1,1) and clear from cursor to end
    sys.stdout.write("\033[H")
    sys.stdout.flush()


def get_terminal_size():
    """Get terminal dimensions."""
    try:
        size = os.get_terminal_size()
        return size.columns, size.lines
    except:
        return 100, 40


def draw_box(title, content_lines, width, color=CYAN):
    """Draw a bordered box with title (ASCII for Windows compatibility)."""
    lines = []
    lines.append(f"{color}+{'-' * (width - 2)}+{RESET}")
    lines.append(f"{color}|{RESET} {BOLD}{title}{RESET}{' ' * (width - len(title) - 4)}{color}|{RESET}")
    lines.append(f"{color}+{'-' * (width - 2)}+{RESET}")

    for line in content_lines:
        truncated = line[:width - 4] if len(line) > width - 4 else line
        padding = ' ' * (width - len(truncated) - 4)
        lines.append(f"{color}|{RESET} {truncated}{padding} {color}|{RESET}")

    # Fill remaining space
    empty_lines = MAX_LOG_LINES - len(content_lines)
    for _ in range(max(0, empty_lines)):
        lines.append(f"{color}|{RESET}{' ' * (width - 2)}{color}|{RESET}")

    lines.append(f"{color}+{'-' * (width - 2)}+{RESET}")
    return lines


def draw_ui(first_draw=False):
    """Draw the main UI with node status and master log."""
    if not g_running:
        return

    width, height = get_terminal_size()
    half_width = (width - 3) // 2

    output = []

    # Header
    header = f"{BOLD}{CYAN}+{'=' * (width - 2)}+{RESET}"
    title = "C2 Distributed Map-Reduce Demo"
    title_line = f"{BOLD}{CYAN}|{RESET}{title:^{width-2}}{BOLD}{CYAN}|{RESET}"
    header_bottom = f"{BOLD}{CYAN}+{'=' * (width - 2)}+{RESET}"
    output.append(header)
    output.append(title_line)
    output.append(header_bottom)

    # Node boxes - top row (nodes 1 and 2)
    with g_log_lock:
        box1 = draw_box(f"[Node 1] Port 9001", g_node_logs[1].copy(), half_width, YELLOW)
        box2 = draw_box(f"[Node 2] Port 9002", g_node_logs[2].copy(), half_width, YELLOW)

    for i in range(len(box1)):
        output.append(f"{box1[i]} {box2[i]}")

    # Node boxes - bottom row (nodes 3 and 4)
    with g_log_lock:
        box3 = draw_box(f"[Node 3] Port 9003", g_node_logs[3].copy(), half_width, YELLOW)
        box4 = draw_box(f"[Node 4] Port 9004", g_node_logs[4].copy(), half_width, YELLOW)

    for i in range(len(box3)):
        output.append(f"{box3[i]} {box4[i]}")

    # Master log section
    output.append("")
    output.append(f"{BOLD}{GREEN}=== Master Control Log ==={RESET}")

    with g_log_lock:
        master_msgs = g_master_log[-8:]
        for msg, color in master_msgs:
            output.append(f"{color}{msg}{RESET}")
        # Pad master log area to prevent shifting
        for _ in range(8 - len(master_msgs)):
            output.append("")

    # Padding to fill screen
    while len(output) < height - 3:
        output.append("")

    # Command bar at bottom
    output.append(f"{DIM}{'-' * width}{RESET}")
    output.append(f"{BOLD}Commands:{RESET} [r] Simple  [m] Math Worker  [c] Clear  [q] Quit")

    # Build full output with line clearing for clean overwrites
    # Use \033[K to clear to end of line - prevents artifacts
    full_output = "\033[H"  # Move cursor home
    for line in output:
        full_output += line + "\033[K\n"

    # Write all at once to reduce flicker
    if first_draw:
        clear_screen()
    sys.stdout.write(full_output)
    sys.stdout.flush()


class NodeRunner(threading.Thread):
    """Thread that manages a single QEMU instance."""

    def __init__(self, node_config):
        super().__init__()
        self.config = node_config
        self.node_id = node_config['id']
        self.port = node_config['port']
        self.process = None
        self.daemon = True
        self._stop_event = threading.Event()

    def run(self):
        """Start QEMU and capture output."""
        if not os.path.exists(MERGED_FLASH):
            log_node(self.node_id, f"ERROR: Flash image not found")
            return

        # Build QEMU command with port forwarding
        cmd = [
            QEMU_PATH,
            "-nographic",
            "-machine", "esp32",
            "-drive", f"file={MERGED_FLASH},if=mtd,format=raw",
            "-no-reboot",
            "-nic", f"user,model=open_eth,hostfwd=tcp::{self.port}-:9000"
        ]

        log_node(self.node_id, f"Starting on port {self.port}...")

        try:
            # Start QEMU process
            self.process = subprocess.Popen(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                cwd=PROJECT_ROOT,
                bufsize=1  # Line buffered
            )
            g_node_processes[self.node_id] = self.process

            # Read output until stopped
            while not self._stop_event.is_set() and self.process.poll() is None:
                try:
                    line = self.process.stdout.readline()
                    if line:
                        clean = line.strip()
                        if clean:
                            log_node(self.node_id, clean)
                except:
                    break

        except Exception as e:
            log_node(self.node_id, f"ERROR: {e}")
        finally:
            if self.node_id in g_node_processes:
                del g_node_processes[self.node_id]

    def stop(self):
        """Stop the QEMU process."""
        self._stop_event.set()
        if self.process:
            log_node(self.node_id, "Shutting down...")
            try:
                self.process.terminate()
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
            except:
                pass


def wait_for_node_ready_by_log(node_id, timeout=30):
    """Wait until a node's C2 server prints 'Waiting for payload...' message."""
    start = time.time()
    while time.time() - start < timeout:
        # Check the persistent ready flag (set by log_node when it sees the message)
        if g_node_ready.get(node_id, False):
            # Found the ready message - wait 1 more second for stability
            time.sleep(1.0)
            return True
        time.sleep(0.5)
    return False


def wait_for_node_ready(port, timeout=30):
    """Wait until a node's C2 server is accepting connections."""
    start = time.time()
    time.sleep(2.0)
    while time.time() - start < timeout:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(1)
            s.connect(('localhost', port))
            s.close()
            return True
        except:
            time.sleep(0.5)
    return False


def send_worker_job(node_id, port, data_chunk, worker_type="simple"):
    """
    Send a map-reduce job to a node.

    Protocol:
    1. Connect to C2 bot on port
    2. Send ELF size (4 bytes, little-endian)
    3. Send ELF binary
    4. Wait for worker to start, then send data
    5. Close write end to signal EOF
    6. Read result

    worker_type: "simple" (sum only) or "math" (uses math functions)
    """
    s = None
    try:
        log_node(node_id, "[CONN] Connecting...")

        # Select worker ELF based on type
        worker_path = MATH_WORKER_ELF if worker_type == "math" else WORKER_ELF

        # Read worker ELF
        with open(worker_path, 'rb') as f:
            elf_data = f.read()

        # Connect to node with TCP keepalive
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        s.settimeout(60)  # Longer timeout for reliability
        s.connect(('localhost', port))
        log_node(node_id, "[CONN] Connected!")

        # Send ELF size header + ELF data in one go (no chunking delays)
        log_node(node_id, f"[SEND] ELF ({len(elf_data)} bytes)...")
        s.sendall(struct.pack('<I', len(elf_data)) + elf_data)
        log_node(node_id, "[SEND] ELF sent, waiting...")

        # Wait for worker to start and be ready for data
        # ESP32 needs time to load and relocate ELF
        time.sleep(1.5)

        # Send the data (numbers as newline-separated string) - all at once
        log_node(node_id, f"[DATA] Sending {len(data_chunk)} numbers...")
        data_str = "\n".join(str(x) for x in data_chunk) + "\n"
        s.sendall(data_str.encode('utf-8'))
        log_node(node_id, "[DATA] Data sent!")

        # Signal EOF by closing write side
        s.shutdown(socket.SHUT_WR)
        log_node(node_id, "[WAIT] Computing...")

        # Read response with longer timeout
        s.settimeout(30)
        response = b""
        while True:
            try:
                chunk = s.recv(4096)
                if not chunk:
                    break
                response += chunk
            except socket.timeout:
                break

        s.close()
        s = None

        # Parse result
        text = response.decode('utf-8', errors='replace')

        if worker_type == "math":
            # Parse math worker result: "RESULT: LINEAR_SUM=X TRIG_AVG=Y GEO_MEAN=Z RMS=W COUNT=N"
            match = re.search(r"RESULT:\s*LINEAR_SUM=(-?\d+)\s+TRIG_AVG=([\d.]+)\s+GEO_MEAN=([\d.]+)\s+RMS=([\d.]+)\s+COUNT=(\d+)", text)
            if match:
                linear_sum = int(match.group(1))
                trig_avg = float(match.group(2))
                geo_mean = float(match.group(3))
                rms = float(match.group(4))
                count = int(match.group(5))
                log_master(f"{CHECK} Node {node_id}: sum={linear_sum}, trig_avg={trig_avg:.4f}", GREEN)
                log_node(node_id, f"[DONE] sum={linear_sum}")
                return {"linear_sum": linear_sum, "trig_avg": trig_avg, "geo_mean": geo_mean, "rms": rms, "count": count}
            else:
                log_master(f"{CROSS} Node {node_id}: Invalid math response", RED)
                log_node(node_id, f"[ERR] Bad response")
                return None
        else:
            # Parse simple worker result: "RESULT: SUM=X COUNT=Y"
            match = re.search(r"RESULT:\s*SUM=(\d+)\s+COUNT=(\d+)", text)
            if match:
                result_sum = int(match.group(1))
                result_count = int(match.group(2))
                log_master(f"{CHECK} Node {node_id}: sum={result_sum}, count={result_count}", GREEN)
                return result_sum, result_count
            else:
                log_master(f"{CROSS} Node {node_id}: Invalid response", RED)
                log_node(node_id, f"[ERR] Bad response")
                return 0, 0

    except Exception as e:
        log_master(f"{CROSS} Node {node_id} failed: {e}", RED)
        log_node(node_id, f"[ERR] {str(e)[:30]}")
        return None if worker_type == "math" else (0, 0)
    finally:
        if s:
            try:
                s.close()
            except:
                pass


def run_mapreduce_demo(live_ui=True, worker_type="simple"):
    """Run a distributed map-reduce computation across all nodes."""
    log_master("=" * 40, CYAN)
    log_master(f"Starting Map-Reduce Job ({worker_type} worker)", CYAN)
    log_master("=" * 40, CYAN)

    # Generate test data: sum of 1 to 1000
    log_master("Generating dataset: integers 1-1000")
    data = list(range(1, 1001))
    random.shuffle(data)

    # Split into chunks based on number of nodes
    num_nodes = len(NODES)
    chunk_size = len(data) // num_nodes
    chunks = []
    for i in range(num_nodes):
        start = i * chunk_size
        end = start + chunk_size if i < num_nodes - 1 else len(data)
        chunks.append(data[start:end])

    log_master(f"Split into {len(chunks)} chunks of ~{chunk_size} items")

    # Send jobs to all nodes in parallel
    results = [None] * num_nodes
    job_done = [False] * num_nodes
    threads = []

    def job_thread(idx, node, chunk, start_delay):
        node_id = node['id']
        # Stagger job starts to reduce contention
        if start_delay > 0:
            time.sleep(start_delay)

        log_node(node_id, f"[START] Job: {len(chunk)} items")

        # Retry logic - try up to 2 times
        result = None
        for attempt in range(2):
            result = send_worker_job(node_id, node['port'], chunk, worker_type=worker_type)

            # Check if successful
            if worker_type == "math":
                if result is not None:
                    break
            else:
                if result[0] > 0:
                    break

            # Failed - retry after delay
            if attempt < 1:
                log_node(node_id, f"[RETRY] Attempt {attempt + 2}...")
                time.sleep(1)

        results[idx] = result
        job_done[idx] = True
        if worker_type == "math":
            if result is not None:
                log_node(node_id, f"[DONE] sum={result['linear_sum']}")
            else:
                log_node(node_id, f"[FAIL] Job failed!")
        else:
            if result[0] > 0:
                log_node(node_id, f"[DONE] sum={result[0]}")
            else:
                log_node(node_id, f"[FAIL] Job failed!")

    # Stagger thread starts by 0.4 seconds each to reduce contention
    for i, node in enumerate(NODES):
        t = threading.Thread(target=job_thread, args=(i, node, chunks[i], i * 0.4))
        t.start()
        threads.append(t)

    # Live UI update loop - redraw while jobs are running
    if live_ui:
        first_draw = True
        while not all(job_done):
            draw_ui(first_draw=first_draw)
            first_draw = False
            time.sleep(0.3)  # Update 3 times per second
        # Final draw after completion
        draw_ui(first_draw=False)

    # Wait for all jobs to complete
    for t in threads:
        t.join(timeout=60)

    # Aggregate results based on worker type
    if worker_type == "math":
        total_sum = 0
        total_count = 0
        trig_avg_sum = 0.0
        valid_nodes = 0

        for r in results:
            if r is not None:
                total_sum += r['linear_sum']
                total_count += r['count']
                trig_avg_sum += r['trig_avg']
                valid_nodes += 1

        # Calculate expected values
        expected_sum = sum(range(1, 1001))  # 500500
        expected_trig_avg = 1.0  # sin^2 + cos^2 = 1

        # Report results
        log_master("=" * 40, CYAN)
        log_master(f"Math Worker Job Complete!", BOLD)
        log_master(f"  Total Sum:      {total_sum}")
        log_master(f"  Total Count:    {total_count}")
        log_master(f"  Expected Sum:   {expected_sum}")
        if valid_nodes > 0:
            avg_trig = trig_avg_sum / valid_nodes
            log_master(f"  Avg Trig Check: {avg_trig:.4f} (expect ~1.0)")

        if total_sum == expected_sum and total_count == 1000:
            log_master(f"{CHECK} SUCCESS: Sum matches perfectly!", GREEN)
            if valid_nodes > 0 and abs(avg_trig - 1.0) < 0.01:
                log_master(f"{CHECK} Math identity verified!", GREEN)
            return True
        else:
            log_master(f"{CROSS} MISMATCH: Results don't match!", RED)
            return False
    else:
        # Simple worker aggregation
        total_sum = 0
        total_count = 0
        for r in results:
            if r is not None and isinstance(r, tuple):
                total_sum += r[0]
                total_count += r[1]

        # Calculate expected value
        expected_sum = sum(range(1, 1001))  # 500500

        # Report results
        log_master("=" * 40, CYAN)
        log_master(f"Job Complete!", BOLD)
        log_master(f"  Total Sum:    {total_sum}")
        log_master(f"  Total Count:  {total_count}")
        log_master(f"  Expected Sum: {expected_sum}")

        if total_sum == expected_sum and total_count == 1000:
            log_master(f"{CHECK} SUCCESS: Results match perfectly!", GREEN)
            return True
        else:
            log_master(f"{CROSS} MISMATCH: Results don't match!", RED)
            return False


def start_cluster(startup_timeout=20):
    """Start all QEMU instances."""
    log_master(f"Starting {len(NODES)}-node cluster...", CYAN)

    # Reset ready flags
    for node_id in g_node_ready:
        g_node_ready[node_id] = False

    threads = []
    for node in NODES:
        runner = NodeRunner(node)
        runner.start()
        threads.append(runner)
        g_node_threads[node['id']] = runner
        time.sleep(0.5)  # Stagger startup slightly

    # Wait for all nodes to be ready by checking for "Waiting for payload..." in logs
    log_master("Waiting for nodes to initialize...", YELLOW)
    log_master("Looking for 'Waiting for payload...' in QEMU output", DIM)
    time.sleep(5)  # Initial wait for nodes to boot and start C2 server

    ready_count = 0
    for i, node in enumerate(NODES):
        if i > 0:
            time.sleep(0.5)  # Stagger checks
        log_master(f"  Checking Node {node['id']} logs...", DIM)
        # Use log-based ready check - waits for "[Bot] Waiting for payload..." + 1s
        if wait_for_node_ready(node['id'], timeout=startup_timeout):
            log_master(f"  {CHECK} Node {node['id']} ready (saw 'Waiting for payload')", GREEN)
            ready_count += 1
        else:
            # Fallback to port check if log message not found
            log_master(f"  Log check failed, trying port {node['port']}...", YELLOW)
            if wait_for_node_ready(node['port'], timeout=10):
                log_master(f"  {CHECK} Node {node['id']} port ready (fallback)", GREEN)
                ready_count += 1
                time.sleep(1)  # Extra wait since log wasn't seen
            else:
                log_master(f"  {CROSS} Node {node['id']} failed to start", RED)

    if ready_count == len(NODES):
        log_master(f"All {len(NODES)} nodes ready!", GREEN)
        return True
    else:
        log_master(f"Only {ready_count}/{len(NODES)} nodes ready", YELLOW)
        return ready_count > 0


def stop_cluster():
    """Stop all QEMU instances."""
    log_master("Stopping cluster...", YELLOW)

    for node_id, runner in g_node_threads.items():
        runner.stop()

    # Wait for threads to finish
    for runner in g_node_threads.values():
        runner.join(timeout=5)

    g_node_threads.clear()
    log_master("Cluster stopped", GREEN)


def interactive_mode():
    """Run in interactive mode with menu."""
    global g_running

    log_master("Interactive mode started", CYAN)
    log_master("Press 'r' simple, 'm' math, 'c' clear, 'q' quit", WHITE)

    last_draw = 0
    draw_interval = 0.5  # Redraw every 500ms
    first_draw = True

    try:
        while g_running:
            # Redraw UI periodically
            now = time.time()
            if now - last_draw > draw_interval:
                draw_ui(first_draw=first_draw)
                first_draw = False
                last_draw = now

            # Check for keyboard input (Windows)
            if HAS_MSVCRT and msvcrt.kbhit():
                key = msvcrt.getch().decode('utf-8', errors='ignore').lower()

                if key == 'q':
                    g_running = False
                    break
                elif key == 'r':
                    log_master("Starting simple map-reduce job...", CYAN)
                    run_mapreduce_demo(live_ui=True, worker_type="simple")
                    log_master("Press 'r' simple, 'm' math, 'q' quit", WHITE)
                elif key == 'm':
                    log_master("Starting MATH map-reduce job...", CYAN)
                    run_mapreduce_demo(live_ui=True, worker_type="math")
                    log_master("Press 'r' simple, 'm' math, 'q' quit", WHITE)
                elif key == 'c':
                    # Clear logs
                    with g_log_lock:
                        for n in g_node_logs:
                            g_node_logs[n].clear()
                        g_master_log.clear()
                    log_master("Logs cleared", WHITE)

            time.sleep(0.1)

    except KeyboardInterrupt:
        g_running = False


def auto_mode(worker_type="simple"):
    """Run automated test and exit with live UI."""
    log_master(f"Auto mode: Running {worker_type} test...", CYAN)

    # Clear screen and start UI
    clear_screen()

    # Wait 3 seconds after UI starts before testing
    log_master("Waiting 3 seconds for nodes to stabilize...", YELLOW)
    draw_ui(first_draw=True)
    time.sleep(3)

    log_master(f"Executing {worker_type} map-reduce job...", CYAN)

    # Run with live UI so you can see what's happening
    success = run_mapreduce_demo(live_ui=True, worker_type=worker_type)

    # Final UI update with results
    draw_ui(first_draw=False)
    time.sleep(1)

    # Print final summary
    print("\n" + "=" * 50)
    print(f"MAP-REDUCE TEST RESULT ({worker_type.upper()} WORKER)")
    print("=" * 50)

    # Print master log
    print(f"\n{BOLD}=== Final Master Log ==={RESET}")
    with g_log_lock:
        for msg, color in g_master_log[-10:]:
            print(f"{color}{msg}{RESET}")

    print("")
    if success:
        print(f"{GREEN}{CHECK} TEST PASSED{RESET}")
        return 0
    else:
        print(f"{RED}{CROSS} TEST FAILED{RESET}")
        return 1


def main():
    global g_running

    parser = argparse.ArgumentParser(
        description="C2 Master - Distributed Map-Reduce Demo",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python c2_master.py              # Interactive mode
  python c2_master.py --auto       # Automated test (simple worker)
  python c2_master.py --auto --math # Automated test with math worker
  python c2_master.py --timeout 30 # Custom startup timeout
"""
    )
    parser.add_argument("--auto", action="store_true",
                       help="Run automated test and exit")
    parser.add_argument("--math", action="store_true",
                       help="Use math worker (with trig functions) instead of simple worker")
    parser.add_argument("--timeout", type=int, default=40,
                       help="Node startup timeout in seconds (default: 40, recommend 40+ for 4 nodes)")
    parser.add_argument("--nodes", type=int, default=4, choices=[1, 2, 3, 4],
                       help="Number of nodes to use (default: 4)")

    args = parser.parse_args()

    worker_type = "math" if args.math else "simple"

    # Limit number of nodes if requested
    global NODES
    if args.nodes < 4:
        NODES = NODES[:args.nodes]

    # Verify prerequisites
    if not os.path.exists(MERGED_FLASH):
        print(f"{RED}ERROR: Flash image not found: {MERGED_FLASH}{RESET}")
        print("Run: python tools/build_and_run.py --build")
        return 1

    worker_elf = MATH_WORKER_ELF if args.math else WORKER_ELF
    if not os.path.exists(worker_elf):
        print(f"{RED}ERROR: Worker ELF not found: {worker_elf}{RESET}")
        worker_name = "math_worker" if args.math else "map_reduce_worker"
        print(f"Run: python tools/build_and_run.py --build-guest {worker_name}")
        return 1

    if not os.path.exists(QEMU_PATH):
        print(f"{RED}ERROR: QEMU not found: {QEMU_PATH}{RESET}")
        return 1

    # Setup signal handler
    def signal_handler(sig, frame):
        global g_running
        g_running = False

    signal.signal(signal.SIGINT, signal_handler)

    try:
        # Start cluster
        print(f"{CYAN}Starting ESP32 QEMU Cluster...{RESET}")
        if not start_cluster(startup_timeout=args.timeout):
            print(f"{RED}Failed to start cluster{RESET}")
            # Print master log for debugging
            print(f"\n{BOLD}=== Startup Log ==={RESET}")
            with g_log_lock:
                for msg, color in g_master_log:
                    print(f"{color}{msg}{RESET}")
            stop_cluster()
            return 1

        # Print startup success
        print(f"\n{BOLD}=== Startup Log ==={RESET}")
        with g_log_lock:
            for msg, color in g_master_log:
                print(f"{color}{msg}{RESET}")

        if args.auto:
            result = auto_mode(worker_type=worker_type)
        else:
            interactive_mode()
            result = 0

    finally:
        g_running = False
        stop_cluster()
        print(f"\n{GREEN}Cluster shutdown complete.{RESET}")

    return result


if __name__ == "__main__":
    sys.exit(main())
