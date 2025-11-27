# Host Symbol Mapping Reference

This document lists the critical symbol mappings established for the Linux Compatibility Layer.

## Filesystem (shim_unistd.c)

These functions handle path translation (`/` -> `/linux/`) and permission faking.

| Guest Symbol | Host Implementation | Notes |
|--------------|---------------------|-------|
| `open`       | `shim_open`         | Translates path, calls `open` |
| `close`      | `shim_close`        | Direct wrapper around `close` |
| `read`       | `shim_read`         | Direct wrapper around `read` |
| `write`      | `shim_write`        | Direct wrapper around `write` |
| `lseek`      | `shim_lseek`        | Direct wrapper around `lseek` |
| `stat`       | `shim_stat`         | **Fakes 0777 permissions** |
| `fstat`      | `shim_fstat`        | **Fakes 0777 permissions** |
| `access`     | `shim_access`       | Returns 0 if file exists (ignore mode) |
| `mkdir`      | `shim_mkdir`        | Translates path |
| `rmdir`      | `shim_rmdir`        | Translates path |
| `unlink`     | `shim_unlink`       | Translates path |
| `rename`     | `shim_rename`       | Translates both paths |
| `getcwd`     | `shim_getcwd`       | Returns internal cached CWD |
| `chdir`      | `shim_chdir`        | Updates internal cached CWD |
| `ioctl`      | `shim_ioctl`        | Passthrough (limited support) |

## Standard I/O

These are exported to support `FILE *` operations.

| Guest Symbol | Host Implementation | Notes |
|--------------|---------------------|-------|
| `fopen`      | `shim_fopen`        | **Translates path**, then calls host `fopen` |
| `fprintf`    | `fprintf`           | Direct export from libc |
| `fread`      | `fread`             | Direct export from libc |
| `fwrite`     | `fwrite`            | Direct export from libc |
| `fclose`     | `fclose`            | Direct export from libc |
| `fflush`     | `fflush`            | Direct export from libc |
| `fseek`      | `fseek`             | Direct export from libc |
| `ftell`      | `ftell`             | Direct export from libc |

## Build System Notes

*   **Linker Flags:** All `shim_*` functions must be forced into the binary using `-u symbol_name` in `CMakeLists.txt` because they are not called by the firmware main loop.
*   **Generation:** The symbol table is generated into `managed_components/espressif__elf_loader/src/esp_all_symbol.c` by `tools/export_symbols.py`.
