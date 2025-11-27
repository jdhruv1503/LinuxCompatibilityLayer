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

# Global state
g_running = True
g_node_processes = {}  # node_id -> subprocess.Popen
g_node_threads = {}    # node_id -> thread
g_node_logs = {1: [], 2: [], 3: [], 4: []}  # Rolling log buffers
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
    # Filter out noisy debug messages
    if any(skip in msg for skip in ['memory_layout', 'heap_init', 'cpu_start',
                                      'esp_netif', 'event:', 'intr_alloc',
                                      'efuse:', 'spi_flash', 'esp_eth']):
        return

    with g_log_lock:
        g_node_logs[node_id].append(msg)
        if len(g_node_logs[node_id]) > MAX_LOG_LINES:
            g_node_logs[node_id].pop(0)


def clear_screen():
    """Clear the terminal screen."""
    os.system('cls' if os.name == 'nt' else 'clear')


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


def draw_ui():
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
        for msg, color in g_master_log[-8:]:
            output.append(f"{color}{msg}{RESET}")

    # Padding
    while len(output) < height - 3:
        output.append("")

    # Command bar at bottom
    output.append(f"{DIM}{'─' * width}{RESET}")
    output.append(f"{BOLD}Commands:{RESET} [r] Run Map-Reduce  [c] Clear Logs  [q] Quit")

    # Print all at once
    clear_screen()
    print("\n".join(output))


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


def wait_for_node_ready(port, timeout=30):
    """Wait until a node's C2 server is accepting connections."""
    start = time.time()
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


def send_worker_job(node_id, port, data_chunk):
    """
    Send a map-reduce job to a node.

    Protocol:
    1. Connect to C2 bot on port
    2. Send ELF size (4 bytes, little-endian)
    3. Send ELF binary
    4. Wait for worker to start, then send data
    5. Close write end to signal EOF
    6. Read result
    """
    try:
        log_node(node_id, "[CONN] Connecting...")

        # Read worker ELF
        with open(WORKER_ELF, 'rb') as f:
            elf_data = f.read()

        # Connect to node
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(30)
        s.connect(('localhost', port))
        log_node(node_id, "[CONN] Connected!")

        # Send ELF size header
        log_node(node_id, f"[SEND] ELF ({len(elf_data)} bytes)...")
        s.sendall(struct.pack('<I', len(elf_data)))

        # Send ELF data
        s.sendall(elf_data)
        log_node(node_id, "[SEND] ELF sent, waiting...")

        # Wait for worker to start and be ready for data
        time.sleep(1.5)

        # Send the data (numbers as newline-separated string)
        log_node(node_id, f"[DATA] Sending {len(data_chunk)} numbers...")
        data_str = "\n".join(str(x) for x in data_chunk) + "\n"
        s.sendall(data_str.encode('utf-8'))
        log_node(node_id, "[DATA] Data sent!")

        # Signal EOF by closing write side
        s.shutdown(socket.SHUT_WR)
        log_node(node_id, "[WAIT] Computing...")

        # Read response
        response = b""
        while True:
            chunk = s.recv(4096)
            if not chunk:
                break
            response += chunk

        s.close()

        # Parse result
        text = response.decode('utf-8', errors='replace')

        # Look for "RESULT: SUM=X COUNT=Y" pattern
        import re
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
        return 0, 0


def run_mapreduce_demo(live_ui=True):
    """Run a distributed map-reduce computation across all nodes."""
    log_master("=" * 40, CYAN)
    log_master("Starting Map-Reduce Job", CYAN)
    log_master("=" * 40, CYAN)

    # Generate test data: sum of 1 to 1000
    log_master("Generating dataset: integers 1-1000")
    data = list(range(1, 1001))
    random.shuffle(data)

    # Split into 4 chunks
    chunk_size = len(data) // 4
    chunks = [
        data[0:chunk_size],
        data[chunk_size:2*chunk_size],
        data[2*chunk_size:3*chunk_size],
        data[3*chunk_size:]
    ]

    log_master(f"Split into {len(chunks)} chunks of ~{chunk_size} items")

    # Send jobs to all nodes in parallel
    results = [None] * 4
    job_done = [False] * 4
    threads = []

    def job_thread(idx, node, chunk):
        node_id = node['id']
        log_node(node_id, f"[START] Job: {len(chunk)} items")
        result = send_worker_job(node_id, node['port'], chunk)
        results[idx] = result
        job_done[idx] = True
        if result[0] > 0:
            log_node(node_id, f"[DONE] sum={result[0]}")
        else:
            log_node(node_id, f"[FAIL] Job failed!")

    for i, node in enumerate(NODES):
        t = threading.Thread(target=job_thread, args=(i, node, chunks[i]))
        t.start()
        threads.append(t)

    # Live UI update loop - redraw while jobs are running
    if live_ui:
        while not all(job_done):
            draw_ui()
            time.sleep(0.3)  # Update 3 times per second
        # Final draw after completion
        draw_ui()

    # Wait for all jobs to complete
    for t in threads:
        t.join(timeout=60)

    # Aggregate results
    total_sum = 0
    total_count = 0
    for i, (s, c) in enumerate(results):
        if s is not None:
            total_sum += s
            total_count += c

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
    """Start all 4 QEMU instances."""
    log_master("Starting 4-node cluster...", CYAN)

    threads = []
    for node in NODES:
        runner = NodeRunner(node)
        runner.start()
        threads.append(runner)
        g_node_threads[node['id']] = runner
        time.sleep(0.5)  # Stagger startup slightly

    # Wait for all nodes to be ready
    log_master("Waiting for nodes to initialize...", YELLOW)

    ready_count = 0
    for node in NODES:
        log_master(f"  Checking Node {node['id']}...", DIM)
        if wait_for_node_ready(node['port'], timeout=startup_timeout):
            log_master(f"  {CHECK} Node {node['id']} ready on port {node['port']}", GREEN)
            ready_count += 1
        else:
            log_master(f"  {CROSS} Node {node['id']} failed to start", RED)

    if ready_count == 4:
        log_master("All 4 nodes ready!", GREEN)
        return True
    else:
        log_master(f"Only {ready_count}/4 nodes ready", YELLOW)
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
    log_master("Press 'r' to run map-reduce, 'q' to quit", WHITE)

    last_draw = 0
    draw_interval = 0.5  # Redraw every 500ms

    try:
        while g_running:
            # Redraw UI periodically
            now = time.time()
            if now - last_draw > draw_interval:
                draw_ui()
                last_draw = now

            # Check for keyboard input (Windows)
            if HAS_MSVCRT and msvcrt.kbhit():
                key = msvcrt.getch().decode('utf-8', errors='ignore').lower()

                if key == 'q':
                    g_running = False
                    break
                elif key == 'r':
                    log_master("Starting map-reduce job...", CYAN)
                    run_mapreduce_demo(live_ui=True)  # Live UI updates during job
                    log_master("Press 'r' to run again, 'q' to quit", WHITE)
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


def auto_mode():
    """Run automated test and exit with live UI."""
    log_master("Auto mode: Running single test...", CYAN)

    # Wait a bit for nodes to fully stabilize
    log_master("Waiting 3 seconds for nodes to stabilize...", YELLOW)
    time.sleep(3)

    log_master("Executing map-reduce job...", CYAN)

    # Run with live UI so you can see what's happening
    success = run_mapreduce_demo(live_ui=True)

    # Final UI update with results
    draw_ui()
    time.sleep(1)

    # Print final summary
    print("\n" + "=" * 50)
    print("MAP-REDUCE TEST RESULT")
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
  python c2_master.py --auto       # Automated test
  python c2_master.py --timeout 30 # Custom startup timeout
"""
    )
    parser.add_argument("--auto", action="store_true",
                       help="Run automated test and exit")
    parser.add_argument("--timeout", type=int, default=20,
                       help="Node startup timeout in seconds (default: 20)")

    args = parser.parse_args()

    # Verify prerequisites
    if not os.path.exists(MERGED_FLASH):
        print(f"{RED}ERROR: Flash image not found: {MERGED_FLASH}{RESET}")
        print("Run: python tools/build_and_run.py --build")
        return 1

    if not os.path.exists(WORKER_ELF):
        print(f"{RED}ERROR: Worker ELF not found: {WORKER_ELF}{RESET}")
        print("Run: python tools/build_and_run.py --build-guest map_reduce_worker")
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
            result = auto_mode()
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
