# ELF Loader Research for ESP32

## Official Espressif ELF Loader Component (v1.1.1)

**Source:** https://components.espressif.com/components/espressif/elf_loader

### Core API Functions
```c
esp_elf_init(&elf)                              // Initialize ELF loader structure
esp_elf_relocate(&elf, elf_file_data_bytes)     // Relocate ELF to executable memory
esp_elf_request(&elf, 0, argc, argv)            // Execute loaded ELF with arguments
esp_elf_deinit(&elf)                            // Cleanup and deallocate
```

### Supported Hardware
- ESP32
- ESP32-S2 (PSRAM support)
- ESP32-S3 (PSRAM support)
- ESP32-C6
- ESP32-C61 (PSRAM support)
- ESP32-P4 (PSRAM support)

### Configuration (menuconfig)
```
Component config → ESP-ELFLoader Configuration → [*] Enable Espressif ELF Loader
```

### Add Dependency (idf_component.yml)
```yaml
dependencies:
  espressif/elf_loader: "1.*"
```

### CMake Integration
```cmake
include(elf_loader)
project_elf(XXXX)
```

### Fast ELF-Only Build (Unix Makefiles)
```bash
idf.py -G 'Unix Makefiles' set-target esp32
idf.py elf
```

---

## Alternative: niicoooo/esp32-elfloader

**Source:** https://github.com/niicoooo/esp32-elfloader

### Compilation Flags for Guest ELF
```bash
-fno-common -Wl,-r -nostartfiles -nodefaultlibs -nostdlib
```

### Data Structures
```c
typedef struct {
    const char *name;
    void *func;
} ELFLoaderSymbol_t;

typedef struct {
    ELFLoaderSymbol_t *exports;
    size_t count;
} ELFLoaderEnv_t;
```

### API Functions
```c
elfLoaderInitLoadAndRelocate()  // Init, load, relocate - returns context handle
elfLoaderSetFunc()              // Locate function by name in loaded module
elfLoaderRun()                  // Execute function with int argument
elfLoaderFree()                 // Deallocate all resources
```

---

## Xtensa ELF Relocation Types

**Source:** https://github.com/espressif/binutils-esp32ulp/blob/master/include/elf/xtensa.h

### Key Relocation Types
| Type | Value | Description |
|------|-------|-------------|
| R_XTENSA_NONE | 0 | No relocation |
| R_XTENSA_32 | 1 | Direct 32-bit |
| R_XTENSA_RTLD | 2 | Runtime linker |
| R_XTENSA_GLOB_DAT | 3 | Global data |
| R_XTENSA_JMP_SLOT | 4 | Jump slot |
| R_XTENSA_RELATIVE | 5 | Relative address |
| R_XTENSA_SLOT0_OP | 20 | Instruction operand (slot 0) |
| R_XTENSA_SLOT0_ALT | 35 | Alternate operand (slot 0) |

### R_XTENSA_SLOT0_OP Handling
- Modifies immediate values in Xtensa instructions
- Used for CALL, J, L32R instructions
- Requires extracting, modifying, and reinserting immediate fields

### R_XTENSA_RELATIVE Handling
```c
uint32_t *addr = (uint32_t *)(segment_base + reloc_entry->r_offset);
*addr += delta;  // delta = actual_load_addr - linked_addr
```

### Implementation Reference
- binutils: `bfd/elf32-xtensa.c`
- Function: `elf_xtensa_do_reloc()`

---

## LittleFS Integration

**Source:** https://components.espressif.com/components/joltwallet/littlefs

### Add Dependency (idf_component.yml)
```yaml
dependencies:
  joltwallet/littlefs: "^1.20"
```

### CMake Partition Image Creation
```cmake
# Pack folder contents into LittleFS partition image
littlefs_create_partition_image(linux_fs data FLASH_IN_PROJECT)
```

### Mount Configuration
```c
esp_vfs_littlefs_conf_t conf = {
    .base_path = "/linux",
    .partition_label = "linux_fs",
    .format_if_mount_failed = true,
    .dont_mount = false,
};
esp_err_t ret = esp_vfs_littlefs_register(&conf);
```

### Partition Table Entry
```csv
linux_fs,   data, 0x83,    ,  2M,
```

**Note:** Windows does not support LittleFS partition generation - use WSL or Linux VM.

---

## ESP-IDF VFS API

**Source:** https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/storage/vfs.html

### VFS Registration (v5.x API)
```c
esp_err_t esp_vfs_register_fs(const char *base_path,
                               const esp_vfs_fs_ops_t *vfs,
                               int flags, void *ctx);
```

### VFS Structure (Legacy v4.x compatible)
```c
esp_vfs_t my_vfs = {
    .flags = ESP_VFS_FLAG_DEFAULT,
    .open = &my_open,
    .close = &my_close,
    .read = &my_read,
    .write = &my_write,
    .ioctl = &my_ioctl,
    // Optional: stat, fstat, lseek, etc.
};

ESP_ERROR_CHECK(esp_vfs_register("/dev/mydevice", &my_vfs, NULL));
```

### VFS Flags
- `ESP_VFS_FLAG_DEFAULT`: VFS copies structure to RAM
- `ESP_VFS_FLAG_STATIC`: Structure is statically allocated
- `ESP_VFS_FLAG_CONTEXT_PTR`: Use `*_p` function variants with context

### File Descriptor Range
- FD 0-2: stdin, stdout, stderr (mapped to UART by default)
- FDs are small positive integers up to `FD_SETSIZE - 1`
