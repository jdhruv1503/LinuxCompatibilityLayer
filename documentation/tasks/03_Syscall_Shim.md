## **0\. Prerequisites: Making printf() Work**

**CRITICAL:** Before diving into filesystem shims, you must first enable basic console output (`printf`). Without this, debugging guest ELFs is impossible.

### 0.1 Why printf Doesn't Work Out of the Box

After Task 02, guest ELFs can execute but cannot produce any output. Here's why:

```
printf("Hello") → Newlib printf → _write_r() → write(1, buf, len) → ???
```

The ELF loader exports Newlib's `printf`, but `printf` internally calls `write()` on file descriptor 1 (stdout). Without a working `write()` implementation that routes stdout to the UART, output is lost.

### 0.2 Minimal Console Output Implementation

**Step 1:** Create `main/syscalls/shim_console.c`:

```c
#include <stddef.h>
#include <stdint.h>
#include "esp_rom_sys.h"

/**
 * @brief Write to console (stdout/stderr)
 * This is the minimal syscall needed for printf to work.
 */
ssize_t shim_write(int fd, const void *buf, size_t count)
{
    // Route stdout (1) and stderr (2) to UART
    if (fd == 1 || fd == 2) {
        const char *p = (const char *)buf;
        for (size_t i = 0; i < count; i++) {
            esp_rom_printf("%c", p[i]);  // ROM printf outputs to UART
        }
        return count;
    }

    // For other file descriptors, use VFS (after implementing shim_open)
    // For now, return error for unknown fds
    return -1;
}
```

**Step 2:** Export the shim_write symbol to guest ELFs.

The ELF loader needs to resolve `write` to our `shim_write`. There are two approaches:

**Approach A: Symbol Alias (in firmware)**
```c
// Create a weak alias so guest ELF's "write" resolves to shim_write
ssize_t write(int fd, const void *buf, size_t count)
    __attribute__((weak, alias("shim_write")));
```

**Approach B: Custom Symbol Table (advanced)**
Configure `CONFIG_ELF_LOADER_CUSTOM_SYMBOL_TABLE` and provide a symbol table that maps "write" → &shim_write.

### 0.3 Testing printf

Create a test guest ELF (`apps/hello_printf/main.c`):

```c
#include <stdio.h>

__attribute__((visibility("default")))
int app_main(int argc, char *argv[])
{
    printf("Hello from guest ELF!\n");
    printf("argc = %d\n", argc);

    for (int i = 0; i < argc; i++) {
        printf("argv[%d] = %s\n", i, argv[i]);
    }

    return 0;
}
```

**Expected Output:**
```
I (xxx) kernel_main: Starting ELF execution...
Hello from guest ELF!
argc = 3
argv[0] = hello_printf.elf
argv[1] = arg1
argv[2] = arg2
I (xxx) kernel_main: ELF execution completed, return value: 0
```

### 0.4 Required sdkconfig Settings

Ensure these are in `sdkconfig.defaults`:

```ini
# Enable ELF loader
CONFIG_ELF_LOADER=y

# Export Newlib C library symbols (printf, malloc, strlen, etc.)
CONFIG_ELF_LOADER_LIBC_SYMBOLS=y

# Export ESP-IDF symbols (optional, for ESP_LOGI, gpio, etc.)
CONFIG_ELF_LOADER_ESPIDF_SYMBOLS=y
```

---

## **1\. Overview: The VFS Translation Layer**

The ESP32 does not run a native Linux kernel, so it lacks a monolithic "system call" interrupt handler (like int 0x80 or syscall). Instead, guest applications (ELFs) function as "Unikernel" tasks that share the address space with the firmware.

When a guest application calls a POSIX function like open(), the ELF Loader resolves this symbol to a function pointer in our firmware. We cannot point this directly to the internal C library functions because:

1. **Pathing Differences:** Linux apps expect root-relative paths (e.g., /var/log.txt), but ESP-IDF requires mount-point prefixed paths (e.g., /littlefs/var/log.txt).
2. **Permission Stubs:** ESP-IDF filesystems often lack user/group permission bits, which can confuse Linux logic checking for \+x or rw rights.
3. **Errno Handling:** We must ensure thread-safe propagation of error codes between the VFS and the guest's expectation.

This document details the creation of shim\_unistd.c, which acts as the bridge between the Guest (Linux API) and the Host (ESP-IDF VFS).

---

## **2\. Implementation: shim\_unistd.c**

Create a file named main/syscalls/shim\_unistd.c. This file will implement the "Shim" functions that are exported to the guest.

### **2.1 Include Dependencies**

You need access to the ESP-IDF VFS and standard headers.

C

\#**include** \<stdio.h\>  
\#**include** \<string.h\>  
\#**include** \<sys/stat.h\>  
\#**include** \<fcntl.h\>  
\#**include** \<errno.h\>  
\#**include** "esp\_vfs.h"  
\#**include** "esp\_log.h"

### **2.2 The Path Translation Problem**

The guest application views the world as starting from /. However, our LittleFS partition is mounted at /linux (or whatever was defined in main.c).

Requirement:  
The shim must detect if a path starts with / and prepend the mount point. If the path is relative (does not start with /), it should be treated as relative to the current working directory (which we generally assume is the mount root for simple payloads).  
**Helper Function Logic:**

C

\#**define** MOUNT\_POINT "/linux"

static void translate\_path(const char \*input\_path, char \*output\_buffer, size\_t max\_len) {  
    if (input\_path\[0\] \== '/') {  
        // Absolute path: map "/log.txt" \-\> "/linux/log.txt"  
        snprintf(output\_buffer, max\_len, "%s%s", MOUNT\_POINT, input\_path);  
    } else {  
        // Relative path: map "log.txt" \-\> "/linux/log.txt" (assuming CWD is root)  
        snprintf(output\_buffer, max\_len, "%s/%s", MOUNT\_POINT, input\_path);  
    }  
}

---

## **3\. Syscall Mappings**

### **3.1 shim\_open**

Maps open(path, flags, mode) to esp\_vfs\_open.

**Critical Implementation Details:**

1. **Translate Path:** Use the helper above.  
2. **Flag Translation:** ESP-IDF uses standard Newlib flags (O\_RDONLY, O\_CREAT, etc.), so direct mapping is usually safe. However, ensure O\_CREAT is handled correctly with the mode.  
3. **Return Value:** Returns a file descriptor (int). Errors must return \-1 and set errno.

C

int shim\_open(const char \*path, int flags, mode\_t mode) {  
    char real\_path\[128\];  
    translate\_path(path, real\_path, sizeof(real\_path));  
      
    int fd \= esp\_vfs\_open(real\_path, flags, mode);  
    if (fd \< 0) {  
        // esp\_vfs\_open sets errno internally, which matches guest expectations  
        return \-1;  
    }  
    return fd;  
}

### **3.2 shim\_read / shim\_write / shim\_close / shim\_lseek**

These are pass-through wrappers. Since the File Descriptor (FD) returned by shim\_open is a valid ESP-IDF VFS descriptor, we can pass it directly to the backend functions.

* shim\_write \-\> esp\_vfs\_write  
* shim\_read \-\> esp\_vfs\_read  
* shim\_close \-\> esp\_vfs\_close  
* shim\_lseek \-\> esp\_vfs\_lseek

### **3.3 shim\_stat & Permission Faking**

Linux apps often check if a file is readable/writable using stat. LittleFS on ESP32 is flat and lacks complex user permissions.

Requirement:  
We must "lie" to the guest application. Always report that files are readable, writable, and executable by everyone (0777).

C

int shim\_stat(const char \*path, struct stat \*st) {  
    char real\_path\[128\];  
    translate\_path(path, real\_path, sizeof(real\_path));

    int ret \= esp\_vfs\_stat(real\_path, st);  
    if (ret \== 0) {  
        // Override permissions to "rwxrwxrwx" (0777)  
        // This prevents Linux apps from failing "permission denied" checks  
        st-\>st\_mode |= 0777;  
    }  
    return ret;  
}

---

## **4\. Exporting Symbols**

For these shims to be usable by the Guest ELF, they must be added to the Symbol Table (refer to 02\_ELF\_Loader\_Core.md).

**Update tools/export\_symbols.py (or your manual list) to include:**

* open \-\> \&shim\_open  
* close \-\> \&shim\_close  
* read \-\> \&shim\_read  
* write \-\> \&shim\_write  
* lseek \-\> \&shim\_lseek  
* stat \-\> \&shim\_stat

*Note: If the guest uses fopen/fprintf, the standard C library in the guest will eventually call open/write. By intercepting the low-level calls, we support both raw syscalls and high-level FILE\* operations.*

---

## **5\. Verification: The "Hello FS" Test**

### **5.1 Create the Test Payload**

Create a simple C file apps/test\_fs/main.c:

C

\#**include** \<stdio.h\>  
\#**include** \<fcntl.h\>  
\#**include** \<unistd.h\>  
\#**include** \<string.h\>

int main() {  
    printf("Guest: Attempting to write to filesystem...\\n");

    // 1\. Use high-level fopen (which calls shim\_open)  
    FILE \*f \= fopen("/guest\_log.txt", "w");  
    if (\!f) {  
        printf("Guest: fopen failed\!\\n");  
        return 1;  
    }  
    fprintf(f, "Hello from the Guest ELF via Shim\!\\n");  
    fclose(f);

    // 2\. Verification via low-level open  
    int fd \= open("/guest\_log.txt", O\_RDONLY);  
    if (fd \< 0) {  
        printf("Guest: open failed\!\\n");  
        return 1;  
    }  
      
    char buffer\[64\];  
    int len \= read(fd, buffer, sizeof(buffer)\-1);  
    buffer\[len\] \= '\\0';  
    close(fd);

    printf("Guest: Read back \-\> '%s'\\n", buffer);  
    return 0;  
}

### **5.2 Compile the Payload**

Use the Xtensa toolchain. We compile without the standard system start files (-nostartfiles) but **with** the C library, trusting the linker to resolve open to our exported symbol (if using dynamic linking) or patching it at load time.

*Assumption: The Loader environment allows resolving undefined symbols in the ELF against the Host symbol table.*

Bash

xtensa-esp32-elf-gcc \-mlongcalls \-fno-common \-c main.c \-o main.o  
xtensa-esp32-elf-ld \-r main.o \-o payload.elf 

*(Note: If strict dynamic linking is set up, the guest ELF should be linked with \-shared or as a relocatable object, leaving open undefined).*

### **5.3 Execution & Output**

1. Place payload.elf in data/.
2. Rebuild Firmware (idf.py build) to pack the filesystem.
3. Create merged flash binary and run in QEMU (see 01_Start.md for QEMU instructions).
4. Execute the payload via your shell interface.

**Expected Serial Output:**

Plaintext

I (5200) ELF\_LOADER: Starting execution...
Guest: Attempting to write to filesystem...
I (5210) VFS\_SHIM: Translating path '/guest\_log.txt' \-\> '/linux/guest\_log.txt'
I (5220) VFS\_SHIM: Translating path '/guest\_log.txt' \-\> '/linux/guest\_log.txt'
Guest: Read back \-\> 'Hello from the Guest ELF via Shim\!'

---

## **6\. Complete shim\_unistd.c Implementation**

Create `main/syscalls/shim_unistd.c`:

```c
/**
 * @file shim_unistd.c
 * @brief POSIX filesystem syscall shim layer for Linux compatibility
 *
 * Translates guest Linux paths to ESP-IDF VFS paths and provides
 * POSIX-compatible interfaces for file operations.
 */

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>

#include "esp_vfs.h"
#include "esp_log.h"

static const char *TAG = "shim_unistd";

// Mount point for the Linux filesystem
#define MOUNT_POINT "/linux"
#define MAX_PATH_LEN 256

/**
 * @brief Translate guest path to host VFS path
 *
 * Guest sees:  /var/log.txt
 * Host needs:  /linux/var/log.txt
 */
static void translate_path(const char *guest_path, char *host_path, size_t max_len) {
    if (guest_path == NULL || host_path == NULL) {
        host_path[0] = '\0';
        return;
    }

    // Handle /dev paths specially - pass through directly
    if (strncmp(guest_path, "/dev/", 5) == 0) {
        strncpy(host_path, guest_path, max_len - 1);
        host_path[max_len - 1] = '\0';
        return;
    }

    if (guest_path[0] == '/') {
        // Absolute path: prepend mount point
        snprintf(host_path, max_len, "%s%s", MOUNT_POINT, guest_path);
    } else {
        // Relative path: assume CWD is mount root
        snprintf(host_path, max_len, "%s/%s", MOUNT_POINT, guest_path);
    }

    ESP_LOGD(TAG, "Path translation: '%s' -> '%s'", guest_path, host_path);
}

/*==============================================================================
 * File Operations
 *============================================================================*/

int shim_open(const char *path, int flags, mode_t mode) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));

    int fd = open(real_path, flags, mode);
    if (fd < 0) {
        ESP_LOGD(TAG, "open('%s') failed: %s", real_path, strerror(errno));
        return -1;
    }

    ESP_LOGD(TAG, "open('%s') = fd %d", real_path, fd);
    return fd;
}

ssize_t shim_read(int fd, void *buf, size_t count) {
    ssize_t ret = read(fd, buf, count);
    if (ret < 0) {
        ESP_LOGD(TAG, "read(fd=%d) failed: %s", fd, strerror(errno));
    }
    return ret;
}

ssize_t shim_write(int fd, const void *buf, size_t count) {
    ssize_t ret = write(fd, buf, count);
    if (ret < 0) {
        ESP_LOGD(TAG, "write(fd=%d) failed: %s", fd, strerror(errno));
    }
    return ret;
}

int shim_close(int fd) {
    int ret = close(fd);
    if (ret < 0) {
        ESP_LOGD(TAG, "close(fd=%d) failed: %s", fd, strerror(errno));
    }
    return ret;
}

off_t shim_lseek(int fd, off_t offset, int whence) {
    off_t ret = lseek(fd, offset, whence);
    if (ret < 0) {
        ESP_LOGD(TAG, "lseek(fd=%d) failed: %s", fd, strerror(errno));
    }
    return ret;
}

/*==============================================================================
 * File Information
 *============================================================================*/

int shim_stat(const char *path, struct stat *st) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));

    int ret = stat(real_path, st);
    if (ret == 0) {
        // LittleFS doesn't support Unix permissions
        // Fake full permissions to prevent "permission denied" errors
        st->st_mode |= S_IRWXU | S_IRWXG | S_IRWXO;  // 0777
    }
    return ret;
}

int shim_fstat(int fd, struct stat *st) {
    int ret = fstat(fd, st);
    if (ret == 0) {
        st->st_mode |= S_IRWXU | S_IRWXG | S_IRWXO;
    }
    return ret;
}

int shim_access(const char *path, int mode) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));

    // Simple existence check - LittleFS doesn't support permissions
    struct stat st;
    int ret = stat(real_path, &st);
    if (ret < 0) {
        return -1;  // File doesn't exist
    }

    // Always report accessible (we fake permissions)
    return 0;
}

/*==============================================================================
 * File Manipulation
 *============================================================================*/

int shim_unlink(const char *path) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));
    return unlink(real_path);
}

int shim_rename(const char *oldpath, const char *newpath) {
    char real_old[MAX_PATH_LEN];
    char real_new[MAX_PATH_LEN];
    translate_path(oldpath, real_old, sizeof(real_old));
    translate_path(newpath, real_new, sizeof(real_new));
    return rename(real_old, real_new);
}

int shim_mkdir(const char *path, mode_t mode) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));
    return mkdir(real_path, mode);
}

int shim_rmdir(const char *path) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));
    return rmdir(real_path);
}

/*==============================================================================
 * Directory Operations
 *============================================================================*/

DIR *shim_opendir(const char *path) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));
    return opendir(real_path);
}

struct dirent *shim_readdir(DIR *dirp) {
    return readdir(dirp);
}

int shim_closedir(DIR *dirp) {
    return closedir(dirp);
}

/*==============================================================================
 * Process Working Directory (Stubbed)
 *============================================================================*/

static char s_cwd[MAX_PATH_LEN] = "/";

char *shim_getcwd(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    strncpy(buf, s_cwd, size - 1);
    buf[size - 1] = '\0';
    return buf;
}

int shim_chdir(const char *path) {
    // Validate the path exists
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));

    struct stat st;
    if (stat(real_path, &st) < 0) {
        errno = ENOENT;
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    // Update CWD (simplified - just store the guest path)
    strncpy(s_cwd, path, sizeof(s_cwd) - 1);
    s_cwd[sizeof(s_cwd) - 1] = '\0';
    return 0;
}
```

---

## **7\. Symbol Export Configuration**

Add these shim functions to the symbol table in `tools/export_symbols.py`:

```python
EXPORT_SYMBOLS = [
    # ... existing symbols ...

    # Filesystem shims (use our wrappers, not direct VFS)
    "shim_open",
    "shim_read",
    "shim_write",
    "shim_close",
    "shim_lseek",
    "shim_stat",
    "shim_fstat",
    "shim_access",
    "shim_unlink",
    "shim_rename",
    "shim_mkdir",
    "shim_rmdir",
    "shim_opendir",
    "shim_readdir",
    "shim_closedir",
    "shim_getcwd",
    "shim_chdir",
]
```

Then create symbol aliases in the generated header or manually in the symbol table:

```c
// In export_symbols.c or a header
#define open    shim_open
#define read    shim_read
#define write   shim_write
#define close   shim_close
#define lseek   shim_lseek
#define stat    shim_stat
#define fstat   shim_fstat
#define access  shim_access
#define unlink  shim_unlink
#define rename  shim_rename
#define mkdir   shim_mkdir
#define rmdir   shim_rmdir
#define opendir shim_opendir
#define readdir shim_readdir
#define closedir shim_closedir
#define getcwd  shim_getcwd
#define chdir   shim_chdir
```

---

## **8\. CMakeLists.txt Update**

Update `main/CMakeLists.txt` to include the shim source:

```cmake
idf_component_register(
    SRCS
        "main.c"
        "syscalls/shim_unistd.c"
    INCLUDE_DIRS
        "."
        "syscalls"
)

# Pack data directory into LittleFS
littlefs_create_partition_image(linux_fs ../data FLASH_IN_PROJECT)
```

---

## **9\. ioctl Support for Custom Devices**

For custom `/dev/` devices, implement ioctl handling:

```c
int shim_ioctl(int fd, unsigned long request, ...) {
    va_list args;
    va_start(args, request);

    // Get the argument (if any)
    void *arg = va_arg(args, void *);
    va_end(args);

    // Pass through to VFS ioctl
    int ret = ioctl(fd, request, arg);

    if (ret < 0) {
        ESP_LOGD(TAG, "ioctl(fd=%d, req=0x%lx) failed: %s",
                 fd, request, strerror(errno));
    }

    return ret;
}
```

---

## **10\. Testing Checklist**

- [ ] `shim_open` correctly translates `/test.txt` to `/linux/test.txt`
- [ ] `shim_open` passes through `/dev/collision` without translation
- [ ] `shim_stat` returns 0777 permissions for all files
- [ ] `shim_read` and `shim_write` work with opened file descriptors
- [ ] `shim_unlink` can delete files in the LittleFS partition
- [ ] `shim_mkdir` / `shim_rmdir` create and remove directories
- [ ] Error cases set errno correctly and return -1

---

## **11\. Common Issues**

| Issue | Cause | Solution |
|-------|-------|----------|
| `ENOENT` on valid path | Path translation error | Check MOUNT_POINT matches actual mount |
| `EBADF` on read/write | FD not valid | Ensure shim_open succeeded |
| Permission errors in app | App checking st_mode | Verify shim_stat adds 0777 |
| Relative paths fail | CWD not handled | Implement shim_chdir properly |
| `/dev/` paths broken | Over-aggressive translation | Add /dev/ passthrough check |  