#!/usr/bin/env python3
"""
ESP32 Linux Compatibility Layer - Build & Simulation Tool

Comprehensive tooling for building firmware, exporting symbols, and running QEMU simulation.

Usage:
    python tools/build_and_run.py              # Full workflow: build -> export -> rebuild -> sim
    python tools/build_and_run.py --build      # Build only
    python tools/build_and_run.py --sim        # Run simulation only (assumes built)
    python tools/build_and_run.py --export     # Export symbols only
    python tools/build_and_run.py --clean      # Full clean before build
    python tools/build_and_run.py --timeout 30 # Custom QEMU timeout (default: 20s)
    python tools/build_and_run.py --verbose    # Verbose output
    python tools/build_and_run.py --no-net     # Run QEMU without networking
    python tools/build_and_run.py --c2         # C2 mode: port forward 9000 and run interactively
"""

import subprocess
import sys
import os
import shutil
import argparse

# ============================================================================
# Configuration
# ============================================================================

# QEMU executable path (Windows)
QEMU_PATH = r"C:\Users\Dhruv\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe"

# Python executable path (Windows)
PYTHON_PATH = r"C:\Users\Dhruv\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe"

# Build paths
PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_DIR = os.path.join(PROJECT_ROOT, "build")
ELF_PATH = os.path.join(BUILD_DIR, "linux_compat_layer.elf")
MERGED_FLASH = os.path.join(BUILD_DIR, "merged-flash.bin")
SYMBOL_OUTPUT = os.path.join(PROJECT_ROOT, "components", "espressif__elf_loader", "src", "esp_all_symbol.c")

# Flash size for QEMU
FLASH_SIZE = 4 * 1024 * 1024  # 4MB

# Default timeout for QEMU (seconds)
DEFAULT_TIMEOUT = 20

# Verbosity
VERBOSE = False


def log(msg, level="INFO"):
    """Print log message with level prefix."""
    print(f"[{level}] {msg}")


def debug(msg):
    """Print debug message if verbose mode enabled."""
    if VERBOSE:
        print(f"[DEBUG] {msg}")


def error(msg):
    """Print error and exit."""
    print(f"[ERROR] {msg}", file=sys.stderr)
    sys.exit(1)


def run_command(command, cwd=None, shell=True, capture=False):
    """
    Run a shell command with error handling.

    Args:
        command: Command string or list
        cwd: Working directory
        shell: Use shell execution
        capture: Capture and return output instead of printing

    Returns:
        Output string if capture=True, else None
    """
    debug(f"Running: {command}")

    try:
        if capture:
            result = subprocess.run(
                command,
                cwd=cwd,
                shell=shell,
                capture_output=True,
                text=True,
                check=True
            )
            return result.stdout
        else:
            subprocess.check_call(command, cwd=cwd, shell=shell)
            return None
    except subprocess.CalledProcessError as e:
        error(f"Command failed with exit code {e.returncode}: {command}")
    except FileNotFoundError as e:
        error(f"Command not found: {e}")


def check_prerequisites():
    """Verify required tools and paths exist."""
    log("Checking prerequisites...")

    # Check QEMU
    if not os.path.exists(QEMU_PATH):
        error(f"QEMU not found at: {QEMU_PATH}\n"
              f"Please install ESP-IDF QEMU or update QEMU_PATH in this script.")

    # Check project structure
    if not os.path.exists(os.path.join(PROJECT_ROOT, "main", "main.c")):
        error(f"Invalid project root: {PROJECT_ROOT}")

    # Check build tools
    build_bat = os.path.join(PROJECT_ROOT, "tools", "run_build.bat")
    if not os.path.exists(build_bat):
        error(f"Build script not found: {build_bat}")

    debug("Prerequisites OK")


def clean_build():
    """Perform full clean of build directory."""
    log("Performing full clean...")
    os.chdir(PROJECT_ROOT)
    run_command("tools\\run_build.bat fullclean")
    log("Clean complete")


def build_project():
    """Build the ESP-IDF project."""
    log("Building project...")
    os.chdir(PROJECT_ROOT)
    run_command("tools\\run_build.bat build")

    # Verify ELF was created
    if not os.path.exists(ELF_PATH):
        error(f"Build failed - ELF not found: {ELF_PATH}")

    log(f"Build complete: {ELF_PATH}")


def export_symbols():
    """Export symbols from firmware ELF to C source file."""
    log("Exporting symbols...")
    os.chdir(PROJECT_ROOT)

    if not os.path.exists(ELF_PATH):
        error(f"ELF file not found: {ELF_PATH}\n"
              f"Run --build first to generate the ELF.")

    export_script = os.path.join(PROJECT_ROOT, "tools", "export_symbols.py")
    if not os.path.exists(export_script):
        error(f"Symbol export script not found: {export_script}")

    run_command(f"\"{PYTHON_PATH}\" \"{export_script}\" \"{ELF_PATH}\" \"{SYMBOL_OUTPUT}\"")

    if not os.path.exists(SYMBOL_OUTPUT):
        error(f"Symbol export failed - output not created: {SYMBOL_OUTPUT}")

    log(f"Symbols exported to: {SYMBOL_OUTPUT}")


def merge_binaries():
    """Merge all binaries into single flash image."""
    log("Merging binaries...")
    os.chdir(PROJECT_ROOT)

    run_command("tools\\run_build.bat merge-bin")

    # ESP-IDF creates merged-binary.bin, we need merged-flash.bin
    merged_binary = os.path.join(BUILD_DIR, "merged-binary.bin")

    if os.path.exists(merged_binary):
        if os.path.exists(MERGED_FLASH):
            os.remove(MERGED_FLASH)
        shutil.move(merged_binary, MERGED_FLASH)
        debug(f"Renamed {merged_binary} -> {MERGED_FLASH}")
    elif not os.path.exists(MERGED_FLASH):
        error(f"Merge failed - no output file found")

    log(f"Merged binary: {MERGED_FLASH}")


def build_guest_app(app_name):
    """Build a guest ELF application."""
    log(f"Building guest app: {app_name}...")
    os.chdir(PROJECT_ROOT)

    if os.name == "nt":
        build_script = os.path.join(PROJECT_ROOT, "tools", "build_guest_app.bat")
        if not os.path.exists(build_script):
            error(f"Guest build script not found: {build_script}")

        run_command(f"tools\\build_guest_app.bat {app_name}")
        guest_elf = os.path.join(PROJECT_ROOT, "build", "guest_apps", f"{app_name}.elf")
    else:
        makefile = os.path.join(PROJECT_ROOT, "tools", "Makefile.guest")
        if not os.path.exists(makefile):
            error(f"Guest makefile not found: {makefile}")

        app_dir = os.path.join("apps", app_name)
        symbols = os.path.join("components", "espressif__elf_loader", "src", "esp_all_symbol.c")
        run_command(f"make -f tools/Makefile.guest APP={app_dir} SYMBOLS={symbols}")
        guest_elf = os.path.join(PROJECT_ROOT, app_dir, f"{app_name}.elf")

    if not os.path.exists(guest_elf):
        error(f"Guest app build failed - ELF not found: {guest_elf}")

    log(f"Guest app built: {guest_elf}")
    return guest_elf

def set_default_elf(app_name):
    """Update main.c to set default ELF path for the given app."""
    main_c = os.path.join(PROJECT_ROOT, "main", "main.c")
    elf_path = f"/linux/{app_name}.elf"

    log(f"Updating main.c to use default ELF: {elf_path}...")

    with open(main_c, "r") as f:
        content = f.read()

    # Replace DEFAULT_ELF_PATH definition
    import re
    pattern = r'#define DEFAULT_ELF_PATH\s+"[^"]*"'
    replacement = f'#define DEFAULT_ELF_PATH "{elf_path}"'

    # Check if pattern exists in file
    if not re.search(pattern, content):
        error(f"Could not find DEFAULT_ELF_PATH in main.c")

    new_content = re.sub(pattern, replacement, content)

    with open(main_c, "w") as f:
        f.write(new_content)

    log(f"Updated DEFAULT_ELF_PATH to: {elf_path}")


def pad_flash():
    """Pad flash binary to exact 4MB for QEMU."""
    log(f"Padding flash to {FLASH_SIZE // (1024*1024)}MB...")

    if not os.path.exists(MERGED_FLASH):
        error(f"Flash binary not found: {MERGED_FLASH}")

    current_size = os.path.getsize(MERGED_FLASH)
    debug(f"Current size: {current_size} bytes")

    if current_size > FLASH_SIZE:
        error(f"Flash binary too large: {current_size} > {FLASH_SIZE}")

    if current_size == FLASH_SIZE:
        log("Flash already correct size")
        return

    pad_size = FLASH_SIZE - current_size
    debug(f"Padding {pad_size} bytes")

    with open(MERGED_FLASH, "ab") as f:
        f.write(b'\x00' * pad_size)

    final_size = os.path.getsize(MERGED_FLASH)
    log(f"Padded to {final_size} bytes ({final_size // (1024*1024)}MB)")


def run_simulation(timeout=DEFAULT_TIMEOUT, networking=True, c2_mode=False, port_forward=None):
    """
    Run QEMU simulation with timeout.

    Args:
        timeout: Maximum seconds to run before force-killing
        networking: Enable OpenEth networking
        c2_mode: Interactive mode for C2 testing (no timeout capture)
        port_forward: Port to forward from host to guest (e.g., 9000 for C2)
    """
    mode_str = "C2 interactive" if c2_mode else "standard"
    log(f"Starting QEMU simulation ({mode_str}, timeout: {timeout}s, networking: {networking})...")

    if not os.path.exists(MERGED_FLASH):
        error(f"Flash binary not found: {MERGED_FLASH}\n"
              f"Run full build workflow first.")

    if not os.path.exists(QEMU_PATH):
        error(f"QEMU not found: {QEMU_PATH}")

    # Build QEMU command
    qemu_cmd = [
        QEMU_PATH,
        "-nographic",
        "-machine", "esp32",
        "-drive", f"file={MERGED_FLASH},if=mtd,format=raw",
        "-no-reboot"
    ]

    if networking:
        # Note: ESP32 QEMU (Espressif build) doesn't support -net dump for pcap capture.
        # Standard QEMU has: -net dump,file=capture.pcap
        # But Espressif's build only supports: -net [user|tap|bridge|socket]
        # For network traffic verification, use real hardware or Wireshark with tap networking.
        nic_opts = "user,model=open_eth"

        # Add port forwarding if specified
        if port_forward:
            # Handle both single int/str and list
            ports = port_forward if isinstance(port_forward, list) else [port_forward]
            for p in ports:
                nic_opts += f",hostfwd=tcp::{p}-:{p}"
                log(f"Port forwarding enabled: localhost:{p} -> guest:{p}")
        
        qemu_cmd.extend(["-nic", nic_opts])
    debug(f"QEMU command: {' '.join(qemu_cmd)}")

    # Change to project root for relative paths
    os.chdir(PROJECT_ROOT)

    if c2_mode:
        # C2 mode: Run interactively without timeout capture
        # User can press Ctrl+C to stop
        log("=" * 60)
        log("C2 INTERACTIVE MODE")
        log("=" * 60)
        log("QEMU is running. The C2 server is listening on localhost:9000")
        log("In another terminal, run:")
        log("  python tools/c2_master.py <payload.elf> localhost")
        log("")
        log("Press Ctrl+C to stop QEMU")
        log("=" * 60)

        try:
            # Run QEMU in foreground, pass through stdin/stdout
            proc = subprocess.Popen(qemu_cmd)
            proc.wait()
        except KeyboardInterrupt:
            log("\nStopping QEMU...")
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
            log("QEMU stopped")
        return

    # Standard mode: Real-time output with timeout
    # Let QEMU inherit stdout/stderr directly (no pipe = no buffering)
    import threading

    try:
        log("=" * 60)
        log("QEMU OUTPUT START")
        log("=" * 60)
        sys.stdout.flush()

        proc = subprocess.Popen(qemu_cmd)

        def kill_proc():
            log(f"\n[INFO] QEMU timeout ({timeout}s) - simulation complete")
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()

        timer = threading.Timer(timeout, kill_proc)
        timer.start()

        try:
            proc.wait()
        except KeyboardInterrupt:
            log("\nStopping QEMU...")
            proc.terminate()
            try:
                proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
        finally:
            timer.cancel()

        log("=" * 60)
        if proc.returncode == 0:
            log("QEMU exited normally")
        else:
            log("QEMU stopped")

    except Exception as e:
        error(f"QEMU execution failed: {e}")


def full_workflow(timeout=DEFAULT_TIMEOUT, networking=True, c2_mode=False, port_forward=None):
    """
    Complete build and simulation workflow:
    1. Build project (first pass)
    2. Export symbols
    3. Rebuild (with new symbols)
    4. Merge binaries
    5. Pad flash
    6. Run simulation
    """
    log("=" * 60)
    log("FULL BUILD & SIMULATION WORKFLOW")
    if port_forward:
        log(f"(Port forwarding: {port_forward})")
    elif c2_mode:
        log("(C2 Mode: Port 9000 forwarded)")
    log("=" * 60)

    check_prerequisites()

    # Step 1: Initial build
    log("\n[1/6] Initial build...")
    build_project()

    # Step 2: Export symbols
    log("\n[2/6] Export symbols...")
    export_symbols()

    # Step 3: Rebuild with symbols
    log("\n[3/6] Rebuild with new symbols...")
    build_project()

    # Step 4: Merge binaries
    log("\n[4/6] Merge binaries...")
    merge_binaries()

    # Step 5: Pad flash
    log("\n[5/6] Pad flash...")
    pad_flash()

    # Step 6: Run simulation
    log("\n[6/6] Run simulation...")
    # Default to 9000 for C2 mode if no ports specified
    if c2_mode and not port_forward:
        port_forward = 9000
    run_simulation(timeout=timeout, networking=networking, c2_mode=c2_mode, port_forward=port_forward)

    if not c2_mode:
        log("\n" + "=" * 60)
        log("WORKFLOW COMPLETE")
        log("=" * 60)


def main():
    global VERBOSE

    parser = argparse.ArgumentParser(
        description="ESP32 Linux Compatibility Layer - Build & Simulation Tool",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python tools/build_and_run.py              # Full workflow
  python tools/build_and_run.py --build      # Build only
  python tools/build_and_run.py --sim        # Run simulation only
  python tools/build_and_run.py --clean --build --sim  # Clean build + sim
  python tools/build_and_run.py --timeout 60 # Longer simulation time
  python tools/build_and_run.py --no-net     # Disable networking
  python tools/build_and_run.py --c2         # C2 mode with port 9000 forwarding

C2 Demo Testing:
  1. Build and run in C2 mode:
     python tools/build_and_run.py --c2

  2. In another terminal, build a payload and send it:
     tools\\build_guest_app.bat c2_payload
     python tools/c2_master.py build/guest_apps/c2_payload.elf localhost
"""
    )

    parser.add_argument("--build", "-b", action="store_true",
                       help="Build project only")
    parser.add_argument("--export", "-e", action="store_true",
                       help="Export symbols only (requires built ELF)")
    parser.add_argument("--merge", "-m", action="store_true",
                       help="Merge binaries only")
    parser.add_argument("--sim", "-s", action="store_true",
                       help="Run simulation only (requires merged flash)")
    parser.add_argument("--clean", "-c", action="store_true",
                       help="Full clean before build")
    parser.add_argument("--timeout", "-t", type=int, default=DEFAULT_TIMEOUT,
                       help=f"QEMU timeout in seconds (default: {DEFAULT_TIMEOUT})")
    parser.add_argument("--no-net", action="store_true",
                       help="Disable QEMU networking")
    parser.add_argument("--verbose", "-v", action="store_true",
                       help="Verbose output")
    parser.add_argument("--c2", action="store_true",
                       help="C2 mode: forward port 9000, run interactively")
    parser.add_argument("--demo2", action="store_true",
                       help="Demo 2: forward ports 80 & 9000, run interactively")
    parser.add_argument("--port", "-p", type=int, default=9000,
                       help="Port to forward in C2/sim mode (default: 9000)")
    parser.add_argument("--build-guest", type=str, action='append',
                       help="Build guest app with given name (e.g., c2_payload)")
    parser.add_argument("--set-elf", type=str, default=None,
                       help="Set default ELF in main.c (e.g., c2_payload)")
    parser.add_argument("--guest-only", action="store_true",
                       help="Only build guest apps, do not run full workflow or simulation.")

    args = parser.parse_args()
    VERBOSE = args.verbose

    # If no specific action, run full workflow
    specific_action_explicitly_do_something_else = args.build or args.export or args.merge or args.sim

    # Determine ports to forward
    ports = args.port
    if args.demo2:
        ports = [80, 9000] # HTTP server and C2 server
        args.c2 = True  # Demo 2 implies interactive mode

        # Demo 2 requires collision_server (the HTTP server) and collision_guard (the computational ELF)
        if args.build_guest is None:
            args.build_guest = []
        if "collision_server" not in args.build_guest:
            args.build_guest.append("collision_server")
        if "collision_guard" not in args.build_guest:
            args.build_guest.append("collision_guard")
            
        # Set collision_server as the default ELF to run
        if args.set_elf is None:
            args.set_elf = "collision_server"

    os.chdir(PROJECT_ROOT)

    try:
        # Handle guest app building
        if args.build_guest:
            for app in args.build_guest:
                elf_path = build_guest_app(app)
                # Copy to data directory so it gets packed into LittleFS
                data_dir = os.path.join(PROJECT_ROOT, "data")
                if not os.path.exists(data_dir):
                    os.makedirs(data_dir)
                dest_path = os.path.join(data_dir, f"{app}.elf")
                shutil.copy2(elf_path, dest_path)
                log(f"Copied {app}.elf to data directory")

        # Handle ELF path setting
        if args.set_elf:
            set_default_elf(args.set_elf)

        if args.clean:
            clean_build()
        
        # If --guest-only is set and no other specific action requested, we exit here.
        if args.guest_only and not specific_action_explicitly_do_something_else:
            log("Guest apps built. Skipping full workflow as --guest-only was specified.")
            return

        # If any specific action (build, export, merge, sim) is requested, execute them.
        if specific_action_explicitly_do_something_else:
            # Run specific actions
            if args.build:
                check_prerequisites()
                build_project()
            if args.export:
                export_symbols()
            if args.merge:
                merge_binaries()
                pad_flash()
            if args.sim:
                run_simulation(timeout=args.timeout, networking=not args.no_net,
                             c2_mode=args.c2, port_forward=ports)
        else:
            # If no specific action (including --guest-only) was requested, run full workflow.
            full_workflow(timeout=args.timeout, networking=not args.no_net, c2_mode=args.c2, port_forward=ports)

    except KeyboardInterrupt:
        log("\nInterrupted by user")
        sys.exit(130)
    except Exception as e:
        error(f"Unexpected error: {e}")


if __name__ == "__main__":
    main()
