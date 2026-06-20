#!/usr/bin/env python3
"""
Generate symbol table for ELF loader dynamic linking.

This script extracts function addresses from the firmware ELF and generates
a C source file that can be used to provide additional symbols to the ELF loader.
"""

import subprocess
import sys
import os

# Symbols to export. Can be a string "name" or a tuple ("shim_name", "guest_name").
EXPORT_SYMBOLS = [
    # Filesystem shims
    ("shim_open", "open"),
    ("shim_read", "read"),
    ("shim_write", "write"),
    ("shim_close", "close"),
    ("shim_lseek", "lseek"),
    ("shim_ioctl", "ioctl"),
    ("shim_fopen", "fopen"),
    ("shim_printf", "printf"),  # Intercept printf for C2 stdout redirection
    ("shim_puts", "puts"),      # Intercept puts for C2 stdout redirection
    ("shim_fgets", "fgets"),    # Intercept fgets for C2 stdin redirection (if needed)
    "fprintf",
    "fclose",
    "fread",
    "fwrite",
    "fflush",
    "fseek",
    "ftell",
    "feof",
    "ferror",
    "clearerr",
    "setvbuf",
    ("shim_stat", "stat"),
    ("shim_fstat", "fstat"),
    ("shim_access", "access"),
    ("shim_unlink", "unlink"),
    ("shim_rename", "rename"),
    ("shim_mkdir", "mkdir"),
    ("shim_rmdir", "rmdir"),
    ("shim_opendir", "opendir"),
    ("shim_readdir", "readdir"),
    ("shim_closedir", "closedir"),
    ("shim_getcwd", "getcwd"),
    ("shim_chdir", "chdir"),

    # File descriptor duplication (for stdout redirection)
    ("shim_dup", "dup"),
    ("shim_dup2", "dup2"),
    ("shim_dup3", "dup3"),

    # Network (socket shims)
    ("shim_socket", "socket"),
    ("shim_bind", "bind"),
    ("shim_listen", "listen"),
    ("shim_accept", "accept"),
    ("shim_connect", "connect"),
    ("shim_send", "send"),
    ("shim_sendto", "sendto"),
    ("shim_recv", "recv"),
    ("shim_recvfrom", "recvfrom"),
    ("shim_shutdown", "shutdown"),
    ("shim_setsockopt", "setsockopt"),
    ("shim_getsockopt", "getsockopt"),
    ("shim_select", "select"),
    ("shim_poll", "poll"),
    ("shim_gethostbyname", "gethostbyname"),
    ("shim_getaddrinfo", "getaddrinfo"),
    ("shim_freeaddrinfo", "freeaddrinfo"),

    # Process (stub/spawn shims)
    ("shim_getpid", "getpid"),
    ("shim_getppid", "getppid"),
    ("shim_fork", "fork"),
    ("shim_execve", "execve"),
    ("shim_execv", "execv"),
    ("shim_waitpid", "waitpid"),
    ("shim_wait", "wait"),
    ("shim_exit", "exit"),
    ("shim__exit", "_exit"),
    ("shim_abort", "abort"),
    ("shim_signal", "signal"),
    ("shim_raise", "raise"),
    ("shim_kill", "kill"),

    # Time shims
    ("shim_clock_gettime", "clock_gettime"),
    ("shim_gettimeofday", "gettimeofday"),
    ("shim_nanosleep", "nanosleep"),
    ("shim_usleep", "usleep"),
    ("shim_sleep", "sleep"),
    ("shim_times", "times"),
    ("shim_alarm", "alarm"),
    ("shim_clock", "clock"),
    ("shim_clock_getres", "clock_getres"),

    # Memory management
    "malloc",
    "free",
    "calloc",
    "realloc",

    # C2 Pipe VFS driver functions
    "vfs_c2_pipe_register",
    "c2_pipe_set_socket",
    "c2_pipe_get_socket",
    "c2_pipe_set_mirror",
    "c2_pipe_is_active",
]

# FreeRTOS symbols that might be useful
FREERTOS_SYMBOLS = [
    "vTaskDelay",
    "xTaskCreate",
    "vTaskDelete",
    "xTaskGetCurrentTaskHandle",
    "xQueueCreate",
    "xQueueSend",
    "xQueueReceive",
    "xSemaphoreCreateMutex",
    "xSemaphoreTake",
    "xSemaphoreGive",
    "pvPortMalloc",
    "vPortFree",
]

# GPIO symbols
GPIO_SYMBOLS = [
    "gpio_set_level",
    "gpio_get_level",
    "gpio_set_direction",
    "gpio_config",
    "gpio_reset_pin",
]

# ESP logging
LOG_SYMBOLS = [
    "esp_log_write",
    "esp_log_timestamp",
]

# Standard C string functions
STRING_SYMBOLS = [
    "strlen",
    "strcpy",
    "strncpy",
    "strcat",
    "strncat",
    "strcmp",
    "strncmp",
    "strchr",
    "strrchr",
    "strstr",
    "memset",
    "memcpy",
    "memmove",
    "memcmp",
    "strtod",
    "strtol",
    "strtoul",
    "atoi",
    "atol",
    "sprintf",
    "snprintf",
    "sscanf",
]

# libgcc soft-float helpers (needed for double precision operations in guest ELFs)
LIBGCC_SYMBOLS = [
    "__floatsidf",    # int to double
    "__extendsfdf2",  # float to double
    "__adddf3",       # double addition
    "__divdf3",       # double division
    "__truncdfsf2",   # double to float
    "__muldf3",       # double multiplication
    "__subdf3",       # double subtraction
    "__fixdfsi",      # double to int
    "__floatunsidf",  # unsigned int to double
    "__gedf2",        # double compare >=
    "__ledf2",        # double compare <=
    "__ltdf2",        # double compare <
    "__gtdf2",        # double compare >
    "__eqdf2",        # double compare ==
    "__nedf2",        # double compare !=
    "__divsf3",       # single precision division
    "__addsf3",       # single precision addition
    "__subsf3",       # single precision subtraction
    "__mulsf3",       # single precision multiplication
]

# Math symbols (single and double precision)
MATH_SYMBOLS = [
    "sinf", "cosf", "tanf", "asinf", "acosf", "atanf", "atan2f",
    "sqrtf", "powf", "expf", "logf", "log10f", "fabsf", "floorf", "ceilf", "fmodf", "roundf",
    "sin", "cos", "tan", "asin", "acos", "atan", "atan2",
    "sqrt", "pow", "exp", "log", "log10", "fabs", "floor", "ceil", "fmod", "round",
]


def find_nm_tool():
    """Find the appropriate nm tool for Xtensa."""
    candidates = [
        "xtensa-esp32-elf-nm",
        "xtensa-esp-elf-nm",
        # Add Windows specific path
        r"C:\Users\Dhruv\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20241119\xtensa-esp-elf\bin\xtensa-esp32-elf-nm.exe",
        os.path.expanduser("~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp-elf-nm"),
    ]

    for candidate in candidates:
        if '*' in candidate:
            import glob
            matches = glob.glob(candidate)
            if matches:
                return matches[0]
        else:
            try:
                result = subprocess.run([candidate, "--version"],
                                       capture_output=True, timeout=5)
                if result.returncode == 0:
                    return candidate
            except (FileNotFoundError, subprocess.TimeoutExpired):
                continue
    return None


def get_symbol_address(elf_path, symbol, nm_tool="xtensa-esp32-elf-nm"):
    """Extract address of symbol from ELF using nm."""
    try:
        result = subprocess.run(
            [nm_tool, elf_path],
            capture_output=True,
            text=True,
            timeout=30
        )
        for line in result.stdout.split('\n'):
            parts = line.split()
            if len(parts) >= 3 and parts[2] == symbol:
                return int(parts[0], 16)
    except Exception as e:
        print(f"Warning: Could not find {symbol}: {e}", file=sys.stderr)
    return None


def generate_symbol_table(elf_path, output_path, symbols_to_export=None):
    """Generate C source with symbol table."""

    nm_tool = find_nm_tool()
    if not nm_tool:
        print("Error: Could not find xtensa-esp32-elf-nm or xtensa-esp-elf-nm", file=sys.stderr)
        sys.exit(1)

    print(f"Using nm tool: {nm_tool}")

    if symbols_to_export is None:
        symbols_to_export = EXPORT_SYMBOLS + FREERTOS_SYMBOLS + GPIO_SYMBOLS + LOG_SYMBOLS + STRING_SYMBOLS + LIBGCC_SYMBOLS + MATH_SYMBOLS

    # Normalize to tuples: (real_name, export_name)
    normalized_symbols = []
    for item in symbols_to_export:
        if isinstance(item, tuple):
            normalized_symbols.append(item)
        else:
            normalized_symbols.append((item, item))

    # Remove duplicates
    # normalized_symbols = list(dict.fromkeys(normalized_symbols)) # Dict doesn't work with tuples cleanly if mixed

    found_symbols = []
    missing_symbols = []

    print(f"\nSearching for {len(normalized_symbols)} symbols in {elf_path}...")

    for real_name, export_name in normalized_symbols:
        addr = get_symbol_address(elf_path, real_name, nm_tool)
        if addr:
            found_symbols.append((real_name, export_name, addr))
            print(f"  Found: {real_name} -> {export_name} @ 0x{addr:08X}")
        else:
            missing_symbols.append(real_name)
            print(f"  Missing: {real_name}")

    # Generate the C file
    with open(output_path, 'w') as f:
        f.write("/*\n")
        f.write(" * Auto-generated symbol table for ELF loader\n")
        f.write(" * DO NOT EDIT - Generated by tools/export_symbols.py\n")
        f.write(" */\n\n")
        f.write("#include <stddef.h>\n")
        f.write("#include \"private/elf_symbol.h\"\n\n")

        # Add extern declarations
        f.write("/* Extern declarations */\n")
        declared = set()
        for real_name, _, _ in found_symbols:
            if real_name not in declared:
                f.write(f"extern void {real_name}(void);\n")
                declared.add(real_name)
        f.write("\n")

        f.write("/* Custom symbol table for ELF loader */\n")
        f.write("const struct esp_elfsym g_customer_elfsyms[] = {\n")

        for real_name, export_name, addr in found_symbols:
            if real_name == export_name:
                f.write(f'    ESP_ELFSYM_EXPORT({real_name}),\n')
            else:
                f.write(f'    {{ "{export_name}", &{real_name} }},\n')

        f.write("    ESP_ELFSYM_END\n")
        f.write("};\n")

    print(f"\nGenerated {output_path}")
    return len(found_symbols)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <firmware.elf> [output.c]")
        sys.exit(1)

    elf_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else "custom_symbols.c"

    if not os.path.exists(elf_path):
        print(f"Error: ELF file not found: {elf_path}", file=sys.stderr)
        sys.exit(1)

    generate_symbol_table(elf_path, output_path)