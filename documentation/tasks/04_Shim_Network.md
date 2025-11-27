## **1\. Overview**

This document defines the implementation of the networking and process control subsystems for the compatibility layer.

Unlike the Filesystem shim, which maps clean POSIX paths to VFS paths, the Networking shim must bridge the gap between the guest's expectation of a standard BSD Socket API and the specific behaviors of the ESP-IDF LwIP stack. Additionally, it defines the "Spawn Model" used to emulate process creation, bypassing the hardware limitations of the ESP32 (No MMU).

---

## **2\. Networking Shim (LwIP Integration)**

The ESP-IDF uses LwIP (Lightweight IP) as its TCP/IP stack. While LwIP provides a BSD-like socket API, there are critical differences in error handling that can crash a Linux payload if not bridged correctly.

### **2.1 The Errno Translation Problem**

Standard Linux applications rely heavily on errno to decide control flow (e.g., retrying on EAGAIN or EINTR).

* **The Issue:** LwIP has its own internal error codes (often negative values or specific LwIP enums) that do not always 1:1 match the Newlib errno.h values linked into the guest ELF.  
* **The Fix:** The shim must capture the LwIP error, translate it to the guest's expected errno value, and store it in the task's thread-local storage.

### **2.2 Implementation: shim\_socket.c**

Create this file to wrap the core socket functions.

#### **The Translation Helper**

C

\#**include** "lwip/sockets.h"  
\#**include** \<errno.h\>

// Helper to translate LwIP errors to generic POSIX errno  
static void map\_lwip\_error\_to\_errno(void) {  
    int lwip\_err \= errno; // LwIP sets the global errno  
      
    // In many ESP-IDF configs, these map 1:1, but strictly safe code   
    // verifies the mapping to prevent divergence.  
    switch (lwip\_err) {  
        case EWOULDBLOCK: errno \= EAGAIN; break; // Crucial for non-blocking I/O  
        case ENOMEM:      errno \= ENOMEM; break;  
        case EADDRINUSE:  errno \= EADDRINUSE; break;  
        case ECONNRESET:  errno \= ECONNRESET; break;  
        default:          errno \= lwip\_err; break;  
    }  
}

#### **Socket Wrappers**

We wrap the standard calls (socket, bind, accept, connect, recv, send) to ensure the translation hook is called.

C

int shim\_socket(int domain, int type, int protocol) {  
    int fd \= lwip\_socket(domain, type, protocol);  
    if (fd \< 0) {  
        map\_lwip\_error\_to\_errno();  
        return \-1;  
    }  
    return fd;  
}

int shim\_bind(int s, const struct sockaddr \*name, socklen\_t namelen) {  
    int ret \= lwip\_bind(s, name, namelen);  
    if (ret \< 0) {  
        map\_lwip\_error\_to\_errno();  
        return \-1;  
    }  
    return ret;  
}

int shim\_accept(int s, struct sockaddr \*addr, socklen\_t \*addrlen) {  
    int fd \= lwip\_accept(s, addr, addrlen);  
    if (fd \< 0) {  
        map\_lwip\_error\_to\_errno();  
        return \-1;  
    }  
    return fd;  
}

---

## **3\. Process Control & Emulation**

The ESP32 lacks a Memory Management Unit (MMU), making it physically impossible to implement fork() with Copy-on-Write (CoW). Therefore, standard process creation semantics must be adapted.

### **3.1 The "No-Fork" Constraint**

* **Standard Linux:** fork() clones the entire address space. exec() replaces it.  
* **ESP32 Reality:** We cannot clone the address space.  
* **Shim Behavior:**  
  * shim\_fork(): **MUST** return \-1 and set errno \= ENOSYS.  
  * Application developers must be instructed to use vfork() (if supported by Newlib) or simply rely on execve (spawn mode).

### **3.2 The Spawn Strategy (execve)**

On this system, execve functions more like posix\_spawn. It does not replace the *current* task's memory (which would corrupt the OS state if not careful); instead, it launches the ELF in a **new FreeRTOS task**.

**Implementation Logic (shim\_process.c):**

C

\#**include** "freertos/FreeRTOS.h"  
\#**include** "freertos/task.h"

// Defined in elf\_loader.c  
extern void elf\_loader\_start(const char \*path, char \*const argv\[\]);

int shim\_execve(const char \*path, char \*const argv\[\], char \*const envp\[\]) {  
    // 1\. Validate the path exists (shim\_stat)  
      
    // 2\. Spawn a new FreeRTOS task to handle the ELF  
    //    We increase stack size to accommodate the loader \+ guest app  
    BaseType\_t ret \= xTaskCreate(  
        (TaskFunction\_t)elf\_loader\_start,  // Entry point wrapper  
        "guest\_proc",                      // Name  
        8192,                              // Stack depth (bytes)  
        (void \*)path,                      // Parameters (deep copy needed in real impl)  
        5,                                 // Priority (default)  
        NULL                               // Handle  
    );

    if (ret \!= pdPASS) {  
        errno \= ENOMEM;  
        return \-1;  
    }

    // 3\. In a spawn model, the parent returns success immediately   
    //    (or waits, if waitpid is implemented)  
    return 0;   
}

*Note: In a robust implementation, you must deep-copy argv because the parent task might free it before the new task starts.*

### **3.3 Termination (exit)**

When a guest application calls exit(code), it believes it is terminating a process. On the ESP32, this translates to deleting the FreeRTOS task.

Crucial Safety Step:  
We must assume the guest app does not know it's a task. It might not free its memory. The Shim (or Loader) should track allocations, but strictly speaking, exit() maps to:

C

void shim\_exit(int status) {  
    // Optional: Log exit status  
    // printf("Process exited with code %d\\n", status);  
      
    // Kill the current FreeRTOS task  
    vTaskDelete(NULL);   
      
    // Infinite loop to satisfy compiler 'noreturn' attributes  
    while(1);   
}

---

## **4\. Summary of Exported Symbols**

For the shim to function, update tools/export\_symbols.py to include:

**Network:**

* socket \-\> \&shim\_socket  
* bind \-\> \&shim\_bind  
* listen \-\> \&lwip\_listen (Pass-through usually safe)  
* accept \-\> \&shim\_accept  
* connect \-\> \&lwip\_connect  
* send \-\> \&lwip\_send  
* recv \-\> \&lwip\_recv

**Process:**

* fork \-\> \&shim\_fork (Returns Error)  
* execve \-\> \&shim\_execve  
* exit \-\> \&shim\_exit

## **5\. Testing Checklist**

1. **Network Client:** Compile a C app using standard socket() / connect() to fetch a webpage (HTTP GET). Verify shim\_socket is hit.
2. **Network Server:** Compile a "Hello World" TCP server. Verify shim\_bind and shim\_accept handle connections correctly.
3. **Spawn Test:** Write a "Launcher" app that calls execve("/linux/child.elf", ...). Verify a new task starts and prints output parallel to the launcher.

---

## **6\. Complete shim\_socket.c Implementation**

Create `main/syscalls/shim_socket.c`:

```c
/**
 * @file shim_socket.c
 * @brief BSD Socket API shim for Linux compatibility
 *
 * Wraps LwIP socket functions with proper errno translation
 * for POSIX-compatible behavior.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"

static const char *TAG = "shim_socket";

/*==============================================================================
 * Error Code Translation
 *============================================================================*/

/**
 * @brief Map LwIP error codes to standard POSIX errno values
 *
 * LwIP generally uses POSIX-compatible errno values, but there are
 * some edge cases that need explicit handling.
 */
static void translate_lwip_errno(void) {
    int err = errno;

    switch (err) {
        case EWOULDBLOCK:
            // Some systems distinguish EWOULDBLOCK from EAGAIN
            // POSIX says they should be the same for sockets
            errno = EAGAIN;
            break;

        case 118:  // ENOTCONN on some LwIP configs
            errno = ENOTCONN;
            break;

        case 119:  // ESHUTDOWN on some LwIP configs
            errno = ESHUTDOWN;
            break;

        default:
            // Most errors map 1:1
            break;
    }
}

/*==============================================================================
 * Socket Creation & Configuration
 *============================================================================*/

int shim_socket(int domain, int type, int protocol) {
    ESP_LOGD(TAG, "socket(domain=%d, type=%d, proto=%d)", domain, type, protocol);

    int fd = lwip_socket(domain, type, protocol);
    if (fd < 0) {
        translate_lwip_errno();
        ESP_LOGD(TAG, "socket() failed: %s", strerror(errno));
        return -1;
    }

    ESP_LOGD(TAG, "socket() = %d", fd);
    return fd;
}

int shim_setsockopt(int s, int level, int optname, const void *optval, socklen_t optlen) {
    int ret = lwip_setsockopt(s, level, optname, optval, optlen);
    if (ret < 0) {
        translate_lwip_errno();
    }
    return ret;
}

int shim_getsockopt(int s, int level, int optname, void *optval, socklen_t *optlen) {
    int ret = lwip_getsockopt(s, level, optname, optval, optlen);
    if (ret < 0) {
        translate_lwip_errno();
    }
    return ret;
}

int shim_fcntl(int s, int cmd, int val) {
    // LwIP supports F_GETFL and F_SETFL for non-blocking mode
    int ret = lwip_fcntl(s, cmd, val);
    if (ret < 0) {
        translate_lwip_errno();
    }
    return ret;
}

/*==============================================================================
 * Connection Management
 *============================================================================*/

int shim_bind(int s, const struct sockaddr *name, socklen_t namelen) {
    ESP_LOGD(TAG, "bind(fd=%d)", s);

    int ret = lwip_bind(s, name, namelen);
    if (ret < 0) {
        translate_lwip_errno();
        ESP_LOGD(TAG, "bind() failed: %s", strerror(errno));
        return -1;
    }
    return ret;
}

int shim_listen(int s, int backlog) {
    ESP_LOGD(TAG, "listen(fd=%d, backlog=%d)", s, backlog);

    int ret = lwip_listen(s, backlog);
    if (ret < 0) {
        translate_lwip_errno();
        ESP_LOGD(TAG, "listen() failed: %s", strerror(errno));
        return -1;
    }
    return ret;
}

int shim_accept(int s, struct sockaddr *addr, socklen_t *addrlen) {
    ESP_LOGD(TAG, "accept(fd=%d)", s);

    int fd = lwip_accept(s, addr, addrlen);
    if (fd < 0) {
        translate_lwip_errno();
        ESP_LOGD(TAG, "accept() failed: %s", strerror(errno));
        return -1;
    }

    ESP_LOGD(TAG, "accept() = %d", fd);
    return fd;
}

int shim_connect(int s, const struct sockaddr *name, socklen_t namelen) {
    ESP_LOGD(TAG, "connect(fd=%d)", s);

    int ret = lwip_connect(s, name, namelen);
    if (ret < 0) {
        translate_lwip_errno();
        ESP_LOGD(TAG, "connect() failed: %s", strerror(errno));
        return -1;
    }
    return ret;
}

int shim_shutdown(int s, int how) {
    int ret = lwip_shutdown(s, how);
    if (ret < 0) {
        translate_lwip_errno();
    }
    return ret;
}

/*==============================================================================
 * Data Transfer
 *============================================================================*/

ssize_t shim_send(int s, const void *data, size_t size, int flags) {
    ssize_t ret = lwip_send(s, data, size, flags);
    if (ret < 0) {
        translate_lwip_errno();
        ESP_LOGD(TAG, "send(fd=%d) failed: %s", s, strerror(errno));
    }
    return ret;
}

ssize_t shim_sendto(int s, const void *data, size_t size, int flags,
                    const struct sockaddr *to, socklen_t tolen) {
    ssize_t ret = lwip_sendto(s, data, size, flags, to, tolen);
    if (ret < 0) {
        translate_lwip_errno();
    }
    return ret;
}

ssize_t shim_recv(int s, void *mem, size_t len, int flags) {
    ssize_t ret = lwip_recv(s, mem, len, flags);
    if (ret < 0) {
        translate_lwip_errno();
        ESP_LOGD(TAG, "recv(fd=%d) failed: %s", s, strerror(errno));
    }
    return ret;
}

ssize_t shim_recvfrom(int s, void *mem, size_t len, int flags,
                      struct sockaddr *from, socklen_t *fromlen) {
    ssize_t ret = lwip_recvfrom(s, mem, len, flags, from, fromlen);
    if (ret < 0) {
        translate_lwip_errno();
    }
    return ret;
}

/*==============================================================================
 * Socket Information
 *============================================================================*/

int shim_getsockname(int s, struct sockaddr *name, socklen_t *namelen) {
    return lwip_getsockname(s, name, namelen);
}

int shim_getpeername(int s, struct sockaddr *name, socklen_t *namelen) {
    return lwip_getpeername(s, name, namelen);
}

/*==============================================================================
 * I/O Multiplexing
 *============================================================================*/

int shim_select(int maxfdp1, fd_set *readset, fd_set *writeset,
                fd_set *exceptset, struct timeval *timeout) {
    int ret = lwip_select(maxfdp1, readset, writeset, exceptset, timeout);
    if (ret < 0) {
        translate_lwip_errno();
    }
    return ret;
}

int shim_poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    int ret = lwip_poll(fds, nfds, timeout);
    if (ret < 0) {
        translate_lwip_errno();
    }
    return ret;
}

/*==============================================================================
 * DNS Resolution
 *============================================================================*/

struct hostent *shim_gethostbyname(const char *name) {
    return lwip_gethostbyname(name);
}

int shim_getaddrinfo(const char *nodename, const char *servname,
                     const struct addrinfo *hints, struct addrinfo **res) {
    return lwip_getaddrinfo(nodename, servname, hints, res);
}

void shim_freeaddrinfo(struct addrinfo *ai) {
    lwip_freeaddrinfo(ai);
}
```

---

## **7\. Complete shim\_process.c Implementation**

Create `main/syscalls/shim_process.c`:

```c
/**
 * @file shim_process.c
 * @brief Process control shim for Linux compatibility
 *
 * Implements the "spawn model" for process creation on ESP32
 * (which lacks an MMU for true fork/exec).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <signal.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "shim_process";

// Forward declaration - implemented in elf_loader.c
extern int elf_loader_run(const char *path, int argc, char *argv[]);

/*==============================================================================
 * Process ID Emulation
 *============================================================================*/

// Map FreeRTOS task handle to "PID"
pid_t shim_getpid(void) {
    // Use task handle as pseudo-PID
    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    return (pid_t)(uintptr_t)handle;
}

pid_t shim_getppid(void) {
    // No real parent-child relationship - return 1 (init)
    return 1;
}

/*==============================================================================
 * Fork (NOT SUPPORTED)
 *============================================================================*/

pid_t shim_fork(void) {
    // ESP32 has no MMU - cannot implement fork()
    ESP_LOGW(TAG, "fork() called - NOT SUPPORTED on ESP32");
    errno = ENOSYS;
    return -1;
}

pid_t shim_vfork(void) {
    // vfork also not supported
    ESP_LOGW(TAG, "vfork() called - NOT SUPPORTED on ESP32");
    errno = ENOSYS;
    return -1;
}

/*==============================================================================
 * Exec Family (Spawn Model)
 *============================================================================*/

// Structure to pass parameters to the spawned task
typedef struct {
    char *path;
    char **argv;
    int argc;
    SemaphoreHandle_t started_sem;
} exec_params_t;

static void exec_task_wrapper(void *pvParameters) {
    exec_params_t *params = (exec_params_t *)pvParameters;

    // Signal that we've started and copied parameters
    char *path = params->path;
    char **argv = params->argv;
    int argc = params->argc;

    xSemaphoreGive(params->started_sem);

    ESP_LOGI(TAG, "Executing: %s", path);

    // Run the ELF
    int ret = elf_loader_run(path, argc, argv);

    ESP_LOGI(TAG, "ELF returned: %d", ret);

    // Free argument copies
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
    free(path);

    // Delete this task
    vTaskDelete(NULL);
}

int shim_execve(const char *path, char *const argv[], char *const envp[]) {
    ESP_LOGI(TAG, "execve('%s')", path);

    // Count arguments
    int argc = 0;
    if (argv) {
        while (argv[argc] != NULL) {
            argc++;
        }
    }

    // Deep copy path and arguments (parent might free them)
    char *path_copy = strdup(path);
    if (!path_copy) {
        errno = ENOMEM;
        return -1;
    }

    char **argv_copy = malloc((argc + 1) * sizeof(char *));
    if (!argv_copy) {
        free(path_copy);
        errno = ENOMEM;
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        argv_copy[i] = strdup(argv[i]);
        if (!argv_copy[i]) {
            for (int j = 0; j < i; j++) free(argv_copy[j]);
            free(argv_copy);
            free(path_copy);
            errno = ENOMEM;
            return -1;
        }
    }
    argv_copy[argc] = NULL;

    // Create synchronization semaphore
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) {
        for (int i = 0; i < argc; i++) free(argv_copy[i]);
        free(argv_copy);
        free(path_copy);
        errno = ENOMEM;
        return -1;
    }

    // Setup parameters
    exec_params_t params = {
        .path = path_copy,
        .argv = argv_copy,
        .argc = argc,
        .started_sem = sem,
    };

    // Create the child task
    BaseType_t ret = xTaskCreate(
        exec_task_wrapper,
        "guest_elf",
        8192,  // Stack size (adjust as needed)
        &params,
        5,     // Priority
        NULL
    );

    if (ret != pdPASS) {
        vSemaphoreDelete(sem);
        for (int i = 0; i < argc; i++) free(argv_copy[i]);
        free(argv_copy);
        free(path_copy);
        errno = ENOMEM;
        return -1;
    }

    // Wait for child to start and copy parameters
    xSemaphoreTake(sem, portMAX_DELAY);
    vSemaphoreDelete(sem);

    // In true exec, we would never return. In spawn model, return success.
    return 0;
}

// Simplified versions
int shim_execv(const char *path, char *const argv[]) {
    return shim_execve(path, argv, NULL);
}

int shim_execvp(const char *file, char *const argv[]) {
    // Simple implementation - just try the path directly
    return shim_execve(file, argv, NULL);
}

/*==============================================================================
 * Process Termination
 *============================================================================*/

void shim_exit(int status) {
    ESP_LOGI(TAG, "exit(%d) called", status);

    // TODO: Cleanup any resources allocated by this "process"
    // - Close open file descriptors
    // - Free heap allocations
    // - Cancel pending timers

    // Delete the current FreeRTOS task
    vTaskDelete(NULL);

    // Should never reach here
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}

void shim__exit(int status) {
    // Same as exit for our purposes
    shim_exit(status);
}

void shim_abort(void) {
    ESP_LOGE(TAG, "abort() called!");
    shim_exit(-1);
}

/*==============================================================================
 * Signal Handling (Stubbed)
 *============================================================================*/

// Minimal signal support using FreeRTOS task notifications
typedef void (*sighandler_t)(int);
static sighandler_t s_signal_handlers[32] = {0};

sighandler_t shim_signal(int signum, sighandler_t handler) {
    if (signum < 0 || signum >= 32) {
        errno = EINVAL;
        return SIG_ERR;
    }

    sighandler_t old = s_signal_handlers[signum];
    s_signal_handlers[signum] = handler;
    return old;
}

int shim_raise(int sig) {
    if (sig < 0 || sig >= 32) {
        errno = EINVAL;
        return -1;
    }

    sighandler_t handler = s_signal_handlers[sig];
    if (handler && handler != SIG_IGN && handler != SIG_DFL) {
        handler(sig);
    } else if (handler == SIG_DFL) {
        // Default action for most signals is terminate
        if (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT) {
            shim_exit(128 + sig);
        }
    }

    return 0;
}

int shim_kill(pid_t pid, int sig) {
    // Can only signal the current task in our model
    if ((pid_t)(uintptr_t)xTaskGetCurrentTaskHandle() == pid) {
        return shim_raise(sig);
    }

    // TODO: Could use task notifications to signal other tasks
    errno = ESRCH;
    return -1;
}
```

---

## **8\. WiFi Initialization for Networking**

Before socket operations work, WiFi must be initialized. Add to `main.c`:

```c
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"

#define WIFI_SSID      "YourSSID"
#define WIFI_PASSWORD  "YourPassword"

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void wifi_init_sta(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}
```

---

## **9\. CMakeLists.txt Update**

```cmake
idf_component_register(
    SRCS
        "main.c"
        "syscalls/shim_unistd.c"
        "syscalls/shim_socket.c"
        "syscalls/shim_process.c"
    INCLUDE_DIRS
        "."
        "syscalls"
)
```

---

## **10\. Symbol Export Updates**

Add to `tools/export_symbols.py`:

```python
EXPORT_SYMBOLS = [
    # ... existing ...

    # Network (socket shims)
    "shim_socket",
    "shim_bind",
    "shim_listen",
    "shim_accept",
    "shim_connect",
    "shim_send",
    "shim_sendto",
    "shim_recv",
    "shim_recvfrom",
    "shim_shutdown",
    "shim_setsockopt",
    "shim_getsockopt",
    "shim_select",
    "shim_poll",
    "shim_gethostbyname",
    "shim_getaddrinfo",
    "shim_freeaddrinfo",

    # Process (stub/spawn shims)
    "shim_getpid",
    "shim_getppid",
    "shim_fork",
    "shim_execve",
    "shim_execv",
    "shim_exit",
    "shim__exit",
    "shim_abort",
    "shim_signal",
    "shim_raise",
    "shim_kill",
]
```

---

## **11\. Testing Network Functionality**

### **11.1 Simple TCP Client Payload**

```c
// apps/tcp_client/main.c
extern int printf(const char *fmt, ...);
extern int socket(int domain, int type, int protocol);
extern int connect(int s, const struct sockaddr *name, int namelen);
extern int send(int s, const void *data, int size, int flags);
extern int recv(int s, void *mem, int len, int flags);
extern int close(int fd);

#include <netinet/in.h>
#include <arpa/inet.h>

int main(void) {
    printf("TCP Client Test\n");

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("socket() failed\n");
        return 1;
    }

    struct sockaddr_in server = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
    };
    inet_aton("93.184.216.34", &server.sin_addr);  // example.com

    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        printf("connect() failed\n");
        return 1;
    }

    const char *request = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    send(sock, request, strlen(request), 0);

    char buf[512];
    int len = recv(sock, buf, sizeof(buf)-1, 0);
    buf[len] = '\0';
    printf("Response:\n%s\n", buf);

    close(sock);
    return 0;
}
```