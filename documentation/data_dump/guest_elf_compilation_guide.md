# Guest ELF Compilation Guide

This document details how to compile C programs into guest ELF binaries that can be loaded and executed by the ESP32 ELF Loader.

---

## 1. Toolchain Requirements

### 1.1 Xtensa ESP32 Toolchain

| Tool | Path (Windows) |
|------|----------------|
| GCC | `C:\Users\Dhruv\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin\xtensa-esp32-elf-gcc.exe` |
| LD | `C:\Users\Dhruv\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin\xtensa-esp32-elf-ld.exe` |
| Strip | `C:\Users\Dhruv\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin\xtensa-esp32-elf-strip.exe` |
| Objdump | `C:\Users\Dhruv\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin\xtensa-esp32-elf-objdump.exe` |
| Readelf | `C:\Users\Dhruv\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin\xtensa-esp32-elf-readelf.exe` |

---

## 2. Compilation Flags

### 2.1 Required Compiler Flags (CFLAGS)

```
-mlongcalls      # Allow long-range function calls (required for Xtensa)
-fno-common      # Don't merge common symbols (prevents relocation issues)
-ffunction-sections  # Place each function in own section (for --gc-sections)
-fdata-sections      # Place each data item in own section
-fPIC                # Position Independent Code (REQUIRED)
-Os                  # Optimize for size
```

### 2.2 Required Linker Flags (LDFLAGS)

```
-nostartfiles    # Don't use standard startup files (we have app_main)
-nostdlib        # Don't link standard library automatically
-fPIC            # Position Independent Code
-shared          # Create shared object (ET_DYN type 3)
-Wl,--gc-sections    # Remove unused sections
-Wl,-z,now           # Resolve all symbols at load time
-e app_main          # Set entry point to app_main
```

### 2.3 Why These Flags?

| Flag | Why Required |
|------|--------------|
| `-fPIC` | ELF loader cannot handle absolute addresses (no MMU) |
| `-shared` | Creates ET_DYN (type 3) instead of ET_REL (type 1) |
| `-e app_main` | ESP-IDF convention; loader expects this entry point |
| `-nostdlib` | Libc functions must be resolved against firmware exports |
| `-mlongcalls` | Xtensa ISA requires this for code > 256KB away |

---

## 3. Entry Point Requirements

### 3.1 Function Signature

```c
int app_main(int argc, char *argv[]);
```

### 3.2 Visibility Attribute

The entry point MUST be exported with default visibility:

```c
__attribute__((visibility("default")))
int app_main(int argc, char *argv[])
{
    // Your code here
    return 0;
}
```

Without `visibility("default")`, the symbol won't appear in the dynamic symbol table and the ELF loader won't find it.

### 3.3 Alternative: Use -fvisibility=default

Instead of per-function attributes, compile with:
```
-fvisibility=default
```

This exports all functions by default.

---

## 4. Minimal Example

### 4.1 Source Code (main.c)

```c
/**
 * @file main.c
 * @brief Minimal guest ELF for ESP32
 */

__attribute__((visibility("default")))
int app_main(int argc, char *argv[])
{
    return 42;  // Return value captured by host
}
```

### 4.2 Build Commands

```batch
set XTENSA_BIN=C:\Users\Dhruv\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin

REM Compile
%XTENSA_BIN%\xtensa-esp32-elf-gcc.exe ^
    -mlongcalls -fno-common -ffunction-sections -fdata-sections -fPIC -Os ^
    -c main.c -o main.o

REM Link
%XTENSA_BIN%\xtensa-esp32-elf-gcc.exe ^
    -nostartfiles -nostdlib -fPIC -shared ^
    -Wl,--gc-sections -Wl,-z,now ^
    -e app_main ^
    main.o -o hello_world.elf

REM Optional: Strip unnecessary sections
%XTENSA_BIN%\xtensa-esp32-elf-strip.exe ^
    --strip-unneeded ^
    --remove-section=.comment ^
    --remove-section=.xt.lit ^
    --remove-section=.xt.prop ^
    --remove-section=.xtensa.info ^
    hello_world.elf
```

---

## 5. Using the Build Script

The project includes `tools/build_guest_app.bat`:

```batch
cd C:\Users\Dhruv\Documents\Projects\LinuxCompatibilityLayer
tools\build_guest_app.bat hello_world
```

This:
1. Compiles `apps/hello_world/main.c`
2. Links to `build/guest_apps/hello_world.elf`
3. Copies to `data/hello_world.elf` (for LittleFS inclusion)

---

## 6. ELF Verification

### 6.1 Check ELF Type

```batch
xtensa-esp32-elf-readelf -h hello_world.elf
```

Expected output:
```
ELF Header:
  Class:                             ELF32
  Data:                              2's complement, little endian
  Type:                              DYN (Shared object file)    <-- MUST be DYN
  Machine:                           Tensilica Xtensa             <-- MUST be Xtensa
  Entry point address:               0x130                        <-- Non-zero
```

### 6.2 Check Exported Symbols

```batch
xtensa-esp32-elf-nm -D hello_world.elf
```

Expected output:
```
00001180 B __bss_start
00001180 D _edata
00001184 B _end
00000130 T app_main    <-- MUST be present
```

### 6.3 Check Sections

```batch
xtensa-esp32-elf-objdump -h hello_world.elf
```

Expected sections:
```
Sections:
Idx Name          Size      VMA       LMA       File off  Algn
  0 .hash         00000028  00000094  00000094  00000094  2**2
  1 .dynsym       00000050  000000bc  000000bc  000000bc  2**2
  2 .dynstr       00000022  0000010c  0000010c  0000010c  2**0
  3 .text         00000007  00000130  00000130  00000130  2**2    <-- Code
  4 .got          00000004  00001180  00001180  00000138  2**2
```

---

## 7. Common Compilation Errors

### 7.1 "relocatable linking with relocations from format elf32-xtensa-le is not supported"

**Cause:** Using `-r` (relocatable) instead of `-shared`
**Fix:** Use `-shared` in LDFLAGS

### 7.2 "undefined reference to `printf`"

**Cause:** Using libc functions without `-nostdlib`
**Fix:** Add `-nostdlib` and ensure symbols are exported by firmware

### 7.3 "entry point not found"

**Cause:** Entry point not exported or wrong name
**Fix:** Use `__attribute__((visibility("default")))` and `-e app_main`

### 7.4 "ELF type 1 not supported"

**Cause:** Created ET_REL (relocatable) instead of ET_DYN (shared)
**Fix:** Use `-shared` flag, not `-r`

---

## 8. Size Optimization

### 8.1 Minimal ELF Size

A minimal "return 42" ELF is ~768 bytes after stripping.

### 8.2 Size Reduction Tips

1. Use `-Os` instead of `-O2`
2. Use `-ffunction-sections -fdata-sections` with `-Wl,--gc-sections`
3. Strip with `--strip-unneeded`
4. Remove debug sections: `--remove-section=.comment`
5. Remove Xtensa-specific sections: `--remove-section=.xt.lit --remove-section=.xt.prop`

---

## 9. Using C Standard Library Functions

### 9.1 Current Status (Task 02)

C stdlib functions (printf, malloc, etc.) are NOT available to guest ELFs until Task 03 is complete.

### 9.2 What Works Now

- Direct function calls that return values
- Integer arithmetic
- Register operations
- Accessing passed arguments (if you know the ABI)

### 9.3 What Doesn't Work

- `printf()` - needs stdout file descriptor
- `malloc()` / `free()` - needs heap setup
- `strlen()` / `strcpy()` - needs symbol export
- File I/O (`fopen`, `fread`) - needs syscall shims
- Any libc function

### 9.4 After Task 03

Once `CONFIG_ELF_LOADER_LIBC_SYMBOLS=y` is properly configured and syscall shims are implemented:

```c
#include <stdio.h>

__attribute__((visibility("default")))
int app_main(int argc, char *argv[])
{
    printf("Hello from guest ELF!\n");
    printf("Received %d arguments\n", argc);
    return 0;
}
```

---

## 10. Assembly Inspection

To understand what the compiler generates:

```batch
xtensa-esp32-elf-objdump -d hello_world.elf
```

Minimal app_main disassembly:
```asm
00000130 <app_main>:
 130:   36 41 00        entry   a1, 32
 133:   2a 0c           movi.n  a2, 42      ; Return value
 135:   1d f0           retw.n              ; Return
```

This is just 7 bytes of code (aligned to 8).
