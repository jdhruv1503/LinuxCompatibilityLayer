# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a **Thin Linux Compatibility Layer (TLCL)** for ESP32 - a Unikernel/Library OS that enables execution of standard Linux ELF binaries on ESP32 microcontrollers without an MMU. The system translates POSIX system calls to ESP-IDF/FreeRTOS primitives and uses LittleFS for filesystem storage.

## Development Environment (Windows)

### Required Tools

| Component | Path | Notes |
|-----------|------|-------|
| ESP-IDF | `C:\Users\Dhruv\.esp-tools\esp-idf` | v5.4 |
| Python | `C:\Users\Dhruv\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe` | IDF venv Python |
| QEMU | `C:\Users\Dhruv\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe` | **Always use full path** |
| Xtensa GCC | `C:\Users\Dhruv\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin\` | Cross-compiler |

### Critical: MSYSTEM Environment Issue

**ESP-IDF refuses to run in Git Bash/MSYS2 environments.** Always use `tools/run_build.bat` which clears the `MSYSTEM` variable automatically.

## Primary Tooling: build_and_run.py

**Use `tools/build_and_run.py` for all build and simulation tasks.** This Python script handles the complete workflow with proper error handling and timeouts.

### Quick Reference

```bash
# Full workflow: build -> export symbols -> rebuild -> merge -> pad -> simulate
python tools/build_and_run.py

# Build only (no simulation)
python tools/build_and_run.py --build

# Run simulation only (requires previous build)
python tools/build_and_run.py --sim

# Clean build + simulation
python tools/build_and_run.py --clean

# Custom QEMU timeout (default: 20 seconds)
python tools/build_and_run.py --timeout 60

# Verbose output for debugging
python tools/build_and_run.py --verbose

# Disable networking (no OpenEth)
python tools/build_and_run.py --no-net

# Socket capture for testing dup2 redirection
python tools/build_and_run.py --capture 12345 --timeout 30

# Individual steps
python tools/build_and_run.py --export   # Export symbols only
python tools/build_and_run.py --merge    # Merge + pad binaries only

# Guest application and C2 demos
python tools/build_and_run.py --build-guest c2_payload              # Build guest ELF only
python tools/build_and_run.py --set-elf c2_payload                  # Set c2_payload as default ELF
python tools/build_and_run.py --build-guest c2_payload --set-elf c2_payload  # Combined
python tools/build_and_run.py --set-elf hello_world                 # Switch default ELF to hello_world
```

### Why This Script is Required

1. **QEMU Never Exits Automatically**: QEMU with `-no-reboot` will NOT exit if firmware idles. The script enforces a timeout to prevent agent lockups.
2. **Two-Pass Build**: Symbol export requires a built ELF, then rebuild links against new symbols.
3. **Flash Padding**: QEMU requires exactly 4MB flash image, padded with zeros.
4. **Proper Error Handling**: Script exits with error code on any failure.

## Alternative: Manual Commands

If you must run commands manually, use these exact commands:

### Build

```batch
tools\run_build.bat build
tools\run_build.bat fullclean   # Full clean
tools\run_build.bat menuconfig  # Configure
```

### Export Symbols

```bash
python tools/export_symbols.py build/linux_compat_layer.elf components/espressif__elf_loader/src/esp_all_symbol.c
```

### Merge & Pad Flash

```batch
tools\run_build.bat merge-bin
move build\merged-binary.bin build\merged-flash.bin
```

Then pad to 4MB (Python one-liner):
```python
import os; f=open('build/merged-flash.bin','ab'); f.write(b'\x00'*(4*1024*1024-os.path.getsize('build/merged-flash.bin'))); f.close()
```

### Run QEMU (ALWAYS with timeout)

**CRITICAL: Never run QEMU without a timeout wrapper!**

```bash
# Using Python subprocess timeout (RECOMMENDED)
python -c "import subprocess; subprocess.run([r'C:\Users\Dhruv\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe', '-nographic', '-machine', 'esp32', '-drive', 'file=build/merged-flash.bin,if=mtd,format=raw', '-nic', 'user,model=open_eth', '-no-reboot'], timeout=20)"
```

The full QEMU command (DO NOT run without timeout wrapper):
```
C:\Users\Dhruv\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe -nographic -machine esp32 -drive file=build/merged-flash.bin,if=mtd,format=raw -nic user,model=open_eth -no-reboot
```

### Multi-Instance QEMU (for distributed demos)

Run multiple QEMU instances with different port forwarding:
```bash
# Node 1 on port 9001
"...qemu-system-xtensa.exe" -nographic -machine esp32 -drive file=build/merged-flash.bin,if=mtd,format=raw -no-reboot -nic user,model=open_eth,hostfwd=tcp::9001-:9000

# Node 2 on port 9002
"...qemu-system-xtensa.exe" -nographic -machine esp32 -drive file=build/merged-flash.bin,if=mtd,format=raw -no-reboot -nic user,model=open_eth,hostfwd=tcp::9002-:9000
```

Port forwarding syntax: `hostfwd=tcp::<HOST_PORT>-:<GUEST_PORT>`

## Guest ELF Compilation

**IMPORTANT:** Guest ELFs must be compiled as shared objects (ET_DYN), NOT relocatable objects (ET_REL).

### Using build_and_run.py (Recommended)

```bash
# Build a guest ELF application
python tools/build_and_run.py --build-guest hello_world

# Set it as the default ELF to load on startup
python tools/build_and_run.py --set-elf hello_world

# Combined: build, set as default, then full build + simulation
python tools/build_and_run.py --build-guest c2_payload --set-elf c2_payload

# Build the C2 bot server
python tools/build_and_run.py --build-guest c2_bot --set-elf c2_bot
```

### Manual Method

```batch
# Direct build with build_guest_app.bat
tools\build_guest_app.bat hello_world
```

**Entry Point Requirements:**
```c
// Entry point MUST have visibility attribute
__attribute__((visibility("default")))
int app_main(int argc, char *argv[])
{
    return 0;
}
```

**Output Location:**
```
build/guest_apps/c2_payload.elf
```

All guest ELFs are compiled to `build/guest_apps/` directory.

## Architecture

### Memory Model (No-MMU Constraint)
- **Single Address Space**: FreeRTOS + loaded ELFs share physical RAM
- **IRAM vs DRAM**: Code uses `heap_caps_malloc(..., MALLOC_CAP_EXEC)`; data uses `MALLOC_CAP_8BIT`
- Binaries must be Position Independent (PIE)

### Core Components

1. **ELF Loader** (`main/main.c` - `load_and_run_elf()`)
   - Uses `esp_elf_relocate()` to load ELF from buffer
   - Calls `elf.entry(argc, argv)` to execute

2. **Symbol Table** (`components/espressif__elf_loader/src/esp_all_symbol.c`)
   - Generated by `tools/export_symbols.py`
   - Maps POSIX function names to firmware addresses
   - **Critical**: Customer symbols checked first to allow overriding libc functions

3. **Syscall Shim Layer** (`main/syscalls/`)
   - `shim_unistd.c`: FS syscalls with path translation (`/path` -> `/linux/path`)
   - `shim_socket.c`: Network syscalls with LwIP errno translation
   - `shim_process.c`: Process emulation (spawn model for execve)

4. **Driver Subsystem** (`main/drivers/`)
   - `drivers.h`: Unified driver interface
   - `drv_network.c`: Network driver (OpenEth for QEMU, EMAC for hardware)
   - `drv_fs_littlefs.c`: LittleFS filesystem driver
   - `drv_fs_sdcard.c`: SD card FATFS driver (real hardware only)
   - `drv_devices.c`: VFS device drivers (/dev/c2, /dev/collision, /dev/buzzer)

5. **VFS Device Drivers** (`main/vfs_drivers/`, `main/drivers/`)
   - `/dev/c2`: C2 pipe for stdout redirection to network socket
   - `/dev/collision`: Virtual distance sensor (demo driver)
   - `/dev/buzzer`: PWM buzzer control (demo driver)

6. **C2 Command & Control Server** (`apps/c2_bot/`)
   - `main.c`: Guest ELF application that listens on TCP port 9000 for payloads
   - Entry point: `app_main()` for POSIX compatibility
   - Uses shared state pointer for stdout/stderr/stdin redirection to network socket
   - Can be loaded as default ELF via `build_and_run.py --set-elf c2_bot`
   - Spawns pthreads for multi-client support

### Key Design Patterns

- **Path Translation**: Guest `/log.txt` -> VFS `/linux/log.txt`
- **Permission Faking**: stat() always returns 0777 (LittleFS lacks permissions)
- **Spawn Model**: execve() creates new FreeRTOS task (no fork)
- **Errno Translation**: LwIP errors mapped to POSIX errno

## Partition Layout

```csv
# partitions.csv (4MB flash)
nvs,        data, nvs,     0x9000,  0x4000,
otadata,    data, ota,     0xd000,  0x2000,
phy_init,   data, phy,     0xf000,  0x1000,
factory,    app,  factory, 0x10000, 1536K,
linux_fs,   data, 0x83,    ,        1536K,
```

## sdkconfig.defaults Requirements

The following MUST be in `sdkconfig.defaults`:

```ini
# ELF Loader
CONFIG_ELF_LOADER_CUSTOMER_SYMBOLS=y

# Flash size - 4MB for QEMU
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y

# Custom partition table
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"

# Ethernet for QEMU (OpenEth)
CONFIG_ETH_ENABLED=y
CONFIG_ETH_USE_OPENETH=y
CONFIG_ETH_USE_ESP32_EMAC=n
```

## Lessons Learned / Critical Issues

### QEMU Issues (CRITICAL)

1. **QEMU NEVER EXITS ON ITS OWN**: Even with `-no-reboot`, QEMU continues running if firmware idles. **ALWAYS use `tools/build_and_run.py` with timeout** or wrap in subprocess with timeout.

2. **Flash Size Must Be Exactly 4MB**: Use pad script or `build_and_run.py` which handles this.

3. **OpenEth for Networking**: Use `-nic user,model=open_eth` for QEMU networking. This creates NAT with DHCP.

4. **External Connections Fail in QEMU NAT**: QEMU user-mode networking doesn't route to external IPs. To test TCP, use port forwarding to localhost: `-nic user,model=open_eth,hostfwd=tcp::8080-:80`

5. **Multi-Instance Startup Time**: When running 4 QEMU instances, use at least 40 second timeout for node readiness. Instances need time to boot and initialize networking.

6. **QEMU NAT Idle Connection Timeouts**: QEMU's NAT stack may close connections that appear idle. **DO NOT use application-level chunked sending with delays** - this makes connections appear idle and triggers timeouts:
   ```python
   # BAD: Delays make connection appear idle -> timeout
   for chunk in chunks:
       socket.sendall(chunk)
       time.sleep(0.01)  # NAT sees this as idle!

   # GOOD: Let TCP handle segmentation
   socket.sendall(all_data)  # Single call, TCP handles chunking
   ```

7. **Stagger Parallel Operations**: When connecting to multiple QEMU instances, stagger operations to avoid overwhelming NAT:
   ```python
   for i, node in enumerate(nodes):
       time.sleep(i * 0.4)  # 400ms stagger
       connect_to_node(node)
   ```

### ESP-IDF Build Issues

1. **Partition Table Not Found**: If build fails with "Failed to find partition 'linux_fs'", ensure `CONFIG_PARTITION_TABLE_CUSTOM=y` is in `sdkconfig.defaults`, then delete `sdkconfig` and rebuild.

2. **Flash Size Overflow**: If partition table exceeds flash size, add `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y` to `sdkconfig.defaults`.

3. **MSYSTEM Error**: ESP-IDF refuses to run in Git Bash. Use `tools/run_build.bat`.

### ELF Loader Issues

1. **Wrong API**: Use `esp_elf_relocate()`, NOT `esp_elf_load()`. The latter doesn't exist.

2. **Symbol Export Required**: Run `export_symbols.py` after first build, then rebuild.

3. **Linker Stripping**: Add `-u symbol_name` to CMakeLists.txt for all shim functions.

### Network Issues

1. **LwIP Errno Translation**: LwIP uses different errno values. Shim must translate.

2. **fork() Not Supported**: ESP32 has no MMU. Use spawn model (execve creates new task).

3. **QEMU guestfwd Limitations**: Guest-to-host connections in QEMU user-mode networking are unreliable. `guestfwd` doesn't work as expected for forwarding guest connections to host services. For reliable testing, use real hardware.

4. **No PCAP Capture in ESP32 QEMU**: Espressif's QEMU build doesn't include `-net dump` for packet capture. Standard QEMU's syntax `-net dump,file=capture.pcap` returns "Parameter 'type' does not accept value 'dump'". ESP32 QEMU only supports `-net [user|tap|bridge|socket]`. For network traffic analysis, use real hardware with Wireshark or tap networking.

5. **Debug Logging Levels**: Use `ESP_LOG_WARN` for production to reduce noise. Use `ESP_LOG_DEBUG` only when debugging specific issues. The firmware sets log levels in `main.c` via `esp_log_level_set()`.

### Stdout/Stdin Redirection (dup2)

1. **DO NOT close STDOUT_FILENO**: ESP32's stdout and stderr share the same UART driver. Closing FD 1 breaks console output entirely. Use the "global socket variable" approach instead.

2. **Global Socket Variable Approach**: For stdout/stdin redirection:
   - Don't modify the FD table
   - Store target socket in a global variable (`g_c2_redirect_state`)
   - Set flags indicating which streams are "redirected"
   - In `shim_write()` / `shim_read()`, check flags and route to socket

3. **Non-blocking sends**: Always use `MSG_DONTWAIT` for socket sends in the redirect path to prevent blocking.

4. **Error handling in shim_write**: If socket send fails, still return success. This prevents app crashes when C2 connection drops.

5. **Declaration Order in C**: The `c2_redirect_state_t` struct and `g_c2_redirect_state` pointer MUST be declared BEFORE any functions that use them (like `shim_read`). Otherwise you get "undeclared identifier" errors.

6. **Use raw read() instead of fgets() for socket stdin**: `fgets()` uses internal FILE* buffering that may not work correctly with socket-redirected stdin. Use raw `read()` on fd=0 which goes through `shim_read()`.

### Python CLI / Terminal UI Issues (Windows)

1. **Unicode Encoding Errors**: Windows cmd.exe uses cp1252 encoding which cannot display Unicode symbols (✓, ✗, →). Always use ASCII alternatives:
   ```python
   CHECK = "[OK]"   # Not "✓"
   CROSS = "[X]"    # Not "✗"
   ARROW = "->"     # Not "→"
   ```

2. **Box Drawing**: Use ASCII characters for boxes:
   ```python
   # ASCII (cross-platform)
   "+---+"
   "|   |"
   "+---+"

   # NOT Unicode box-drawing
   "╔═══╗"
   "║   ║"
   "╚═══╝"
   ```

3. **ANSI Colors**: ANSI escape codes work on Windows 10+ terminals but may need to be enabled programmatically.

4. **UI Flicker Reduction**: For live terminal UIs, avoid `clear_screen()` which causes visible flicker. Instead:
   ```python
   # Move cursor home and clear each line as you write
   sys.stdout.write("\033[H")  # Move cursor to (1,1)
   for line in output:
       sys.stdout.write(line + "\033[K\n")  # \033[K clears to end of line
   sys.stdout.flush()
   ```

5. **Strip ANSI from QEMU Output**: QEMU output often contains ANSI escape codes (cursor movement, etc.) that corrupt display. Use comprehensive pattern:
   ```python
   import re
   ANSI_PATTERN = re.compile(
       r'\x1b\[[0-9;]*[a-zA-Z]'      # CSI sequences (ESC[0m, ESC[B, etc.)
       r'|\x1b\][^\x07]*\x07'         # OSC sequences
       r'|\x1b[PX^_][^\x1b]*\x1b\\'   # DCS, SOS, PM, APC
       r'|\x1b[NOc].'                 # SS2, SS3, reset
       r'|\x1b.'                      # Any other ESC + char
       r'|\x08'                       # Backspace
   )
   clean_text = ANSI_PATTERN.sub('', raw_text)
   # Also fix broken escapes like [B[
   clean_text = re.sub(r'\[([A-Z])\[', '[', clean_text)
   ```

### C Code Issues

1. **Struct Declaration Order**: In C, structs and global variables MUST be declared before any functions that reference them. This is especially important in `shim_unistd.c` where `c2_redirect_state_t` must appear before `shim_read()`.

2. **Visibility Attributes**: Guest ELF entry points must use `__attribute__((visibility("default")))` to be visible to the ELF loader.

## Build Artifacts

After successful build:
- `build/linux_compat_layer.elf` - Firmware ELF (used for symbol export)
- `build/linux_compat_layer.bin` - Firmware binary
- `build/linux_fs.bin` - LittleFS image
- `build/merged-flash.bin` - Combined 4MB image for QEMU
- `build/guest_apps/*.elf` - Compiled guest ELF applications
- `data/*.elf` - Guest ELFs copied for inclusion in flash

## Tools Reference

| Tool | Purpose |
|------|---------|
| `tools/build_and_run.py` | **Primary tool** - Full build & simulation workflow with guest app support |
| `tools/run_build.bat` | Wrapper for `idf.py` (handles MSYSTEM) |
| `tools/export_symbols.py` | Generate symbol table from ELF |
| `tools/build_guest_app.bat` | Compile guest ELF applications (manual method) |
| `tools/c2_master.py` | Distributed map-reduce demo controller (multi-node QEMU) |

### build_and_run.py Options Reference

```
--build              Build only (no simulation)
--sim                Run simulation only
--clean              Clean build + simulation
--export             Export symbols only
--merge              Merge + pad binaries only
--build-guest APP    Build guest ELF application
--set-elf APP        Set APP as default ELF in main.c
--timeout N          QEMU timeout in seconds (default: 20)
--verbose            Verbose output for debugging
--no-net             Disable networking (no OpenEth)
--capture PORT       Enable socket capture for dup2 testing
```

### C2 Workflow Example

The C2 server is now a guest ELF application that can be loaded like any other:

```bash
# 1. Build and start the C2 server bot
python tools/build_and_run.py --build-guest c2_bot --set-elf c2_bot

# 2. In another terminal, build a payload and send it
python tools/build_and_run.py --build-guest c2_payload
python tools/c2_master.py build/guest_apps/c2_payload.elf localhost

# 3. (Optional) Switch to running a different app as default
python tools/build_and_run.py --set-elf hello_world
```

**Key Advantages:**
- C2 server is now a guest app, not built into firmware
- Can be updated/modified without rebuilding firmware
- Supports clean separation of concerns (bot vs payload)
- All guest apps can spawn subprocesses via execve() if needed

### Distributed Map-Reduce Demo

Run a 4-node cluster demo with distributed computation:

```bash
# Build the map-reduce worker first
python tools/build_and_run.py --build-guest map_reduce_worker --build

# Run the demo in automated test mode (40s timeout recommended)
python tools/c2_master.py --auto --timeout 40

# Or run in interactive mode with split-screen UI
python tools/c2_master.py

# Press 'r' to run map-reduce, 'c' to clear, 'q' to quit
```

**Demo Features:**
- Spawns 4 QEMU instances with port forwarding (9001-9004)
- Distributes sum(1..1000) calculation across nodes
- Aggregates results and verifies correctness (expected: 500500)
- Interactive terminal UI shows real-time node output
- Live UI updates during job execution showing [CONN], [SEND], [DATA], [WAIT], [DONE] status

**c2_master.py Options:**
```
--auto               Run automated test and exit
--math               Use math worker (with trig functions) instead of simple worker
--timeout N          Node startup timeout in seconds (default: 40)
--nodes N            Number of nodes to use: 1, 2, 3, or 4 (default: 4)
```

**Interactive Mode Keys:**
- `r` - Run simple map-reduce job
- `m` - Run math worker job
- `c` - Clear logs
- `q` - Quit

## Documentation

- `documentation/tasks/` - Implementation guides for each task
- `documentation/task_outputs/` - Completed task reports
- `documentation/data_dump/` - API references, research notes
  - `QEMU_MultiInstance_Reference.md` - Multi-QEMU setup guide
  - `QEMU_NAT_Networking_Gotchas.md` - NAT timeout issues and solutions
  - `ESP32_Stdin_Redirection_Guide.md` - stdin/stdout/stderr redirection
  - `Windows_Terminal_UI_Reference.md` - Cross-platform terminal UI guide
