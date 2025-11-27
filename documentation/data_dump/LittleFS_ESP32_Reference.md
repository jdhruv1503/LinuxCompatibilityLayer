# LittleFS on ESP32 Reference

## Overview

LittleFS is a fail-safe filesystem designed for microcontrollers with limited RAM and flash. On ESP32, it's accessed through the ESP VFS (Virtual File System) layer.

## Component Installation

### Using ESP Component Registry

In `main/idf_component.yml`:
```yaml
dependencies:
  joltwallet/littlefs: "^1.20"
```

**Important:** The header file is `esp_littlefs.h`, NOT `esp_vfs_littlefs.h`.

### Manual Include

```c
#include "esp_littlefs.h"
```

## Partition Configuration

### partitions.csv Entry

```csv
# Name,     Type, SubType, Offset, Size, Flags
linux_fs,   data, 0x83,    ,       1536K,
```

- **Type:** `data` (0x01)
- **SubType:** `0x83` (user-defined for LittleFS)
- **Size:** Power-of-2 recommended but not required

### Partition Alignment

LittleFS works with any partition size, but:
- Minimum: 64KB recommended
- Block size: 4096 bytes (ESP32 flash page size)
- Should be aligned to flash sector boundary

## API Reference

### Configuration Structure

```c
esp_vfs_littlefs_conf_t conf = {
    .base_path = "/linux",              // VFS mount point
    .partition_label = "linux_fs",       // Must match partitions.csv
    .format_if_mount_failed = true,      // Auto-format corrupt FS
    .dont_mount = false,                 // Mount immediately
    .grow_on_mount = true,               // Expand to partition size
};
```

### Mount Operations

```c
// Register and mount
esp_err_t ret = esp_vfs_littlefs_register(&conf);

// Check return codes
switch (ret) {
    case ESP_OK:
        // Success
        break;
    case ESP_FAIL:
        // Mount failed (possibly corrupt)
        break;
    case ESP_ERR_NOT_FOUND:
        // Partition not found
        break;
    case ESP_ERR_INVALID_STATE:
        // Already mounted
        break;
    case ESP_ERR_NO_MEM:
        // Out of memory
        break;
}

// Unmount
esp_vfs_littlefs_unregister(conf.partition_label);
```

### Filesystem Info

```c
size_t total = 0, used = 0;
esp_err_t ret = esp_littlefs_info(conf.partition_label, &total, &used);
if (ret == ESP_OK) {
    ESP_LOGI(TAG, "LittleFS: %d/%d bytes used", used, total);
}
```

### Format Operations

```c
// Manual format (erases all data)
esp_littlefs_format(conf.partition_label);
```

## Creating LittleFS Images

### Build System Integration

In `main/CMakeLists.txt`:
```cmake
littlefs_create_partition_image(linux_fs ../data FLASH_IN_PROJECT)
```

Arguments:
1. `linux_fs` - Partition label (must match partitions.csv)
2. `../data` - Source directory for filesystem contents
3. `FLASH_IN_PROJECT` - Include in flash target

### Manual Image Creation

```bash
# Install mklittlefs
pip install littlefs-python

# Create image
python -m littlefs --create --block_size=4096 \
    --block_count=384 \  # 1536KB / 4KB
    --image=linux_fs.bin \
    data/
```

## File Operations (Standard POSIX)

Once mounted, use standard C file operations:

```c
// Write file
FILE *f = fopen("/linux/test.txt", "w");
fprintf(f, "Hello LittleFS!\n");
fclose(f);

// Read file
f = fopen("/linux/test.txt", "r");
char buf[128];
fgets(buf, sizeof(buf), f);
fclose(f);

// Check existence
struct stat st;
if (stat("/linux/test.txt", &st) == 0) {
    ESP_LOGI(TAG, "File size: %ld", st.st_size);
}

// Delete file
unlink("/linux/test.txt");

// Create directory
mkdir("/linux/subdir", 0755);

// List directory
DIR *dir = opendir("/linux");
struct dirent *entry;
while ((entry = readdir(dir)) != NULL) {
    ESP_LOGI(TAG, "Found: %s", entry->d_name);
}
closedir(dir);
```

## Limitations

1. **No Permissions**: LittleFS doesn't store Unix permissions; stat() returns fake 0777
2. **No Hard Links**: Only files and directories supported
3. **No Symbolic Links**: Not supported
4. **Max Path Length**: Configurable via `CONFIG_LITTLEFS_OBJ_NAME_LEN` (default 64)
5. **No Timestamps**: mtime/atime not stored (some implementations add this)

## Configuration Options (sdkconfig)

```
# Maximum partitions (default 3)
CONFIG_LITTLEFS_MAX_PARTITIONS=3

# Object name length (path component max)
CONFIG_LITTLEFS_OBJ_NAME_LEN=64

# Cache size (performance vs RAM tradeoff)
CONFIG_LITTLEFS_CACHE_SIZE=256

# Lookahead buffer size
CONFIG_LITTLEFS_LOOKAHEAD_SIZE=128

# Enable/disable file time (mtime)
CONFIG_LITTLEFS_USE_MTIME=y
```

## Troubleshooting

### "Failed to find LittleFS partition"

1. Check partition label matches between code and partitions.csv
2. Verify `CONFIG_PARTITION_TABLE_CUSTOM=y` in sdkconfig
3. Ensure partition table is flashed

### "Failed to mount or format filesystem"

1. Flash may be corrupted - try erasing: `idf.py erase-flash`
2. Partition too small for LittleFS metadata
3. Check flash is not write-protected

### Files Not Persisting

1. Ensure proper `fclose()` or `fflush()` after writes
2. Check `format_if_mount_failed` isn't erasing on each boot
3. Verify partition isn't being reflashed on each upload

## References

- [joltwallet/littlefs Component](https://components.espressif.com/components/joltwallet/littlefs)
- [LittleFS Design Documentation](https://github.com/littlefs-project/littlefs/blob/master/DESIGN.md)
- [ESP-IDF VFS Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/vfs.html)
