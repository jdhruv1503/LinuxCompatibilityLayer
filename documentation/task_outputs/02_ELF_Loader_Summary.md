# Task 02: ELF Loader Implementation Summary

**Status:** Complete
**Date:** 2025-11-27
**Component:** `espressif/elf_loader` v1.1.1

---

## 1. Executive Summary

Successfully integrated the Espressif ELF Loader component to load and execute position-independent Xtensa ELF binaries from LittleFS storage. Guest ELFs can be loaded, relocated to IRAM, and executed with full return value capture.

---

## 2. Technical Implementation

### 2.1 ELF Loader Integration

The `espressif/elf_loader` component (v1.1.1) handles:
- ELF header parsing and validation
- Program header (PHDR) processing for PT_LOAD segments
- Memory allocation with proper IRAM/DRAM separation
- Xtensa-specific relocations
- Symbol resolution against firmware exports

**Key API Functions:**
```c
esp_elf_init(esp_elf_t *elf)          // Initialize ELF context
esp_elf_relocate(esp_elf_t *elf, const uint8_t *data)  // Parse & relocate
elf.entry(argc, argv)                  // Direct entry point call
esp_elf_deinit(esp_elf_t *elf)        // Cleanup
```

**Note:** `esp_elf_request()` returns status code (ESP_OK=0), NOT the app's return value. Use `elf.entry()` directly to capture actual return values.

### 2.2 Guest ELF Requirements

Guest ELFs must be compiled as **shared objects** (ET_DYN, type 3) with position-independent code:

| Requirement | Value | Reason |
|-------------|-------|--------|
| ELF Type | ET_DYN (3) | Relocatable without MMU |
| Machine | EM_XTENSA (94) | ESP32 architecture |
| Entry Point | `app_main` | ESP-IDF convention |
| Visibility | `default` | Symbol must be exported |

**Compiler Flags:**
```
CFLAGS  = -mlongcalls -fno-common -ffunction-sections -fdata-sections -fPIC -Os
LDFLAGS = -nostartfiles -nostdlib -fPIC -shared -Wl,--gc-sections -Wl,-z,now -e app_main
```

### 2.3 Memory Layout

After relocation, the ELF is placed in IRAM:
```
IRAM Range: 0x40080000 - 0x400A0000 (128KB)
Example:    elf.entry = 0x4008dcd0 (within IRAM)
```

The `esp_elf_t` structure tracks 5 sections (ELF_SECS=5):
- `ELF_SEC_TEXT` (0): Executable code
- `ELF_SEC_BSS` (1): Uninitialized data
- `ELF_SEC_DATA` (2): Initialized data
- `ELF_SEC_RODATA` (3): Read-only data
- `ELF_SEC_DRLRO` (4): Dynamic read-only

### 2.4 Files Created/Modified

| File | Purpose |
|------|---------|
| `main/main.c` | Core ELF loader with `load_and_run_elf()` function |
| `apps/hello_world/main.c` | Minimal test ELF (returns 42) |
| `tools/build_guest_app.bat` | Windows batch script for guest compilation |
| `tools/run_build.bat` | ESP-IDF build wrapper (handles MSYSTEM issue) |
| `tools/export_symbols.py` | Symbol table generator (for future custom symbols) |
| `sdkconfig.defaults` | ELF loader configuration |

---

## 3. Bug Workarounds

### 3.1 esp_elf_print_sec() Array Bounds Bug

**Issue:** `esp_elf_print_sec()` in the component has an array bounds bug.
```c
// In elf_loader component:
static const char *sec_names[] = { "text", "bss", "data", "rodata" };  // 4 elements
#define ELF_SECS 5  // But iterates 5 times!
```

**Workaround:** Do not call `esp_elf_print_sec()`. Use custom print:
```c
ESP_LOGI(TAG, "ELF Sections: text=0x%08x(%u), entry=%p",
         (unsigned)elf.sec[0].addr, (unsigned)elf.sec[0].size, elf.entry);
```

### 3.2 Return Value Capture

**Issue:** `esp_elf_request()` returns ESP_OK (0) on success, not app's return value.

**Workaround:** Call entry point directly:
```c
if (elf.entry) {
    ret = elf.entry(argc, argv);
}
```

---

## 4. Build System

### 4.1 Firmware Build (Windows)

```batch
cd C:\Users\Dhruv\Documents\Projects\LinuxCompatibilityLayer
tools\run_build.bat build
```

The `run_build.bat` script:
1. Clears MSYSTEM environment variables (ESP-IDF rejects MSYS2)
2. Sets up IDF_PATH and IDF_TOOLS_PATH
3. Invokes `idf.py` via the IDF Python environment

### 4.2 Guest ELF Build

```batch
cd C:\Users\Dhruv\Documents\Projects\LinuxCompatibilityLayer
tools\build_guest_app.bat hello_world
```

Output: `build/guest_apps/hello_world.elf` (also copied to `data/`)

### 4.3 QEMU Testing

```batch
# 1. Create merged flash image
cd build
"C:\Users\Dhruv\.espressif\python_env\idf5.4_py3.11_env\Scripts\python.exe" ^
    -m esptool --chip esp32 merge_bin -o merged-flash.bin ^
    --flash_mode dio --flash_size 4MB ^
    0x1000 bootloader/bootloader.bin ^
    0x8000 partition_table/partition-table.bin ^
    0xd000 ota_data_initial.bin ^
    0x10000 linux_compat_layer.bin ^
    0x190000 linux_fs.bin

# 2. Pad to 4MB
dd if=/dev/zero bs=1 count=$((4194304 - $(wc -c < merged-flash.bin))) >> merged-flash.bin

# 3. Run QEMU
"C:\Users\Dhruv\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe" ^
    -nographic -machine esp32 ^
    -drive file=merged-flash.bin,if=mtd,format=raw ^
    -no-reboot
```

---

## 5. Verification Results

### 5.1 QEMU Output (Successful)

```
I (3359) kernel_main: Loading ELF: /linux/hello_world.elf
I (3359) ELF: ELF loader version: 1.1.1
I (3829) kernel_main: ELF file size: 768 bytes
I (3839) ELF: Type:                                    3
I (3839) ELF: Machine:                                 94
I (3839) ELF: Entry point address:                     130
I (3839) ELF: elf->entry=0x4008dcd0
I (3849) kernel_main: ELF Sections: text=0x4008dcd0(8), entry=0x4008dcd0
I (3849) kernel_main: Starting ELF execution (argc=3)...
I (3849) kernel_main: ----------------------------------------
I (3849) kernel_main: ----------------------------------------
I (3849) kernel_main: ELF execution completed, return value: 42
I (3849) kernel_main: Test ELF returned: 42
```

### 5.2 Key Metrics

| Metric | Value |
|--------|-------|
| ELF File Size | 768 bytes |
| .text Section Size | 8 bytes |
| Entry Point (Relocated) | 0x4008dcd0 |
| Return Value | 42 (correct) |
| Load Time | ~500ms (in QEMU) |

---

## 6. Limitations

### 6.1 Current Limitations

1. **No C Standard Library**: Guest ELFs cannot use `printf()`, `malloc()`, `strlen()`, etc.
2. **No System Calls**: No `open()`, `read()`, `write()`, `exit()` support
3. **No External Symbol Resolution**: Guest cannot call firmware functions (yet)
4. **No argc/argv Parsing**: Arguments passed but not usable without libc

### 6.2 Required for Task 03

To enable standard Linux apps (with printf), the following must be implemented:
- Standard I/O file descriptors (0=stdin, 1=stdout, 2=stderr)
- `write()` syscall routing to ESP-IDF console
- Symbol export table with libc functions
- Memory allocation functions (malloc/free)

---

## 7. Code References

### 7.1 Entry Point Definition (Guest)
```c
// apps/hello_world/main.c:7-11
__attribute__((visibility("default")))
int app_main(int argc, char *argv[])
{
    return 42;
}
```

### 7.2 ELF Loading (Host)
```c
// main/main.c:21-101
int load_and_run_elf(const char *filepath, int argc, char *argv[])
{
    esp_elf_t elf;
    // ... init, read file, relocate ...
    if (elf.entry) {
        ret = elf.entry(argc, argv);  // Direct call for return value
    }
    esp_elf_deinit(&elf);
    return ret;
}
```

---

## 8. Next Steps (Task 03)

1. Implement `shim_write()` to route stdout (fd=1) to ESP console
2. Export libc symbols via CONFIG_ELF_LOADER_LIBC_SYMBOLS
3. Create minimal newlib stubs for unsupported functions
4. Test with `printf("Hello World\n")` guest app
