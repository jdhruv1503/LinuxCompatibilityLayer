/**
 * @file vfs_c2_pipe.h
 * @brief Virtual pipe driver for stdout redirection to network socket
 *
 * This driver provides /dev/c2 - a virtual device that forwards
 * all writes to a configured TCP socket for remote output streaming.
 */

#ifndef VFS_C2_PIPE_H
#define VFS_C2_PIPE_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the /dev/c2 virtual pipe driver
 *
 * Call this in app_main() before any C2 operations.
 * This registers the VFS driver at /dev/c2.
 */
void vfs_c2_pipe_register(void);

/**
 * @brief Set the target socket for C2 output
 *
 * All data written to /dev/c2 will be forwarded to this socket.
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
 * When enabled, all data written to /dev/c2 is also written to
 * stderr (UART) for local debugging purposes.
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

#ifdef __cplusplus
}
#endif

#endif // VFS_C2_PIPE_H
