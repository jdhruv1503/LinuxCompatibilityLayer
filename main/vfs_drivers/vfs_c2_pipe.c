/**
 * @file vfs_c2_pipe.c
 * @brief Virtual pipe driver for stdout redirection to network socket
 *
 * This driver provides /dev/c2 - a virtual device that forwards
 * all writes to a configured TCP socket for remote output streaming.
 * Used to implement dup2() redirection for C2 payloads.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "esp_vfs.h"
#include "esp_log.h"
#include "lwip/sockets.h"

#include "vfs_c2_pipe.h"

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
    .mirror_to_uart = true,  // Default: mirror to UART for debugging
};

/*==============================================================================
 * VFS Operations
 *============================================================================*/

static int c2_open(const char *path, int flags, int mode) {
    ESP_LOGD(TAG, "c2_open(path='%s', flags=0x%x)", path, flags);

    if (s_ctx.active) {
        // Only one C2 session at a time
        ESP_LOGD(TAG, "C2 pipe already active");
        errno = EBUSY;
        return -1;
    }

    s_ctx.active = true;
    ESP_LOGD(TAG, "C2 pipe opened");
    return 0;  // Return virtual FD (VFS manages actual FD mapping)
}

static int c2_close(int fd) {
    ESP_LOGD(TAG, "c2_close(fd=%d)", fd);
    s_ctx.active = false;
    ESP_LOGD(TAG, "C2 pipe closed");
    return 0;
}

static ssize_t c2_write(int fd, const void *data, size_t size) {
    if (!s_ctx.active) {
        errno = EBADF;
        return -1;
    }

    // Mirror to UART for local debugging (optional)
    if (s_ctx.mirror_to_uart) {
        // Use stderr to avoid recursion if stdout is redirected
        fwrite(data, 1, size, stderr);
        fflush(stderr);
    }

    // Forward to network socket
    if (s_ctx.target_socket_fd >= 0) {
        int sent = send(s_ctx.target_socket_fd, data, size, MSG_DONTWAIT);

        if (sent < 0) {
            // Network error - log but don't fail the write
            // This prevents app crash if C2 connection drops
            ESP_LOGD(TAG, "send() failed: %d (%s)", errno, strerror(errno));
            return size;  // Pretend success to keep app running
        }

        ESP_LOGD(TAG, "Sent %d bytes to socket fd=%d", sent, s_ctx.target_socket_fd);
        return sent;
    }

    // No socket configured - just pretend we wrote it
    ESP_LOGD(TAG, "No socket configured, discarding %d bytes", (int)size);
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
        ESP_LOGD(TAG, "recv() failed: %d (%s)", errno, strerror(errno));
        return -1;
    }

    ESP_LOGD(TAG, "Received %d bytes from socket fd=%d", received, s_ctx.target_socket_fd);
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
    ESP_LOGD(TAG, "C2 pipe configured for socket fd=%d", socket_fd);
}

int c2_pipe_get_socket(void) {
    return s_ctx.target_socket_fd;
}

void c2_pipe_set_mirror(bool enable) {
    s_ctx.mirror_to_uart = enable;
    ESP_LOGD(TAG, "C2 pipe UART mirror %s", enable ? "enabled" : "disabled");
}

bool c2_pipe_is_active(void) {
    return s_ctx.active && s_ctx.target_socket_fd >= 0;
}
