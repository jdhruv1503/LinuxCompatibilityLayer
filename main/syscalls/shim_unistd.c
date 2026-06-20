/**
 * @file shim_unistd.c
 * @brief POSIX filesystem syscall shim layer for Linux compatibility
 *
 * Translates guest Linux paths to ESP-IDF VFS paths and provides
 * POSIX-compatible interfaces for file operations.
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"

#include "esp_vfs.h"
#include "esp_log.h"
#include "lwip/sockets.h"

static const char *TAG = "shim_unistd";

// Mount point for the Linux filesystem
#define MOUNT_POINT "/linux"
#define MAX_PATH_LEN 256


#define PIPE_BASE_FD 10000
#define MAX_PIPES 4
#define PIPE_QUEUE_LENGTH 256

typedef struct {
    bool used;
    QueueHandle_t queue;
    int read_fd;
    int write_fd;
    bool read_open;
    bool write_open;
} shim_pipe_ctx_t;

static shim_pipe_ctx_t s_pipes[MAX_PIPES] = {0};

static shim_pipe_ctx_t *find_pipe_by_fd(int fd, bool *is_read_end) {
    if (is_read_end) {
        *is_read_end = false;
    }

    for (int i = 0; i < MAX_PIPES; i++) {
        if (!s_pipes[i].used) {
            continue;
        }
        if (fd == s_pipes[i].read_fd) {
            if (is_read_end) {
                *is_read_end = true;
            }
            return &s_pipes[i];
        }
        if (fd == s_pipes[i].write_fd) {
            if (is_read_end) {
                *is_read_end = false;
            }
            return &s_pipes[i];
        }
    }

    return NULL;
}

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
 * C2 Redirection State
 *
 * Uses a shared pointer approach so c2_server.c and shim_write can
 * reliably communicate the socket and flags without sync issues.
 * NOTE: Must be declared before shim_read/shim_write which use it.
 *============================================================================*/

typedef struct {
    int socket_fd;
    bool redirect_stdout;
    bool redirect_stderr;
    bool redirect_stdin;
} c2_redirect_state_t;

// Shared C2 redirection state (allocated once, pointer is stable)
static c2_redirect_state_t s_c2_state = {
    .socket_fd = -1,
    .redirect_stdout = false,
    .redirect_stderr = false,
    .redirect_stdin = false,
};

// Public pointer to state (c2_server.c modifies this via pointer)
c2_redirect_state_t *g_c2_redirect_state = &s_c2_state;

/*==============================================================================
 * File Operations
 *============================================================================*/

__attribute__((used))
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

__attribute__((used))
ssize_t shim_read(int fd, void *buf, size_t count) {
    bool is_read_end = false;
    shim_pipe_ctx_t *pipe = find_pipe_by_fd(fd, &is_read_end);
    if (pipe) {
        if (!is_read_end) {
            ESP_LOGD(TAG, "read on pipe write end fd=%d", fd);
            errno = EBADF;
            return -1;
        }

        uint8_t *out = (uint8_t *)buf;
        size_t i = 0;
        for (; i < count; i++) {
            uint8_t ch;
            if (xQueueReceive(pipe->queue, &ch, 0) != pdTRUE) {
                break;
            }
            out[i] = ch;
        }

        if (i == 0) {
            if (!pipe->write_open) {
                return 0;
            }
            errno = EAGAIN;
            return -1;
        }

        ESP_LOGD(TAG, "pipe read(fd=%d): %d bytes", fd, (int)i);
        return (ssize_t)i;
    }

    // Check if stdin is redirected to socket
    if (fd == STDIN_FILENO && g_c2_redirect_state->redirect_stdin &&
        g_c2_redirect_state->socket_fd >= 0) {
        // Read from the C2 socket instead of UART
        ssize_t ret = recv(g_c2_redirect_state->socket_fd, buf, count, 0);
        if (ret < 0) {
            ESP_LOGD(TAG, "C2 stdin recv(%d bytes) failed: %s", (int)count, strerror(errno));
        } else {
            ESP_LOGD(TAG, "C2 stdin recv: %d bytes", (int)ret);
        }
        return ret;
    }

    // Normal read
    ssize_t ret = read(fd, buf, count);
    if (ret < 0) {
        ESP_LOGD(TAG, "read(fd=%d) failed: %s", fd, strerror(errno));
    }
    return ret;
}

__attribute__((used))
ssize_t shim_write(int fd, const void *buf, size_t count) {
    bool is_read_end = false;
    shim_pipe_ctx_t *pipe = find_pipe_by_fd(fd, &is_read_end);
    if (pipe) {
        if (is_read_end) {
            ESP_LOGD(TAG, "write on pipe read end fd=%d", fd);
            errno = EBADF;
            return -1;
        }

        const uint8_t *in = (const uint8_t *)buf;
        size_t i = 0;
        for (; i < count; i++) {
            if (xQueueSend(pipe->queue, &in[i], 0) != pdTRUE) {
                break;
            }
        }

        if (i == 0) {
            errno = EAGAIN;
            return -1;
        }

        ESP_LOGD(TAG, "pipe write(fd=%d): %d bytes", fd, (int)i);
        return (ssize_t)i;
    }

    // Check if this write should be intercepted for C2 using shared state
    bool is_c2_redirect = (fd == STDOUT_FILENO && g_c2_redirect_state->redirect_stdout) ||
                          (fd == STDERR_FILENO && g_c2_redirect_state->redirect_stderr);

    if (is_c2_redirect) {
        // Use the socket from shared state
        if (g_c2_redirect_state->socket_fd >= 0) {
            // Write to UART for local output (debugging)
            write(fd, buf, count);

            // Also send to C2 socket
            int sent = send(g_c2_redirect_state->socket_fd, buf, count, MSG_DONTWAIT);
            if (sent < 0) {
                // Network error - log at DEBUG level to avoid noise
                ESP_LOGD(TAG, "C2 send(%d bytes) failed: %s (socket not connected?)",
                         (int)count, strerror(errno));
            } else {
                ESP_LOGD(TAG, "C2 send(%d bytes) succeeded", sent);
            }
        }
        // Return success regardless of network status
        return count;
    }

    // Normal write
    ssize_t ret = write(fd, buf, count);
    if (ret < 0) {
        ESP_LOGD(TAG, "write(fd=%d) failed: %s", fd, strerror(errno));
    }
    return ret;
}

__attribute__((used))
int shim_close(int fd) {
    bool is_read_end = false;
    shim_pipe_ctx_t *pipe = find_pipe_by_fd(fd, &is_read_end);
    if (pipe) {
        if (is_read_end) {
            pipe->read_open = false;
        } else {
            pipe->write_open = false;
        }

        if (!pipe->read_open && !pipe->write_open) {
            if (pipe->queue) {
                vQueueDelete(pipe->queue);
            }
            memset(pipe, 0, sizeof(*pipe));
        }

        ESP_LOGD(TAG, "pipe close(fd=%d)", fd);
        return 0;
    }

    int ret = close(fd);
    if (ret < 0) {
        ESP_LOGD(TAG, "close(fd=%d) failed: %s", fd, strerror(errno));
    }
    return ret;
}

__attribute__((used))
off_t shim_lseek(int fd, off_t offset, int whence) {
    off_t ret = lseek(fd, offset, whence);
    if (ret < 0) {
        ESP_LOGD(TAG, "lseek(fd=%d) failed: %s", fd, strerror(errno));
    }
    return ret;
}

__attribute__((used))
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

/*==============================================================================
 * Stdio Operations
 *============================================================================*/

// Forward declaration of custom fprintf
extern int custom_fprintf(FILE *stream, const char *format, ...);

__attribute__((used))
FILE *shim_fopen(const char *path, const char *mode) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));

    FILE *f = fopen(real_path, mode);
    if (!f) {
        ESP_LOGD(TAG, "fopen('%s') failed: %s", real_path, strerror(errno));
    }
    return f;
}

/*==============================================================================
 * Custom fprintf for C2 Stdout Redirection
 *
 * This intercepts fprintf calls and redirects stdout/stderr to the C2 socket
 * when redirection is active. This is necessary because libc's printf/fprintf
 * use internal FILE* structures that bypass fd=1 writes.
 *============================================================================*/

// Re-export the real fprintf from libc
int real_fprintf(FILE *stream, const char *format, ...);

__attribute__((used))
int custom_fprintf(FILE *stream, const char *format, ...) {
    va_list args;
    va_start(args, format);

    // Check if this is stdout or stderr and C2 redirection is active
    bool is_stdout_redirect = (stream == stdout && g_c2_redirect_state->redirect_stdout);
    bool is_stderr_redirect = (stream == stderr && g_c2_redirect_state->redirect_stderr);

    if ((is_stdout_redirect || is_stderr_redirect) && g_c2_redirect_state->socket_fd >= 0) {
        // Format the string into a buffer
        char buf[1024];
        int ret = vsnprintf(buf, sizeof(buf), format, args);
        va_end(args);

        if (ret > 0) {
            // Send to C2 socket
            send(g_c2_redirect_state->socket_fd, buf, ret, MSG_DONTWAIT);

            // Also write to UART for debugging
            fwrite(buf, 1, ret, stderr);
            fflush(stderr);
        }
        return ret;
    }

    // Normal fprintf (not redirected)
    int ret = vfprintf(stream, format, args);
    va_end(args);
    return ret;
}

/*==============================================================================
 * Custom printf/puts for C2 Stdout Redirection
 *
 * Guest ELFs call printf/puts which must be intercepted at this level.
 * These shims check the C2 redirect state and send output to the socket.
 *============================================================================*/

__attribute__((used))
int shim_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    // Format the string into a buffer first
    char buf[1024];
    int ret = vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);

    // Always log that shim_printf was called (use ESP_LOGI for visibility)
    ESP_LOGI(TAG, "shim_printf called: redirect=%d, socket=%d, len=%d",
             g_c2_redirect_state->redirect_stdout,
             g_c2_redirect_state->socket_fd,
             ret);

    // Check if stdout redirection is active
    if (g_c2_redirect_state->redirect_stdout && g_c2_redirect_state->socket_fd >= 0) {
        if (ret > 0) {
            // Send to C2 socket
            int sent = send(g_c2_redirect_state->socket_fd, buf, ret, MSG_DONTWAIT);
            ESP_LOGI(TAG, "shim_printf: sent %d/%d bytes to socket %d",
                     sent, ret, g_c2_redirect_state->socket_fd);

            // Also write to UART for local debugging
            fwrite(buf, 1, ret, stderr);
            fflush(stderr);
        }
        return ret;
    }

    // Normal printf (not redirected) - use vprintf which writes to stdout
    if (ret > 0) {
        fwrite(buf, 1, ret, stdout);
        fflush(stdout);
    }
    return ret;
}

__attribute__((used))
int shim_puts(const char *s) {
    if (s == NULL) {
        return EOF;
    }

    size_t len = strlen(s);

    // Always log that shim_puts was called
    ESP_LOGI(TAG, "shim_puts called: redirect=%d, socket=%d, len=%d",
             g_c2_redirect_state->redirect_stdout,
             g_c2_redirect_state->socket_fd,
             (int)len);

    // Check if stdout redirection is active
    if (g_c2_redirect_state->redirect_stdout && g_c2_redirect_state->socket_fd >= 0) {
        // Send string to C2 socket
        int sent = send(g_c2_redirect_state->socket_fd, s, len, MSG_DONTWAIT);

        // Send newline
        send(g_c2_redirect_state->socket_fd, "\n", 1, MSG_DONTWAIT);

        ESP_LOGI(TAG, "shim_puts: sent %d bytes + newline to socket %d",
                 sent, g_c2_redirect_state->socket_fd);

        // Also write to UART for local debugging
        fputs(s, stderr);
        fputc('\n', stderr);
        fflush(stderr);

        return (sent >= 0) ? 1 : EOF;
    }

    // Normal puts (not redirected)
    fputs(s, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    return 1;
}

/*==============================================================================
 * Custom fgets for C2 Stdin Redirection
 *
 * When stdin is redirected to a socket (for map-reduce workers),
 * this intercepts fgets calls and reads from the socket instead.
 *============================================================================*/

__attribute__((used))
char *shim_fgets(char *s, int size, FILE *stream) {
    // Check if this is stdin and stdin redirection is active
    if (stream == stdin && g_c2_redirect_state->redirect_stdin &&
        g_c2_redirect_state->socket_fd >= 0) {

        ESP_LOGD(TAG, "shim_fgets: reading from C2 socket fd=%d, size=%d",
                 g_c2_redirect_state->socket_fd, size);

        // Read from socket character by character until newline or EOF
        int i = 0;
        while (i < size - 1) {
            char c;
            ssize_t ret = recv(g_c2_redirect_state->socket_fd, &c, 1, 0);

            if (ret <= 0) {
                // EOF or error
                if (i == 0) {
                    // No data read, return NULL (EOF)
                    ESP_LOGD(TAG, "shim_fgets: EOF (ret=%d)", (int)ret);
                    return NULL;
                }
                break;  // Return what we have
            }

            s[i++] = c;

            if (c == '\n') {
                break;  // End of line
            }
        }

        s[i] = '\0';
        ESP_LOGD(TAG, "shim_fgets: read %d bytes: '%s'", i, s);
        return s;
    }

    // Normal fgets (not redirected)
    return fgets(s, size, stream);
}

/*==============================================================================
 * File Information
 *============================================================================*/

__attribute__((used))
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

__attribute__((used))
int shim_fstat(int fd, struct stat *st) {
    int ret = fstat(fd, st);
    if (ret == 0) {
        st->st_mode |= S_IRWXU | S_IRWXG | S_IRWXO;
    }
    return ret;
}

__attribute__((used))
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

__attribute__((used))
int shim_unlink(const char *path) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));
    return unlink(real_path);
}

__attribute__((used))
int shim_rename(const char *oldpath, const char *newpath) {
    char real_old[MAX_PATH_LEN];
    char real_new[MAX_PATH_LEN];
    translate_path(oldpath, real_old, sizeof(real_old));
    translate_path(newpath, real_new, sizeof(real_new));
    return rename(real_old, real_new);
}

__attribute__((used))
int shim_mkdir(const char *path, mode_t mode) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));
    return mkdir(real_path, mode);
}

__attribute__((used))
int shim_rmdir(const char *path) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));
    return rmdir(real_path);
}

/*==============================================================================
 * Directory Operations
 *============================================================================*/

__attribute__((used))
DIR *shim_opendir(const char *path) {
    char real_path[MAX_PATH_LEN];
    translate_path(path, real_path, sizeof(real_path));
    return opendir(real_path);
}

__attribute__((used))
struct dirent *shim_readdir(DIR *dirp) {
    return readdir(dirp);
}

__attribute__((used))
int shim_closedir(DIR *dirp) {
    return closedir(dirp);
}

/*==============================================================================
 * Process Working Directory (Stubbed)
 *============================================================================*/

static char s_cwd[MAX_PATH_LEN] = "/";

__attribute__((used))
char *shim_getcwd(char *buf, size_t size) {
    if (buf == NULL || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    strncpy(buf, s_cwd, size - 1);
    buf[size - 1] = '\0';
    return buf;
}

__attribute__((used))
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

/*==============================================================================
 * File Descriptor Duplication (for stdout redirection)
 *============================================================================*/

// Import from vfs_c2_pipe.c
extern void c2_pipe_set_socket(int socket_fd);

// Track original stdout/stderr for potential restoration (reserved for future use)
__attribute__((unused)) static int s_original_stdout_fd = -1;
__attribute__((unused)) static int s_original_stderr_fd = -1;

__attribute__((used))
int shim_dup(int oldfd) {
    ESP_LOGD(TAG, "dup(oldfd=%d)", oldfd);

    // Simple dup - just return the same fd (not true duplication)
    // This is a simplification since ESP32 VFS doesn't support true dup
    // For C2 use case, this is sufficient
    return oldfd;
}

__attribute__((used))
int shim_dup2(int oldfd, int newfd) {
    ESP_LOGD(TAG, "dup2(oldfd=%d, newfd=%d)", oldfd, newfd);

    // We support redirecting stdin/stdout/stderr to sockets for C2
    if (newfd != STDIN_FILENO && newfd != STDOUT_FILENO && newfd != STDERR_FILENO) {
        ESP_LOGD(TAG, "dup2: only stdin/stdout/stderr redirection supported");
        errno = ENOTSUP;
        return -1;
    }

    // If oldfd == newfd, POSIX says just return newfd
    if (oldfd == newfd) {
        return newfd;
    }

    // Configure the C2 pipe to use this socket
    // This is the "Global Socket Variable Approach" - we don't actually
    // close/reopen FDs, just set up the C2 pipe to intercept writes
    c2_pipe_set_socket(oldfd);

    // Set the socket FD in shared state so shim_puts/shim_printf can use it
    g_c2_redirect_state->socket_fd = oldfd;

    // Mark which FD is being redirected using shared state
    if (newfd == STDIN_FILENO) {
        g_c2_redirect_state->redirect_stdin = true;
        ESP_LOGD(TAG, "stdin now redirected to C2 socket fd=%d", oldfd);
    }
    if (newfd == STDOUT_FILENO) {
        g_c2_redirect_state->redirect_stdout = true;
        ESP_LOGD(TAG, "stdout now redirected to C2 socket fd=%d", oldfd);
    }
    if (newfd == STDERR_FILENO) {
        g_c2_redirect_state->redirect_stderr = true;
        ESP_LOGD(TAG, "stderr now redirected to C2 socket fd=%d", oldfd);
    }

    // Return newfd to indicate success (POSIX-compliant)
    return newfd;
}

__attribute__((used))
int shim_dup3(int oldfd, int newfd, int flags) {
    // dup3 with flags - we ignore flags for simplicity
    // O_CLOEXEC flag is not relevant for ESP32 single-process model
    ESP_LOGD(TAG, "dup3(oldfd=%d, newfd=%d, flags=0x%x)", oldfd, newfd, flags);
    return shim_dup2(oldfd, newfd);
}

/*==============================================================================
 * Time/User/System Utilities
 *============================================================================*/

__attribute__((used))
int shim_clock_gettime(clockid_t clk_id, struct timespec *tp) {
    if (!tp) {
        errno = EINVAL;
        ESP_LOGD(TAG, "clock_gettime invalid args");
        return -1;
    }

    if (clk_id != CLOCK_REALTIME && clk_id != CLOCK_MONOTONIC) {
        errno = EINVAL;
        ESP_LOGD(TAG, "clock_gettime unsupported clock id=%d", (int)clk_id);
        return -1;
    }

    int64_t us = esp_timer_get_time();
    tp->tv_sec = us / 1000000;
    tp->tv_nsec = (us % 1000000) * 1000;
    ESP_LOGD(TAG, "clock_gettime(%d): %ld.%09ld", (int)clk_id, (long)tp->tv_sec, (long)tp->tv_nsec);
    return 0;
}

__attribute__((used))
int shim_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (!req || req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        ESP_LOGD(TAG, "nanosleep invalid request");
        return -1;
    }

    uint64_t total_ms = (uint64_t)req->tv_sec * 1000ULL + (uint64_t)(req->tv_nsec / 1000000L);
    if (req->tv_nsec % 1000000L) {
        total_ms += 1;
    }

    if (total_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(total_ms));
    }

    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }

    ESP_LOGD(TAG, "nanosleep: %llu ms", (unsigned long long)total_ms);
    return 0;
}

__attribute__((used))
uid_t shim_getuid(void) { ESP_LOGD(TAG, "getuid -> 0"); return 0; }

__attribute__((used))
gid_t shim_getgid(void) { ESP_LOGD(TAG, "getgid -> 0"); return 0; }

__attribute__((used))
uid_t shim_geteuid(void) { ESP_LOGD(TAG, "geteuid -> 0"); return 0; }

__attribute__((used))
gid_t shim_getegid(void) { ESP_LOGD(TAG, "getegid -> 0"); return 0; }

__attribute__((used))
int shim_ftruncate(int fd, off_t length) {
    if (length < 0) {
        errno = EINVAL;
        ESP_LOGD(TAG, "ftruncate invalid length=%ld", (long)length);
        return -1;
    }

    int ret = ftruncate(fd, length);
    if (ret < 0) {
        ESP_LOGD(TAG, "ftruncate(fd=%d, len=%ld) failed: %s", fd, (long)length, strerror(errno));
    } else {
        ESP_LOGD(TAG, "ftruncate(fd=%d, len=%ld) ok", fd, (long)length);
    }
    return ret;
}

__attribute__((used))
int shim_pipe(int pipefd[2]) {
    if (!pipefd) {
        errno = EINVAL;
        ESP_LOGD(TAG, "pipe called with null array");
        return -1;
    }

    for (int i = 0; i < MAX_PIPES; i++) {
        if (s_pipes[i].used) {
            continue;
        }

        QueueHandle_t q = xQueueCreate(PIPE_QUEUE_LENGTH, sizeof(uint8_t));
        if (!q) {
            errno = ENOMEM;
            ESP_LOGD(TAG, "pipe queue allocation failed");
            return -1;
        }

        s_pipes[i].used = true;
        s_pipes[i].queue = q;
        s_pipes[i].read_fd = PIPE_BASE_FD + (i * 2);
        s_pipes[i].write_fd = PIPE_BASE_FD + (i * 2) + 1;
        s_pipes[i].read_open = true;
        s_pipes[i].write_open = true;

        pipefd[0] = s_pipes[i].read_fd;
        pipefd[1] = s_pipes[i].write_fd;

        ESP_LOGD(TAG, "pipe created: rfd=%d wfd=%d", pipefd[0], pipefd[1]);
        return 0;
    }

    errno = ENFILE;
    ESP_LOGD(TAG, "pipe table exhausted");
    return -1;
}

__attribute__((used))
pid_t shim_getpid(void) {
    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    pid_t pid = (pid_t)(uintptr_t)handle;
    ESP_LOGD(TAG, "getpid -> %d", (int)pid);
    return pid;
}

__attribute__((used))
int shim_gettimeofday(struct timeval *tv, struct timezone *tz) {
    if (!tv) {
        errno = EINVAL;
        ESP_LOGD(TAG, "gettimeofday invalid tv=NULL");
        return -1;
    }

    int64_t us = esp_timer_get_time();
    tv->tv_sec = us / 1000000;
    tv->tv_usec = us % 1000000;

    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }

    ESP_LOGD(TAG, "gettimeofday -> %ld.%06ld", (long)tv->tv_sec, (long)tv->tv_usec);
    return 0;
}
