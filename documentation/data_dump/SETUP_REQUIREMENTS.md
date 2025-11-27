# Setup Requirements for Development Environment

## Prerequisites

### 1. ESP-IDF (Espressif IoT Development Framework)
- **Version Required:** v5.4+
- **Windows Path:** `C:\Users\<USER>\.esp-tools\esp-idf`
- **Installation:** Clone from GitHub or use ESP-IDF Tools Installer

**Manual Installation (Windows):**
```bash
mkdir C:\Users\%USERNAME%\.esp-tools
cd C:\Users\%USERNAME%\.esp-tools
git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git
```

### 2. Python 3.8+ (Full Installation)
- **Windows Path:** `C:\Users\<USER>\.esp-tools\python-full`
- **Important:** Use full Python installer, NOT embeddable package (venv module required)

### 3. Xtensa Toolchain
- Installed automatically via `idf_tools.py`
- Binary: `xtensa-esp32-elf-gcc`
- Required for compiling guest ELF payloads

### 4. QEMU-Xtensa (for simulation)

**Installation via ESP-IDF tools:**
```bash
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa
```

**Windows Path after installation:**
```
C:\Users\<USER>\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\qemu-system-xtensa.exe
```

### 5. esptool (for creating merged flash binaries)
- Included with ESP-IDF
- Used for `merge_bin` command

## Verification Commands

```bash
# Check ESP-IDF (must run through cmd.exe on Windows to avoid MSYSTEM issues)
cmd.exe //c "set MSYSTEM= && C:\\Users\\%USERNAME%\\.esp-tools\\esp-idf\\export.bat && idf.py --version"

# Check Xtensa toolchain
xtensa-esp32-elf-gcc --version

# Check QEMU
qemu-system-xtensa --version

# Check Python and pyelftools
python -c "import elftools; print('pyelftools OK')"
```

## Project Build & Simulation

### Build the Project
```bash
# On Windows, use cmd.exe wrapper to avoid MSYSTEM issues
cmd.exe //c "set MSYSTEM= && C:\\Users\\%USERNAME%\\.esp-tools\\esp-idf\\export.bat && cd /d PROJECT_PATH && idf.py build"
```

### Create Merged Flash Binary for QEMU
```bash
cd build
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

### Run in QEMU
```bash
qemu-system-xtensa -nographic -machine esp32 \
    -drive file=build/merged-flash.bin,if=mtd,format=raw \
    -no-reboot
```

### QEMU with Networking (for C2 demos)
```bash
qemu-system-xtensa -nographic -machine esp32 \
    -drive file=build/merged-flash.bin,if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=tcp::9000-:9000 \
    -no-reboot
```

## Common Issues

### "esp-idf not supported in MSYS2"
ESP-IDF scripts detect Git Bash/MSYS2 and refuse to run. Solution: Clear MSYSTEM variable.
```batch
set MSYSTEM=
```

### "venv module not found"
Using Python embeddable package. Install full Python distribution instead.

### "Failed to find LittleFS partition"
Ensure `partitions.csv` has the `linux_fs` partition and matches `main.c` partition label.

### "InstrFetchProhibited" panic
Code was allocated in DRAM instead of IRAM. Use `heap_caps_malloc(..., MALLOC_CAP_EXEC)`.

### QEMU shows no output
- Ensure `-nographic` flag is used
- Check that merged-flash.bin is exactly 4MB (pad with zeros if needed)

### QEMU networking not working
- Use `-nic user,model=open_eth` for NAT networking
- Connect to localhost with port forwarding instead of ESP32 IP

## Windows-Specific Notes

1. **Use cmd.exe for ESP-IDF commands** - Git Bash sets MSYSTEM which breaks ESP-IDF
2. **Install full Python** - Embeddable package lacks venv module
3. **Use forward slashes in paths** when running commands in Git Bash
4. **QEMU binary location**: `C:\Users\<USER>\.espressif\tools\qemu-xtensa\...\qemu\bin\`
