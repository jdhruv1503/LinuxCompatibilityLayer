# Task 07 Refactoring Summary - C2 Server as Guest App

## Overview
Refactored the Command & Control (C2) server from a firmware-integrated component (FreeRTOS task) into a standalone guest ELF application, improving separation of concerns and enabling the C2 bot to be updated independently of the firmware.

## Changes Made

### 1. Architecture Change: Firmware-Based → Guest App-Based

**Before:** C2 server was a FreeRTOS task created during firmware initialization
- Located in `main/c2_server.c` and `main/c2_server.h`
- Called via `c2_server_start()` in `main/main.c`
- Compiled directly into firmware binary

**After:** C2 server is now a guest ELF application
- Located in `apps/c2_bot/main.c`
- Implements `app_main()` entry point (standard for all guest apps)
- Compiled separately as Position Independent Executable (ET_DYN)
- Can be loaded via `build_and_run.py --set-elf c2_bot`

### 2. API Conversion: FreeRTOS → POSIX

**Key API Changes:**
- `xTaskCreate()` → `pthread_create()` for multi-client support
- `vTaskDelete()` → `pthread_exit()`
- FreeRTOS logging (`ESP_LOGI/LOGE`) → Standard C `printf`/`fprintf`
- `vTaskDelay()` removed - synchronous processing only
- Maintained all socket and file I/O APIs (POSIX-compatible)

### 3. Files Modified

| File | Changes |
|------|---------|
| `main/main.c` | Removed c2_server.h include, ENABLE_C2_SERVER #if/#else, c2_server_start() call |
| `main/CMakeLists.txt` | Removed `../apps/c2_bot/c2_server.c` from SRCS and removed include_dir |
| `apps/c2_bot/main.c` | **NEW** - Refactored C2 server as guest app with POSIX APIs |
| `apps/c2_bot/CMakeLists.txt` | **NEW** - Build config for c2_bot guest app |
| `CLAUDE.md` | Updated architecture docs, tooling examples, workflow |
| `tools/build_and_run.py` | Already supported --build-guest and --set-elf (no changes needed) |

### 4. Deleted Files

- `main/c2_server.c` - Moved to `apps/c2_bot/` and refactored
- `main/c2_server.h` - Legacy header, functionality superseded by app_main()

## Usage

### Building and Running C2 Bot

```bash
# Build C2 bot guest app
python tools/build_and_run.py --build-guest c2_bot --set-elf c2_bot

# In another terminal, build payload and send
python tools/build_and_run.py --build-guest c2_payload
python tools/c2_master.py build/guest_apps/c2_payload.elf localhost
```

### Output Redirection

The C2 bot still uses the shared state pointer mechanism for stdout/stderr redirection:
- Exported from `shim_unistd.c` as `g_c2_redirect_state` (struct pointer)
- Works identically to firmware-based version
- Allows bidirectional communication: payload output → socket

## Design Decisions

### 1. Why POSIX APIs Instead of FreeRTOS?
- Guest apps should only use standard POSIX/C library APIs
- Avoids firmware dependencies in guest code
- Ensures apps can run on any POSIX system (not just ESP32)
- Clean architectural boundary

### 2. Multi-Client Support via pthreads
- Optional: falls back to single-threaded if pthread unavailable
- Each client handled in separate thread
- Prevents one slow client from blocking others
- Non-blocking socket sends prevent app crashes

### 3. Stdout Redirection Mechanism Unchanged
- Uses same shared state pointer from `shim_unistd.c`
- Works because both firmware and guest apps share address space
- No changes needed to shim layer

## Testing

### Quick Verification
```bash
# 1. Start QEMU with C2 bot
python tools/build_and_run.py --build-guest c2_bot --set-elf c2_bot

# 2. In another terminal (QEMU running)
python tools/build_and_run.py --build-guest c2_payload
python tools/c2_master.py build/guest_apps/c2_payload.elf localhost
```

**Expected Output:**
- C2 bot prints: "C2 Server listening on port 9000"
- C2 master connects and sends payload
- Payload output streams back to master
- Bot handles disconnection gracefully

## Benefits

1. **Separation of Concerns**: C2 bot is independent guest app, not firmware
2. **Easier Updates**: Modify bot behavior without firmware rebuild
3. **Cleaner Architecture**: All guest code uses POSIX APIs only
4. **Modularity**: Bot can be replaced/extended independently
5. **Testability**: C2 bot can be tested like any other guest app

## Backward Compatibility

- Firmware always loads DEFAULT_ELF_PATH (no C2 server embedded)
- `build_and_run.py --set-elf c2_bot` sets default to load C2 bot
- Existing guest apps (hello_world, tcp_client, etc.) unaffected
- Socket redirection mechanism identical

## References

- Original C2 system docs: `documentation/task_outputs/07_Demo1_C2_System.md`
- Guest app build system: `tools/build_guest_app.bat`
- Build automation: `tools/build_and_run.py`
