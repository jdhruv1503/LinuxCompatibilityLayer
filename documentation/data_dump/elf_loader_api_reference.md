# ESP32 ELF Loader API Reference

**Component:** `espressif/elf_loader` v1.1.1
**Source:** ESP Component Registry

---

## 1. Core Data Structures

### 1.1 esp_elf_t - ELF Object

```c
typedef struct esp_elf {
    unsigned char   *psegment;          // Segment buffer pointer
    uint32_t         svaddr;            // Start virtual address of segment
    unsigned char   *ptext;             // Instruction buffer pointer
    unsigned char   *pdata;             // Data buffer pointer
    esp_elf_sec_t   sec[ELF_SECS];      // Section info array (5 sections)
    int (*entry)(int argc, char *argv[]); // Entry point function pointer
} esp_elf_t;
```

### 1.2 esp_elf_sec_t - Section Object

```c
typedef struct esp_elf_sec {
    uintptr_t       v_addr;             // Virtual address
    off_t           offset;             // Offset in ELF
    uintptr_t       addr;               // Physical address in memory
    size_t          size;               // Section size
} esp_elf_sec_t;
```

### 1.3 Section Indices

```c
#define ELF_SEC_TEXT            0       // .text (executable code)
#define ELF_SEC_BSS             1       // .bss (uninitialized data)
#define ELF_SEC_DATA            2       // .data (initialized data)
#define ELF_SEC_RODATA          3       // .rodata (read-only data)
#define ELF_SEC_DRLRO           4       // .data.rel.ro (dynamic read-only)
#define ELF_SECS                5       // Total sections
```

---

## 2. API Functions

### 2.1 esp_elf_init

```c
int esp_elf_init(esp_elf_t *elf);
```

**Purpose:** Initialize ELF object structure.

**Parameters:**
- `elf`: Pointer to ELF object

**Returns:**
- `ESP_OK` (0) on success
- Negative error code on failure

**Usage:**
```c
esp_elf_t elf;
int ret = esp_elf_init(&elf);
if (ret != 0) {
    ESP_LOGE(TAG, "Failed to init ELF loader: %d", ret);
}
```

---

### 2.2 esp_elf_relocate

```c
int esp_elf_relocate(esp_elf_t *elf, const uint8_t *pbuf);
```

**Purpose:** Parse ELF data, allocate memory, and perform relocations.

**Parameters:**
- `elf`: Initialized ELF object
- `pbuf`: Buffer containing raw ELF file data

**Returns:**
- `ESP_OK` (0) on success
- Negative error code on failure

**Memory Allocation:**
- Allocates IRAM for executable code (via `heap_caps_malloc(MALLOC_CAP_EXEC)`)
- Allocates DRAM for data sections

**Important:** The input buffer (`pbuf`) can be freed after this call.

---

### 2.3 esp_elf_request

```c
int esp_elf_request(esp_elf_t *elf, int opt, int argc, char *argv[]);
```

**Purpose:** Execute the ELF entry point.

**Parameters:**
- `elf`: Relocated ELF object
- `opt`: Request options (usually 0)
- `argc`: Argument count
- `argv`: Argument array

**Returns:**
- `ESP_OK` (0) if execution completed successfully
- **NOT the return value of the ELF function!**

**Warning:** This function returns a status code, not the app's return value. To get the actual return value, call `elf.entry()` directly:

```c
// WRONG: ret will be 0 (ESP_OK) even if app returns 42
ret = esp_elf_request(&elf, 0, argc, argv);

// CORRECT: ret will be the actual return value (e.g., 42)
if (elf.entry) {
    ret = elf.entry(argc, argv);
}
```

---

### 2.4 esp_elf_deinit

```c
void esp_elf_deinit(esp_elf_t *elf);
```

**Purpose:** Free all memory allocated for ELF and reset structure.

**Parameters:**
- `elf`: ELF object to deinitialize

**Usage:**
```c
esp_elf_deinit(&elf);
// elf is now invalid - do not use
```

---

### 2.5 Debug Print Functions

```c
void esp_elf_print_ehdr(const uint8_t *pbuf);  // Print ELF header
void esp_elf_print_phdr(const uint8_t *pbuf);  // Print program headers
void esp_elf_print_shdr(const uint8_t *pbuf);  // Print section headers
void esp_elf_print_sec(esp_elf_t *elf);        // Print loaded sections (BUGGY!)
```

**BUG WARNING:** `esp_elf_print_sec()` has an array bounds bug and will crash. Do not use.

---

## 3. Configuration Options

In `sdkconfig.defaults`:

```ini
# Enable ELF loader
CONFIG_ELF_LOADER=y

# Export ESP-IDF libc symbols to guest ELFs
CONFIG_ELF_LOADER_LIBC_SYMBOLS=y

# Export ESP-IDF framework symbols (logging, GPIO, etc.)
CONFIG_ELF_LOADER_ESPIDF_SYMBOLS=y

# Custom symbol table (advanced)
CONFIG_ELF_LOADER_CUSTOM_SYMBOL_TABLE=n
```

---

## 4. Symbol Resolution

The ELF loader resolves undefined symbols in guest ELFs against exported firmware symbols.

### 4.1 Default Exported Symbols (LIBC_SYMBOLS)

When `CONFIG_ELF_LOADER_LIBC_SYMBOLS=y`:
- `printf`, `sprintf`, `snprintf`
- `memcpy`, `memset`, `memmove`
- `strlen`, `strcpy`, `strcmp`, `strcat`
- `malloc`, `free`, `realloc`, `calloc`
- And more newlib functions...

### 4.2 Default Exported Symbols (ESPIDF_SYMBOLS)

When `CONFIG_ELF_LOADER_ESPIDF_SYMBOLS=y`:
- `ESP_LOGI`, `ESP_LOGW`, `ESP_LOGE`, `ESP_LOGD`
- `gpio_set_level`, `gpio_get_level`
- `vTaskDelay`
- And more ESP-IDF functions...

### 4.3 Custom Symbol Export

To export custom shim functions, use `CONFIG_ELF_LOADER_CUSTOM_SYMBOL_TABLE` and provide a symbol table in the format expected by the component.

---

## 5. Supported ELF Types

| ELF Type | Value | Supported | Notes |
|----------|-------|-----------|-------|
| ET_NONE | 0 | No | Invalid |
| ET_REL | 1 | No | Relocatable (use -shared instead) |
| ET_EXEC | 2 | No | Absolute addresses won't work |
| **ET_DYN** | **3** | **Yes** | Position-independent shared object |
| ET_CORE | 4 | No | Core dump |

**Only ET_DYN (type 3) ELFs are supported.** Compile with `-shared -fPIC`.

---

## 6. Xtensa Relocation Types

The loader handles these Xtensa-specific relocations:

| Type | Value | Description |
|------|-------|-------------|
| R_XTENSA_NONE | 0 | No relocation |
| R_XTENSA_32 | 1 | 32-bit absolute |
| R_XTENSA_RTLD | 2 | Runtime linker |
| R_XTENSA_GLOB_DAT | 3 | GOT entry |
| R_XTENSA_JMP_SLOT | 4 | PLT entry |
| R_XTENSA_RELATIVE | 5 | Base + addend |
| R_XTENSA_PLT | 6 | PLT relocation |

---

## 7. Memory Requirements

### 7.1 IRAM Usage

Code sections are allocated in IRAM:
- Address range: `0x40080000 - 0x400A0000` (128KB total)
- Must have `MALLOC_CAP_EXEC` capability
- Shared with firmware interrupt handlers

### 7.2 DRAM Usage

Data sections allocated in DRAM:
- `.data`, `.bss`, `.rodata` sections
- Uses standard heap allocation

### 7.3 Typical Memory Usage

| Section | Minimal ELF | Typical App |
|---------|-------------|-------------|
| .text | 8 bytes | 1-10 KB |
| .data | 0 bytes | 100-500 bytes |
| .rodata | 0 bytes | 100-1000 bytes |
| .bss | 0 bytes | 100-500 bytes |

---

## 8. Error Codes

| Code | Meaning |
|------|---------|
| 0 (ESP_OK) | Success |
| -1 | Generic error |
| ESP_ERR_NO_MEM | Out of memory |
| ESP_ERR_INVALID_ARG | Invalid parameter |
| ESP_ERR_NOT_SUPPORTED | Unsupported ELF type/arch |

---

## 9. Complete Example

```c
#include "esp_elf.h"
#include "esp_log.h"

int load_and_run_elf(const char *filepath, int argc, char *argv[])
{
    esp_elf_t elf;
    int ret;

    // 1. Initialize
    ret = esp_elf_init(&elf);
    if (ret != 0) return -1;

    // 2. Read file into buffer
    FILE *f = fopen(filepath, "rb");
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(size);
    fread(data, 1, size, f);
    fclose(f);

    // 3. Relocate
    ret = esp_elf_relocate(&elf, data);
    free(data);  // No longer needed
    if (ret != 0) {
        esp_elf_deinit(&elf);
        return -1;
    }

    // 4. Execute (call entry directly for return value)
    if (elf.entry) {
        ret = elf.entry(argc, argv);
    } else {
        ret = -1;
    }

    // 5. Cleanup
    esp_elf_deinit(&elf);
    return ret;
}
```
