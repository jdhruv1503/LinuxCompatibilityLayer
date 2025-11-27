# ESP-IDF QEMU Reference Guide

## Overview

Espressif maintains a fork of QEMU with ESP32/ESP32-S2/ESP32-S3/ESP32-C3 support. This document covers installation and usage for ESP32 emulation.

## Installation

### Via ESP-IDF Tools

```bash
# Install QEMU through idf_tools.py
python $IDF_PATH/tools/idf_tools.py install qemu-xtensa qemu-riscv32

# Verify installation
python $IDF_PATH/tools/idf_tools.py list --installed | grep qemu
```

### Installation Paths (Windows)

After installation, QEMU binaries are located at:
```
C:\Users\<USER>\.espressif\tools\qemu-xtensa\esp_develop_9.0.0_20240606\qemu\bin\
├── qemu-system-xtensa.exe      # For ESP32, ESP32-S2, ESP32-S3
└── qemu-img.exe                # Image manipulation utility

C:\Users\<USER>\.espressif\tools\qemu-riscv32\esp_develop_9.0.0_20240606\qemu\bin\
└── qemu-system-riscv32.exe     # For ESP32-C3, ESP32-C6
```

## Creating Flash Images

QEMU requires a single merged flash binary containing all partitions:

### Using esptool merge_bin

```bash
python -m esptool --chip esp32 merge_bin \
    -o merged-flash.bin \
    --flash_mode dio \
    --flash_size 4MB \
    0x1000 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0xd000 ota_data_initial.bin \
    0x10000 <app>.bin \
    0x190000 <filesystem>.bin
```

### Padding to Exact Flash Size

QEMU may require the binary to match exact flash size:

```bash
# Check current size
stat -c%s merged-flash.bin

# Pad to 4MB (4194304 bytes)
dd if=/dev/zero bs=1 count=$((4194304 - $(stat -c%s merged-flash.bin))) >> merged-flash.bin
```

## Running QEMU

### Basic Command

```bash
qemu-system-xtensa \
    -nographic \
    -machine esp32 \
    -drive file=merged-flash.bin,if=mtd,format=raw
```

### Command Options

| Option | Description |
|--------|-------------|
| `-nographic` | Disable GUI, output serial to terminal |
| `-machine esp32` | Select ESP32 machine type |
| `-machine esp32s2` | Select ESP32-S2 machine type |
| `-machine esp32s3` | Select ESP32-S3 machine type |
| `-drive file=X,if=mtd,format=raw` | Attach flash as MTD device |
| `-no-reboot` | Exit on reset instead of rebooting |
| `-s` | Start GDB server on port 1234 |
| `-S` | Start paused, wait for GDB connection |
| `-serial stdio` | Serial port to stdio (default with -nographic) |
| `-serial tcp::5555,server,nowait` | Serial port to TCP socket |

### Machine Types

- `esp32` - ESP32 (Xtensa LX6)
- `esp32s2` - ESP32-S2 (Xtensa LX7)
- `esp32s3` - ESP32-S3 (Xtensa LX7 dual-core)
- `esp32c3` - ESP32-C3 (RISC-V) - use `qemu-system-riscv32`

### Networking (Future Use)

QEMU supports simulated WiFi through an internal access point:

```bash
qemu-system-xtensa \
    -nographic \
    -machine esp32 \
    -drive file=merged-flash.bin,if=mtd,format=raw \
    -nic user,model=open_eth,id=net0,hostfwd=tcp::8080-:80
```

The `-nic user` option:
- Creates NAT network
- `-hostfwd=tcp::8080-:80` forwards host:8080 to guest:80

## Debugging with GDB

### Start QEMU with GDB Server

```bash
qemu-system-xtensa \
    -nographic \
    -machine esp32 \
    -drive file=merged-flash.bin,if=mtd,format=raw \
    -s -S
```

### Connect GDB

```bash
xtensa-esp32-elf-gdb build/<app>.elf
(gdb) target remote :1234
(gdb) continue
```

## Limitations

1. **No Real WiFi/Bluetooth**: Hardware WiFi/BT not emulated; use `-nic user` for basic networking
2. **Timing Differences**: Cycle-accurate timing not guaranteed
3. **Peripheral Support**: Not all peripherals fully emulated
4. **Flash Size**: Binary must match expected flash size exactly

## Comparison: QEMU vs Wokwi

| Feature | QEMU | Wokwi |
|---------|------|-------|
| Installation | Local (idf_tools.py) | CLI or cloud |
| Speed | Fast | Moderate |
| Peripherals | Limited | Extensive (LEDs, sensors, etc.) |
| Networking | User-mode NAT | Simulated WiFi ("Wokwi-GUEST") |
| GDB Debug | Yes | Yes (via GDB stub) |
| Visual | None | Web-based circuit view |
| CI/CD | Excellent | Good |
| Reliability | Stable | Depends on network/CLI |

## References

- [Espressif QEMU Fork](https://github.com/espressif/qemu)
- [ESP-IDF QEMU Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/qemu.html)
- [esptool merge_bin](https://docs.espressif.com/projects/esptool/en/latest/esp32/esptool/advanced-commands.html#merge-bin)
