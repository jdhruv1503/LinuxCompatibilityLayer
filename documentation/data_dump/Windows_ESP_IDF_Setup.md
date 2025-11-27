# ESP-IDF Setup on Windows (Without Installer)

## Overview

This document covers manual ESP-IDF setup on Windows when using Git Bash or when the official installer isn't suitable. Key challenge: ESP-IDF scripts refuse to run in MSYS2/Git Bash environments.

## Prerequisites

1. **Git for Windows** (includes Git Bash)
2. **Python 3.8+** (full installation, not embeddable)

## Installation Steps

### 1. Create Tools Directory

```batch
mkdir C:\Users\%USERNAME%\.esp-tools
```

### 2. Install Python (Full Version)

**Important:** The Python "embeddable" package lacks the `venv` module required by ESP-IDF.

Download Python installer from https://www.python.org/downloads/ and install to:
```
C:\Users\<USERNAME>\.esp-tools\python-full
```

During installation:
- Check "Add to PATH" (optional, we'll manage manually)
- Check "pip"
- Check "tcl/tk" (for GUIs if needed)

### 3. Clone ESP-IDF

```bash
cd /c/Users/$USERNAME/.esp-tools
git clone -b v5.4 --recursive https://github.com/espressif/esp-idf.git
```

Or specific release:
```bash
git clone -b v5.4 --depth 1 https://github.com/espressif/esp-idf.git
cd esp-idf
git submodule update --init --recursive --depth 1
```

### 4. Install ESP-IDF Tools

**Critical:** Must run through `cmd.exe` to avoid MSYSTEM detection:

Create `install_esp.bat`:
```batch
@echo off
set MSYSTEM=
set IDF_PATH=C:\Users\%USERNAME%\.esp-tools\esp-idf
set PATH=C:\Users\%USERNAME%\.esp-tools\python-full;C:\Users\%USERNAME%\.esp-tools\python-full\Scripts;%PATH%
cd /d %IDF_PATH%
python tools\idf_tools.py install-python-env
python tools\idf_tools.py install
```

Run from Git Bash:
```bash
cmd.exe //c "C:\\Users\\$USERNAME\\.esp-tools\\install_esp.bat"
```

### 5. Install QEMU (for simulation)

```bash
cmd.exe //c "set MSYSTEM= && python C:\Users\%USERNAME%\.esp-tools\esp-idf\tools\idf_tools.py install qemu-xtensa"
```

## Build Script

Create `build_project.bat`:
```batch
@echo off
set MSYSTEM=
set IDF_PATH=C:\Users\%USERNAME%\.esp-tools\esp-idf
set PATH=C:\Users\%USERNAME%\.esp-tools\python-full;C:\Users\%USERNAME%\.esp-tools\python-full\Scripts;%PATH%
call "%IDF_PATH%\export.bat"
cd /d %1
idf.py build
```

Usage from Git Bash:
```bash
cmd.exe //c "C:\\Users\\$USERNAME\\.esp-tools\\build_project.bat C:\\path\\to\\project"
```

## The MSYSTEM Problem

ESP-IDF's `export.sh` and Windows scripts check for MSYS2 environment:

```bash
# From ESP-IDF scripts
if [ -n "$MSYSTEM" ]; then
    echo "WARNING: esp-idf.git is not supported in MSYS/MSYS2/Cygwin environment."
    return 1
fi
```

Git Bash sets `MSYSTEM=MINGW64`, triggering this check.

### Solutions

1. **Clear MSYSTEM** (recommended):
   ```batch
   set MSYSTEM=
   ```

2. **Use PowerShell** instead of Git Bash

3. **Use CMD directly** for ESP-IDF commands

## Directory Structure After Setup

```
C:\Users\<USERNAME>\
├── .esp-tools\
│   ├── esp-idf\              # ESP-IDF framework
│   │   ├── components\       # ESP-IDF components
│   │   ├── tools\            # Build tools, idf_tools.py
│   │   └── export.bat        # Environment setup
│   └── python-full\          # Python installation
│       ├── python.exe
│       ├── Scripts\
│       │   └── pip.exe
│       └── Lib\
└── .espressif\
    └── tools\                # Downloaded tools
        ├── xtensa-esp-elf\   # Toolchain
        ├── cmake\            # CMake
        ├── ninja\            # Ninja build
        └── qemu-xtensa\      # QEMU emulator
            └── esp_develop_9.0.0_20240606\
                └── qemu\bin\
                    └── qemu-system-xtensa.exe
```

## Environment Variables

Required for building:

| Variable | Value |
|----------|-------|
| `IDF_PATH` | `C:\Users\<USER>\.esp-tools\esp-idf` |
| `PATH` | Must include Python, toolchain, cmake, ninja |
| `MSYSTEM` | Must be empty/unset |

The `export.bat` script sets most of these automatically.

## Troubleshooting

### "Python not found"

Add Python to PATH before running ESP-IDF:
```batch
set PATH=C:\Users\%USERNAME%\.esp-tools\python-full;%PATH%
```

### "venv module not found"

Using embeddable Python. Install full Python distribution instead.

### "esp-idf not supported in MSYS2"

Clear MSYSTEM variable:
```batch
set MSYSTEM=
```

### "cmake not found"

Run `export.bat` first, or install cmake via idf_tools:
```bash
python $IDF_PATH/tools/idf_tools.py install cmake
```

### Build fails with encoding errors

Set UTF-8 encoding:
```batch
set PYTHONUTF8=1
chcp 65001
```

## Verifying Installation

```bash
# From Git Bash, check tools
cmd.exe //c "set MSYSTEM= && C:\\Users\\$USERNAME\\.esp-tools\\esp-idf\\export.bat && idf.py --version"

# Check QEMU
/c/Users/$USERNAME/.espressif/tools/qemu-xtensa/esp_develop_9.0.0_20240606/qemu/bin/qemu-system-xtensa.exe --version
```

## References

- [ESP-IDF Get Started](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- [ESP-IDF Tools](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-tools.html)
- [idf_tools.py Reference](https://github.com/espressif/esp-idf/blob/master/tools/idf_tools.py)
