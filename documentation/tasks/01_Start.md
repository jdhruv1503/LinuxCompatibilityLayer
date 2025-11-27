## **1. Project Overview & Directory Structure**

This guide details the setup of an ESP-IDF project designed to simulate a Linux-like environment on the ESP32. We will use a "Unikernel" approach where "processes" are ELF binaries loaded from a filesystem.

To facilitate rapid development without constant hardware flashing, we will configure **QEMU ESP32 emulation** to run our firmware with the custom partition table and filesystem.

### **Recommended Directory Layout**

Create a new project directory with the following structure:

```
project_root/
├── CMakeLists.txt              # Top-level build script
├── sdkconfig.defaults          # Default config (Flash size, LittleFS settings)
├── partitions.csv              # Custom partition table
├── main/
│   ├── CMakeLists.txt          # Main component build script
│   ├── idf_component.yml       # Component dependencies
│   └── main.c                  # Firmware entry point (Mounts FS)
└── data/                       # Staging area for ELF binaries
    └── .placeholder            # (Create this file to ensure git tracks the folder)
```

---

## **2. Partition Table Configuration**

Standard ESP32 partition tables do not provide enough space for a filesystem capable of storing multiple ELF binaries. We must define a custom table.

**Action:** Create a file named `partitions.csv` in your project_root.

The partition must be named `linux_fs`, marked as data, and be sized appropriately for 4MB flash.

```csv
# Name,     Type, SubType, Offset,  Size, Flags
nvs,        data, nvs,     0x9000,  0x4000,
otadata,    data, ota,     0xd000,  0x2000,
phy_init,   data, phy,     0xf000,  0x1000,
factory,    app,  factory, 0x10000, 1536K,
linux_fs,   data, 0x83,    ,        1536K,
```

*Note: We use SubType 0x83 to designate this as our LittleFS data partition. Total usage ~3.1MB, fits in 4MB flash.*

---

## **3. Dependency Management (LittleFS & ELF Loader)**

We require the LittleFS component to manage the filesystem and the ELF loader for dynamic binary execution.

**Action:** Create a file `main/idf_component.yml`:

```yaml
dependencies:
  joltwallet/littlefs: "^1.20"
  espressif/elf_loader: "^1.1"
```

**Important:** The `joltwallet/littlefs` component uses `esp_littlefs.h` header (NOT `esp_vfs_littlefs.h`).

---

## **4. CMake Configuration & Image Packing**

This is the most critical step for automation. We will configure CMake to compile our firmware and then automatically pack the contents of the `data/` folder into a LittleFS image.

**Action:** Update `main/CMakeLists.txt` with the following content:

```cmake
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS ".")

# AUTOMATION: Pack the 'data' directory into a LittleFS image
# Arguments:
#   1. Partition Name (Must match partitions.csv): linux_fs
#   2. Base Directory: ../data
#   3. Flash Flag: FLASH_IN_PROJECT (Flashes automatically with idf.py flash)
littlefs_create_partition_image(linux_fs ../data FLASH_IN_PROJECT)
```

---

## **5. Firmware Entry Point (Mounting the Filesystem)**

Before we can run any ELF binaries, the firmware must mount the partition.

**Action:** Update `main/main.c` to initialize LittleFS on boot:

```c
#include <stdio.h>
#include "esp_littlefs.h"
#include "esp_log.h"

static const char *TAG = "kernel_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing LittleFS for Linux Compatibility Layer...");

    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/linux",      // Mount point
        .partition_label = "linux_fs", // Must match partitions.csv
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    // Register the filesystem
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

    // Future: Executing binaries will happen here
}
```

---

## **6. Top-Level Configuration Files**

### **6.1 Top-Level CMakeLists.txt**

Create `CMakeLists.txt` in the project root:

```cmake
cmake_minimum_required(VERSION 3.16)

# Include the ELF loader component for building guest applications
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Set custom partition table
set(PARTITION_TABLE_CUSTOM_FILENAME "partitions.csv")

project(linux_compat_layer)
```

### **6.2 sdkconfig.defaults**

Create `sdkconfig.defaults` in the project root to ensure consistent builds:

```ini
# Flash size (4MB)
CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y

# Enable LittleFS
CONFIG_LITTLEFS_MAX_PARTITIONS=3
CONFIG_LITTLEFS_OBJ_NAME_LEN=64

# Increase main task stack for ELF loader operations
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192

# Custom partition table
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# Heap configuration
CONFIG_HEAP_POISONING_DISABLED=y
```

### **6.3 Data Directory Placeholder**

Create `data/.placeholder` (empty file) to ensure the directory is tracked by git and included in LittleFS image generation.

---

## **7. Build and Verification**

### **7.1 Build the Project**

```bash
# From Git Bash on Windows, use cmd.exe wrapper
cmd.exe //c "set MSYSTEM= && C:\\Users\\Dhruv\\.esp-tools\\esp-idf\\export.bat && cd /d PROJECT_PATH && idf.py build"
```

*Observation:* During the build output, look for a line indicating `Generating littlefs image`. This confirms the `data/` folder was packed.

### **7.2 Create Merged Flash Binary for QEMU**

QEMU requires a single binary containing all flash partitions:

```bash
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

### **7.3 Run in QEMU**

```bash
# Run QEMU simulation
/c/Users/Dhruv/.espressif/tools/qemu-xtensa/esp_develop_9.0.0_20240606/qemu/bin/qemu-system-xtensa.exe \
    -nographic \
    -machine esp32 \
    -drive file=build/merged-flash.bin,if=mtd,format=raw \
    -no-reboot
```

### **7.4 Check Serial Output**

Watch the terminal output. You should see the boot logs followed by our success message:

```
I (xxx) kernel_main: Initializing LittleFS for Linux Compatibility Layer...
I (xxx) kernel_main: LittleFS mounted successfully at /linux
```

If you see "Failed to find LittleFS partition," verify that the name in `partitions.csv` matches `main.c` and `CMakeLists.txt` exactly.

---

## **8. QEMU Options Reference**

| Option | Description |
|--------|-------------|
| `-nographic` | Serial output to terminal (no GUI window) |
| `-machine esp32` | ESP32 machine type |
| `-drive file=...,if=mtd,format=raw` | Flash image as MTD device |
| `-no-reboot` | Exit on reset instead of rebooting (useful for testing) |
| `-s` | Start GDB server on port 1234 |
| `-S` | Start paused, wait for GDB connection |
| `-nic user,model=open_eth` | Enable NAT networking for network tasks |

---

## **9. Troubleshooting**

| Error | Cause | Solution |
|-------|-------|----------|
| `Failed to find LittleFS partition` | Partition name mismatch | Verify `linux_fs` in partitions.csv, main.c, and CMakeLists.txt |
| `E (xxx) boot: Factory app partition is not bootable` | Partition size too small | Verify partition offsets don't exceed 4MB flash |
| `LittleFS: No space left on device` | Data folder too large | Increase linux_fs partition or reduce payload size |
| `Build error: littlefs_create_partition_image` | Component not found | Run `idf.py reconfigure` after adding dependency |
| QEMU no output | Missing `-nographic` flag | Add `-nographic` to QEMU command |
| QEMU boot fails | Flash binary not 4MB | Pad merged-flash.bin to exactly 4194304 bytes |
| ESP-IDF won't run in Git Bash | MSYSTEM environment variable | Clear with `set MSYSTEM=` or use cmd.exe |
| `esp_vfs_littlefs.h` not found | Wrong header name | Use `esp_littlefs.h` instead |

---

## **10. Complete File Checklist**

After completing this task, your project should have:

- [ ] `CMakeLists.txt` (project root)
- [ ] `sdkconfig.defaults`
- [ ] `partitions.csv`
- [ ] `main/CMakeLists.txt`
- [ ] `main/main.c`
- [ ] `main/idf_component.yml`
- [ ] `data/.placeholder` (or any test file)
- [ ] `build/merged-flash.bin` (after build + merge)
