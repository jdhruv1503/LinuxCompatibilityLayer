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
 * Linux to BSD sockaddr Translation
 *
 * Linux sockaddr_in:     { sa_family_t (2), port (2), addr (4), zero (8) }
 * BSD/LwIP sockaddr_in:  { len (1), family (1), port (2), addr (4), zero (8) }
 *
 * Guest apps use Linux-style structs. The shim translates to BSD style for LwIP.
 *============================================================================*/

// Linux-style sockaddr_in (what guest apps use)
struct linux_sockaddr_in {
    uint16_t sin_family;     // AF_INET
    uint16_t sin_port;       // Port in network byte order
    uint32_t sin_addr;       // IPv4 address
    char     sin_zero[8];    // Padding
};

// Convert Linux sockaddr_in to BSD/LwIP sockaddr_in
static void linux_to_bsd_sockaddr(const struct sockaddr *linux_addr,
                                   struct sockaddr_in *bsd_addr,
                                   socklen_t len) {
    if (len >= sizeof(struct linux_sockaddr_in)) {
        const struct linux_sockaddr_in *lin = (const struct linux_sockaddr_in *)linux_addr;

        // BSD format: len, family (8-bit each), then port/addr
        bsd_addr->sin_len = sizeof(struct sockaddr_in);
        bsd_addr->sin_family = (uint8_t)(lin->sin_family & 0xFF);
        bsd_addr->sin_port = lin->sin_port;
        bsd_addr->sin_addr.s_addr = lin->sin_addr;
        memset(bsd_addr->sin_zero, 0, sizeof(bsd_addr->sin_zero));
    }
}

// Convert BSD/LwIP sockaddr_in back to Linux format (for accept/recvfrom output)
static void bsd_to_linux_sockaddr(const struct sockaddr_in *bsd_addr,
                                   struct sockaddr *linux_addr,
                                   socklen_t *len) {
    if (*len >= sizeof(struct linux_sockaddr_in)) {
        struct linux_sockaddr_in *lin = (struct linux_sockaddr_in *)linux_addr;

        lin->sin_family = bsd_addr->sin_family;
        lin->sin_port = bsd_addr->sin_port;
        lin->sin_addr = bsd_addr->sin_addr.s_addr;
        memset(lin->sin_zero, 0, sizeof(lin->sin_zero));
        *len = sizeof(struct linux_sockaddr_in);
    }
}

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
    ESP_LOGD(TAG, "bind(fd=%d, namelen=%d)", s, (int)namelen);

    // Convert Linux sockaddr to BSD format for LwIP
    struct sockaddr_in bsd_addr;
    linux_to_bsd_sockaddr(name, &bsd_addr, namelen);

    ESP_LOGD(TAG, "bind: linux family=%d -> bsd len=%d family=%d port=%d",
             ((struct linux_sockaddr_in *)name)->sin_family,
             bsd_addr.sin_len, bsd_addr.sin_family,
             ntohs(bsd_addr.sin_port));

    int ret = lwip_bind(s, (struct sockaddr *)&bsd_addr, sizeof(bsd_addr));
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

    // Use BSD-format buffer for LwIP
    struct sockaddr_in bsd_addr;
    socklen_t bsd_len = sizeof(bsd_addr);

    int fd = lwip_accept(s, (struct sockaddr *)&bsd_addr, &bsd_len);
    if (fd < 0) {
        translate_lwip_errno();
        ESP_LOGD(TAG, "accept() failed: %s", strerror(errno));
        return -1;
    }

    // Convert output to Linux format if caller wants address
    if (addr && addrlen && *addrlen > 0) {
        bsd_to_linux_sockaddr(&bsd_addr, addr, addrlen);
    }

    ESP_LOGD(TAG, "accept() = %d", fd);
    return fd;
}

int shim_connect(int s, const struct sockaddr *name, socklen_t namelen) {
    ESP_LOGD(TAG, "connect(fd=%d)", s);

    // Convert Linux sockaddr to BSD format for LwIP
    struct sockaddr_in bsd_addr;
    linux_to_bsd_sockaddr(name, &bsd_addr, namelen);

    int ret = lwip_connect(s, (struct sockaddr *)&bsd_addr, sizeof(bsd_addr));
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
    // Convert Linux sockaddr to BSD format for LwIP
    struct sockaddr_in bsd_addr;
    if (to) {
        linux_to_bsd_sockaddr(to, &bsd_addr, tolen);
    }

    ssize_t ret = lwip_sendto(s, data, size, flags,
                              to ? (struct sockaddr *)&bsd_addr : NULL,
                              to ? sizeof(bsd_addr) : 0);
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
    // Use BSD-format buffer for LwIP
    struct sockaddr_in bsd_addr;
    socklen_t bsd_len = sizeof(bsd_addr);

    ssize_t ret = lwip_recvfrom(s, mem, len, flags,
                                from ? (struct sockaddr *)&bsd_addr : NULL,
                                from ? &bsd_len : NULL);
    if (ret < 0) {
        translate_lwip_errno();
        return ret;
    }

    // Convert output to Linux format if caller wants address
    if (from && fromlen && *fromlen > 0) {
        bsd_to_linux_sockaddr(&bsd_addr, from, fromlen);
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
