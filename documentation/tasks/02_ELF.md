## **1\. Architectural Constraints: IRAM vs. DRAM**

The ESP32 utilizes a modified Harvard architecture (Xtensa LX6/LX7). This imposes strict physical separation between Instruction RAM (IRAM) and Data RAM (DRAM).

* **IRAM (Instruction RAM):** \* Accessible only via the CPU's Instruction Fetch unit (and limited data access).  
  * **Constraint:** Code **must** reside here to be executed. If code is placed in DRAM, the CPU cannot fetch it, resulting in an InstrFetchProhibited panic.  
  * **Allocation:** Memory must be allocated using specific capability flags.  
* **DRAM (Data RAM):**  
  * Accessible via the Data Load/Store unit.  
  * Used for variables, stack, and heap data.  
  * **Constraint:** Code placed here is non-executable.

## **2\. ELF Parsing Pipeline**

The loader must implement a strict two-pass parsing strategy to ensure correct memory placement.

### **2.1 Validation**

The loader must first validate the ELF Header (Elf32\_Ehdr) to ensure compatibility:

1. **Magic Bytes:** Must match 0x7F 'E' 'L' 'F'.  
2. **Machine Type:** Must be EM\_XTENSA (0x5E).  
3. **Class:** Must be ELFCLASS32.

### **2.2 Segment Loading (Program Headers)**

The loader iterates through the Program Headers (Elf32\_Phdr). It must **ignore** headers where p\_type \!= PT\_LOAD.

For every PT\_LOAD segment, the loader determines the destination memory pool based on the segment's flags (p\_flags):

| Segment Flag | Description | Allocation Strategy |
| :---- | :---- | :---- |
| **PF\_X (Executable)** | Contains machine code (.text) | **CRITICAL:** Use heap\_caps\_malloc(p\_memsz, MALLOC\_CAP\_EXEC) |
| **PF\_W (Writable)** | Contains data (.data, .bss) | Use heap\_caps\_malloc(p\_memsz, MALLOC\_CAP\_8BIT) |

**Implementation Requirement:** Do *not* use standard malloc(). You must use heap\_caps\_malloc to strictly enforce the IRAM/DRAM distinction. If MALLOC\_CAP\_EXEC fails (out of IRAM), the loader must abort immediately.

---

## **3\. Relocation Engine**

Because the ESP32 lacks an MMU to map virtual address 0x00 to physical RAM, binaries must be relocatable. The loader calculates the **Delta**:

C

int32\_t delta \= (int32\_t)actual\_allocated\_address \- (int32\_t)linked\_address\_in\_elf;

*Note: Linked address is typically 0x00 for Position Independent Executables (PIE).*

The loader parses the Relocation Tables (.rela.text, .rela.data) and handles two specific Xtensa relocation types:

### **3.1 R\_XTENSA\_RELATIVE (Type 17\)**

* **Target:** Pointers and data references (e.g., global variables referencing other globals).  
* **Action:** Simple addition.  
  C  
  uint32\_t \*addr \= (uint32\_t \*)(segment\_base \+ reloc\_entry-\>r\_offset);  
  \*addr \+= delta;

### **3.2 R\_XTENSA\_SLOT0\_OP (Type 20\)**

* **Target:** Machine instructions (e.g., CALL, J, L32R) where the target address is embedded in the opcode immediate field.  
* **Complexity:** The Xtensa instruction set uses variable-length instructions and packed bit fields.  
* **Action:** The loader must read the instruction word, extract the immediate value (often shifted), add the delta, and write it back.  
  * *Warning:* Incorrect handling here will cause jumps to land in random memory locations.

---

## **4\. Cache Coherency (Mandatory Safety Step)**

The ESP32 CPU uses separate Instruction Cache (I-Cache) and Data Cache (D-Cache).

**The Hazard:**

1. The loader writes machine code to IRAM using the **Data Bus**.  
2. These writes may sit in the **D-Cache** or Write Buffers and are not immediately visible to the I-Cache.  
3. When the CPU attempts to jump to the new code, the **Instruction Fetch** unit reads from the **I-Cache**.  
4. If the I-Cache contains stale data (or garbage), the CPU decodes invalid opcodes, triggering an IllegalInstruction panic.

The Solution:  
Before jumping to the entry point, the loader must synchronize the caches.  
**Implementation Logic:**

C

// 1\. Write code to IRAM...  
// 2\. Perform Relocations...

// 3\. MANDATORY CACHE SYNC  
if (spi\_flash\_cache\_enabled()) {  
    // Flush data cache to ensure code is written to physical RAM  
    esp\_cache\_msync((void \*)start\_addr, size, ESP\_CACHE\_MSYNC\_FLAG\_DIR\_C2M);  
      
    // Invalidate instruction cache to force fetching fresh code from RAM  
    // Note: On some ESP32 versions, this may require ROM functions or   
    // simply ensuring the region is refreshed.  
}

*Failure to include this step renders the loader unstable.*

---

## **5\. Dynamic Linking: Exported Symbols**

The loaded binary ("Guest") needs access to firmware functions ("Host") like printf, vTaskDelay, or gpio\_set\_level.

### **5.1 The tools/export\_symbols.py Script**

You must create a Python script that runs during the build process.

1. **Input:** The compiled firmware ELF (project.elf).  
2. **Logic:** Scans the symbol table (using nm or pyelftools).  
3. **Output:** Generates a C source file (export\_symbols.c) containing a lookup table.

### **5.2 Symbol Table Structure**

The generated structure should look like this:

C

typedef struct {  
    const char \*name;  
    void \*addr;  
} sym\_table\_t;

const sym\_table\_t exported\_symbols\[\] \= {  
    { "printf", (void\*)0x400D1234 },  
    { "vTaskDelay", (void\*)0x40085678 },  
    { "gpio\_set\_level", (void\*)0x400E9999 },  
    // ... auto-generated list  
};

### **5.3 Runtime Resolution**

When the loader encounters an undefined symbol in the Guest ELF (e.g., CALL printf), it:

1. Looks up the string "printf" in exported\_symbols.  
2. Retrieves the address 0x400D1234.  
3. Patches the CALL instruction in the Guest memory using the delta calculated from this absolute address.

---

## **6\. Implementation Checklist**

* \[ \] **Parser:** Correctly identifies PT\_LOAD vs ignored segments.
* \[ \] **Allocator:** Uses MALLOC\_CAP\_EXEC for text segments.
* \[ \] **Relocator:** Correctly calculates delta and handles R\_XTENSA\_SLOT0\_OP.
* \[ \] **Cache:** Explicitly flushes/invalidates cache before execution.
* \[ \] **Linker:** export\_symbols.py successfully generates a valid C struct.

---

## **7\. Using the Official Espressif ELF Loader (Recommended)**

Instead of implementing a custom loader from scratch, use the official `espressif/elf_loader` component:

### **7.1 Add Dependency**

In `main/idf_component.yml`:

```yaml
dependencies:
  espressif/elf_loader: "^1.1"
```

### **7.2 Basic Usage**

```c
#include "esp_elf.h"
#include "esp_log.h"

static const char *TAG = "elf_loader";

int load_and_run_elf(const char *filepath) {
    esp_elf_t elf;

    // 1. Initialize the ELF structure
    int ret = esp_elf_init(&elf);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to init ELF loader");
        return -1;
    }

    // 2. Read ELF file from filesystem
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open ELF file: %s", filepath);
        esp_elf_deinit(&elf);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t *elf_data = malloc(size);
    fread(elf_data, 1, size, f);
    fclose(f);

    // 3. Relocate ELF to executable memory
    ret = esp_elf_relocate(&elf, elf_data);
    free(elf_data);

    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to relocate ELF");
        esp_elf_deinit(&elf);
        return -1;
    }

    // 4. Execute with arguments
    char *argv[] = {"payload.elf", NULL};
    int argc = 1;
    ret = esp_elf_request(&elf, 0, argc, argv);

    ESP_LOGI(TAG, "ELF returned: %d", ret);

    // 5. Cleanup
    esp_elf_deinit(&elf);
    return ret;
}
```

---

## **8\. Compiling Guest ELF Payloads**

### **8.1 Compiler Flags**

Guest ELF files must be compiled with specific flags:

```bash
# Compile to object file
xtensa-esp32-elf-gcc \
    -mlongcalls \
    -fno-common \
    -ffunction-sections \
    -fdata-sections \
    -c main.c -o main.o

# Link as relocatable object (for dynamic linking)
xtensa-esp32-elf-ld \
    -r \
    -nostartfiles \
    -nodefaultlibs \
    -nostdlib \
    main.o -o payload.elf
```

### **8.2 Flag Explanations**

| Flag | Purpose |
|------|---------|
| `-mlongcalls` | Generate long call sequences for functions outside 512KB range |
| `-fno-common` | Put uninitialized globals in .bss (required for relocation) |
| `-r` | Output relocatable object (keep relocation entries) |
| `-nostartfiles` | Don't link standard startup code (we provide our own entry) |
| `-nostdlib` | Don't link standard library (resolved at runtime via symbols) |

### **8.3 Guest Application Structure**

```c
// apps/hello_world/main.c
// External functions resolved by symbol table at runtime
extern int printf(const char *fmt, ...);
extern void vTaskDelay(int ticks);

int main(int argc, char *argv[]) {
    printf("Hello from Guest ELF!\n");
    printf("argc = %d\n", argc);

    for (int i = 0; i < 5; i++) {
        printf("Tick %d\n", i);
        vTaskDelay(100);  // 100 ticks = 1 second (configTICK_RATE_HZ=100)
    }

    return 0;
}
```

### **8.4 CMake Integration for Guest Apps**

Create `apps/CMakeLists.txt`:

```cmake
# Build guest ELF applications
set(XTENSA_GCC xtensa-esp32-elf-gcc)
set(XTENSA_LD xtensa-esp32-elf-ld)

set(COMMON_CFLAGS -mlongcalls -fno-common -ffunction-sections -fdata-sections)
set(COMMON_LDFLAGS -r -nostartfiles -nodefaultlibs -nostdlib)

# Hello World payload
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/hello_world.elf
    COMMAND ${XTENSA_GCC} ${COMMON_CFLAGS} -c
            ${CMAKE_CURRENT_SOURCE_DIR}/hello_world/main.c
            -o ${CMAKE_BINARY_DIR}/hello_world.o
    COMMAND ${XTENSA_LD} ${COMMON_LDFLAGS}
            ${CMAKE_BINARY_DIR}/hello_world.o
            -o ${CMAKE_BINARY_DIR}/hello_world.elf
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/hello_world/main.c
)

add_custom_target(guest_apps ALL
    DEPENDS ${CMAKE_BINARY_DIR}/hello_world.elf
)
```

---

## **9\. The export\_symbols.py Script**

Create `tools/export_symbols.py`:

```python
#!/usr/bin/env python3
"""
Generate symbol table for ELF loader dynamic linking.
Extracts function addresses from firmware ELF and generates C source.
"""

import subprocess
import sys
import re

# Symbols to export (add more as needed)
EXPORT_SYMBOLS = [
    # Standard C library
    "printf", "sprintf", "snprintf", "puts", "putchar",
    "malloc", "free", "calloc", "realloc",
    "memcpy", "memset", "memmove", "memcmp",
    "strlen", "strcpy", "strncpy", "strcmp", "strncmp",
    "fopen", "fclose", "fread", "fwrite", "fseek", "ftell",

    # FreeRTOS
    "vTaskDelay", "xTaskCreate", "vTaskDelete",
    "xQueueCreate", "xQueueSend", "xQueueReceive",
    "xSemaphoreCreateMutex", "xSemaphoreTake", "xSemaphoreGive",

    # ESP-IDF
    "esp_log_write", "esp_log_timestamp",
    "gpio_set_level", "gpio_get_level", "gpio_set_direction",

    # Networking
    "socket", "bind", "listen", "accept", "connect",
    "send", "recv", "close", "setsockopt",

    # VFS
    "open", "read", "write", "lseek", "stat", "ioctl",
]

def get_symbol_address(elf_path, symbol):
    """Extract address of symbol from ELF using nm."""
    try:
        result = subprocess.run(
            ["xtensa-esp32-elf-nm", elf_path],
            capture_output=True, text=True
        )
        for line in result.stdout.split('\n'):
            parts = line.split()
            if len(parts) >= 3 and parts[2] == symbol:
                return int(parts[0], 16)
    except Exception as e:
        print(f"Warning: Could not find {symbol}: {e}", file=sys.stderr)
    return None

def generate_symbol_table(elf_path, output_path):
    """Generate C source with symbol table."""
    symbols = []

    for sym in EXPORT_SYMBOLS:
        addr = get_symbol_address(elf_path, sym)
        if addr:
            symbols.append((sym, addr))
            print(f"  Found: {sym} @ 0x{addr:08X}")
        else:
            print(f"  Missing: {sym}")

    with open(output_path, 'w') as f:
        f.write("// Auto-generated symbol table - DO NOT EDIT\n")
        f.write("#include <stddef.h>\n\n")
        f.write("typedef struct {\n")
        f.write("    const char *name;\n")
        f.write("    void *addr;\n")
        f.write("} exported_symbol_t;\n\n")
        f.write(f"const exported_symbol_t exported_symbols[] = {{\n")

        for name, addr in symbols:
            f.write(f'    {{ "{name}", (void*)0x{addr:08X} }},\n')

        f.write("    { NULL, NULL }  // Sentinel\n")
        f.write("};\n\n")
        f.write(f"const size_t exported_symbols_count = {len(symbols)};\n")

    print(f"\nGenerated {output_path} with {len(symbols)} symbols")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <firmware.elf> <output.c>")
        sys.exit(1)

    generate_symbol_table(sys.argv[1], sys.argv[2])
```

### **9.1 Running the Script**

```bash
# After building firmware
python tools/export_symbols.py build/linux_compat_layer.elf main/export_symbols.c

# Rebuild to include generated file
idf.py build
```

---

## **10\. Xtensa Relocation Reference**

### **10.1 Common Relocation Types**

| Type | Value | Description | Handling |
|------|-------|-------------|----------|
| `R_XTENSA_NONE` | 0 | No relocation | Skip |
| `R_XTENSA_32` | 1 | Direct 32-bit | `*addr += delta` |
| `R_XTENSA_RTLD` | 2 | Runtime linker | Skip (handled by loader) |
| `R_XTENSA_GLOB_DAT` | 3 | Global data | Resolve symbol, set address |
| `R_XTENSA_JMP_SLOT` | 4 | PLT entry | Resolve symbol, patch jump |
| `R_XTENSA_RELATIVE` | 5 | Relative | `*addr += load_base` |
| `R_XTENSA_SLOT0_OP` | 20 | Instruction operand | Extract, modify, insert immediate |

### **10.2 R\_XTENSA\_SLOT0\_OP Implementation**

This relocation modifies the immediate field of Xtensa instructions. The Xtensa ISA uses variable-length instructions (2 or 3 bytes) with complex encoding.

```c
// Simplified handler (actual implementation is complex)
void handle_slot0_op(uint8_t *inst_addr, int32_t delta, int reloc_type) {
    // Read instruction bytes (may be 2 or 3 bytes)
    uint32_t inst = inst_addr[0] | (inst_addr[1] << 8);
    if ((inst & 0x8) == 0) {  // 3-byte instruction
        inst |= (inst_addr[2] << 16);
    }

    // Extract and modify immediate based on instruction type
    // This requires knowledge of specific opcode formats
    // The official Espressif loader handles this automatically

    // Write back modified instruction
    inst_addr[0] = inst & 0xFF;
    inst_addr[1] = (inst >> 8) & 0xFF;
    if ((inst & 0x8) == 0) {
        inst_addr[2] = (inst >> 16) & 0xFF;
    }
}
```

**Recommendation:** Use the official `espressif/elf_loader` which handles all Xtensa relocations correctly.

---

## **11\. Memory Layout Visualization**

```
ESP32 Memory Map (Simplified)
=============================

0x3FFB_0000 ┌────────────────────┐
            │   DRAM (Data RAM)  │  <- .data, .bss, heap
            │   320 KB           │  <- Use MALLOC_CAP_8BIT
0x3FFF_FFFF └────────────────────┘

0x4007_0000 ┌────────────────────┐
            │   IRAM (Inst RAM)  │  <- .text (executable code)
            │   ~200 KB          │  <- Use MALLOC_CAP_EXEC
0x400A_FFFF └────────────────────┘

0x400C_0000 ┌────────────────────┐
            │   Flash Cache      │  <- Firmware code (read-only)
0x400F_FFFF └────────────────────┘

Guest ELF Loading:
─────────────────
1. .text  → IRAM (MALLOC_CAP_EXEC)
2. .data  → DRAM (MALLOC_CAP_8BIT)
3. .bss   → DRAM (MALLOC_CAP_8BIT, zeroed)
4. Apply relocations with delta = actual_addr - linked_addr
5. Flush cache
6. Jump to entry point
```

---

## **12\. Testing the ELF Loader**

### **12.1 Minimal Test Payload**

```c
// apps/test_minimal/main.c
extern int printf(const char *fmt, ...);

int main(void) {
    printf("ELF Loader Test: SUCCESS\n");
    return 42;  // Return value can be checked
}
```

### **12.2 Expected Output**

```
I (xxx) elf_loader: Loading /linux/test_minimal.elf
I (xxx) elf_loader: Relocating 3 segments
I (xxx) elf_loader: Text: 0x40080000, Data: 0x3FFB0000
I (xxx) elf_loader: Starting execution...
ELF Loader Test: SUCCESS
I (xxx) elf_loader: ELF returned: 42
```