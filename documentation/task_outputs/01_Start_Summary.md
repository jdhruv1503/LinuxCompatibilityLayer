# Task 01: Project Setup - Technical Summary

## Overview

Task 01 established the foundational infrastructure for the Thin Linux Compatibility Layer (TLCL) project on ESP32. This involved creating the ESP-IDF project structure, configuring custom partitions for LittleFS filesystem storage, and verifying functionality using QEMU emulation.

## Environment Setup

### Development Environment (Windows)

| Component | Path | Version |
|-----------|------|---------|
| ESP-IDF | `C:\Users\Dhruv\.esp-tools\esp-idf` | v5.4 |
| Python | `C:\Users\Dhruv\.esp-tools\python-full` | 3.11.7 |
| QEMU-Xtensa | `C:\Users\Dhruv\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe` | esp_develop_9.0.0 |
| Toolchain | Installed via `idf_tools.py` | Xtensa ESP32 |

### Critical Windows/MSYS2 Workaround

ESP-IDF refuses to run in Git Bash/MSYS2 environments (detects `MSYSTEM` variable and exits). Solution: Execute all ESP-IDF commands through `cmd.exe` with environment clearing:

```batch
@echo off
set MSYSTEM=
set IDF_PATH=C:\Users\Dhruv\.esp-tools\esp-idf
set PATH=C:\Users\Dhruv\.esp-tools\python-full;C:\Users\Dhruv\.esp-tools\python-full\Scripts;%PATH%
call "%IDF_PATH%\export.bat"
cd /d C:\Users\Dhruv\Documents\Projects\LinuxCompatibilityLayer
idf.py build
```

## Files Created

### 1. Project Root Configuration

#### `CMakeLists.txt`
```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(PARTITION_TABLE_CUSTOM_FILENAME "partitions.csv")
project(linux_compat_layer)
```

#### `partitions.csv`
Custom partition table optimized for 4MB flash:

```csv
# Name,     Type, SubType, Offset,  Size, Flags
nvs,        data, nvs,     0x9000,  0x4000,
otadata,    data, ota,     0xd000,  0x2000,
phy_init,   data, phy,     0xf000,  0x1000,
factory,    app,  factory, 0x10000, 1536K,
linux_fs,   data, 0x83,    ,        1536K,
```

**Memory Layout:**
- `0x0000 - 0x0FFF`: Bootloader reserved
- `0x1000 - 0x8FFF`: Bootloader (32KB)
- `0x9000 - 0xCFFF`: NVS (16KB)
- `0xD000 - 0xEFFF`: OTA data (8KB)
- `0xF000 - 0xFFFF`: PHY init (4KB)
- `0x10000 - 0x18FFFF`: Factory app (1536KB = 1.5MB)
- `0x190000 - 0x30FFFF`: LittleFS linux_fs (1536KB = 1.5MB)

Total: ~3.1MB used of 4MB flash

#### `sdkconfig.defaults`
```
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y
CONFIG_LITTLEFS_MAX_PARTITIONS=3
CONFIG_LITTLEFS_OBJ_NAME_LEN=64
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_HEAP_POISONING_DISABLED=y
```

### 2. Main Application Component

#### `main/idf_component.yml`
```yaml
dependencies:
  joltwallet/littlefs: "^1.20"
  espressif/elf_loader: "^1.1"
```

**Note:** The `joltwallet/littlefs` component uses `esp_littlefs.h` header, NOT `esp_vfs_littlefs.h`.

#### `main/CMakeLists.txt`
```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS ".")
littlefs_create_partition_image(linux_fs ../data FLASH_IN_PROJECT)
```

The `littlefs_create_partition_image()` macro:
- Creates `linux_fs.bin` from `data/` directory contents
- Automatically adds to flash target
- Partition label must match `partitions.csv`

#### `main/main.c`
```c
#include <stdio.h>
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "kernel_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing LittleFS for Linux Compatibility Layer...");

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/linux",
        .partition_label = "linux_fs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)", esp_err_to_name(ret));
        }
        return;
    }
    ESP_LOGI(TAG, "LittleFS mounted successfully at /linux");
}
```

### 3. Data Directory

#### `data/.placeholder`
Empty file to ensure the `data/` directory is tracked by git and included in LittleFS image generation.

## Build Process

### Full Build Command Sequence

```batch
:: From Git Bash, invoke through cmd.exe
cmd.exe //c "C:\Users\Dhruv\.esp-tools\build_project.bat"
```

### Build Artifacts

| File | Description | Size |
|------|-------------|------|
| `build/linux_compat_layer.bin` | Application binary | ~191KB |
| `build/bootloader/bootloader.bin` | Bootloader | ~26KB |
| `build/partition_table/partition-table.bin` | Partition table | 3KB |
| `build/ota_data_initial.bin` | OTA data | 8KB |
| `build/linux_fs.bin` | LittleFS image | Variable |

## QEMU Simulation

### Creating Merged Flash Binary

QEMU requires a single binary file containing all flash contents:

```bash
# From project directory
cd build

# Merge all flash components
python -m esptool --chip esp32 merge_bin \
    -o merged-flash.bin \
    --flash_mode dio \
    --flash_size 4MB \
    0x1000 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0xd000 ota_data_initial.bin \
    0x10000 linux_compat_layer.bin \
    0x190000 linux_fs.bin

# Pad to exactly 4MB (QEMU requirement)
dd if=/dev/zero bs=1 count=$((4194304 - $(stat -c%s merged-flash.bin))) >> merged-flash.bin
```

### Running QEMU Simulation

```bash
# Full path for Windows
/c/Users/Dhruv/.espressif/tools/qemu-xtensa/esp_develop_9.0.0_20240606/qemu/bin/qemu-system-xtensa.exe \
    -nographic \
    -machine esp32 \
    -drive file=build/merged-flash.bin,if=mtd,format=raw \
    -no-reboot
```

**QEMU Options:**
- `-nographic`: No GUI, serial output to terminal
- `-machine esp32`: ESP32 machine type
- `-drive file=...,if=mtd,format=raw`: Flash as MTD device
- `-no-reboot`: Exit on reset instead of rebooting

### Successful Output

```
I (1836) kernel_main: Initializing LittleFS for Linux Compatibility Layer...
I (2006) kernel_main: LittleFS mounted successfully at /linux
```

## Errors Encountered and Solutions

### 1. MSYSTEM Environment Variable

**Error:**
```
WARNING: esp-idf.git is not supported in MSYS/MSYS2/Cygwin environment.
```

**Solution:** Clear MSYSTEM before invoking ESP-IDF:
```batch
set MSYSTEM=
```

### 2. Python venv Module Missing

**Error:**
```
Error: Command '['python', '-m', 'venv', ...]' returned non-zero exit status 1
```

**Cause:** Python embeddable package lacks venv module.

**Solution:** Install full Python distribution instead of embeddable.

### 3. Partition Table Overflow

**Error:**
```
Partition linux_fs invalid: Ends at 0x410000 but flash only 0x400000
```

**Cause:** Initial partition sizes (2MB each) exceeded 4MB flash.

**Solution:** Reduced partitions to 1536KB each with explicit offsets.

### 4. LittleFS Header Not Found

**Error:**
```
fatal error: esp_vfs_littlefs.h: No such file or directory
```

**Cause:** The `joltwallet/littlefs` component uses different header name.

**Solution:** Changed to `#include "esp_littlefs.h"`

### 5. QEMU Flash Size Mismatch

**Symptom:** QEMU fails to boot or shows incorrect behavior.

**Cause:** Merged binary was not exactly 4MB.

**Solution:** Pad binary with zeros to exactly 4,194,304 bytes.

## Verification Checklist

- [x] ESP-IDF v5.4 installed and configured
- [x] Project builds without errors
- [x] Custom partition table recognized
- [x] LittleFS component downloaded from registry
- [x] ELF loader component downloaded from registry
- [x] LittleFS image generated from data/ directory
- [x] QEMU simulation boots successfully
- [x] LittleFS mounts at /linux mount point
- [x] Serial output visible in QEMU

## Dependencies Installed

From ESP Component Registry:
- `joltwallet/littlefs@1.20.2` - LittleFS VFS driver
- `espressif/elf_loader@1.1.0` - ELF binary loader (for future tasks)

## Next Steps (Task 02)

Task 02 will implement the ELF loader to:
1. Parse ELF headers for Xtensa binaries
2. Allocate IRAM for executable segments
3. Allocate DRAM for data segments
4. Perform Xtensa-specific relocations
5. Execute loaded binaries as FreeRTOS tasks
