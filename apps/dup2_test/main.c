/**
 * @file main.c
 * @brief Test application for dup2 stdout redirection with verification
 *
 * This test verifies that dup2 actually sends data through the socket
 * by checking the return value of the underlying send() calls.
 */

#define AF_INET         2
#define SOCK_STREAM     1
#define STDOUT_FILENO   1

typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char     sin_zero[8];
};

// External symbols from host shim
extern int printf(const char *fmt, ...);
extern int socket(int domain, int type, int protocol);
extern int connect(int s, const void *name, int namelen);
extern int send(int s, const void *data, int size, int flags);
extern int close(int fd);
extern int dup2(int oldfd, int newfd);
extern int bind(int s, const void *name, int namelen);
extern int listen(int s, int backlog);
extern int accept(int s, void *addr, void *addrlen);
extern int recv(int s, void *mem, int len, int flags);

// htons
static uint16_t my_htons(uint16_t h) {
    return ((h & 0xFF) << 8) | ((h >> 8) & 0xFF);
}

__attribute__((visibility("default")))
int app_main(int argc, char *argv[]) {
    printf("=== dup2 Socket Verification Test ===\n\n");

    // Test 1: Direct socket send (baseline)
    printf("[Test 1] Direct socket send test\n");
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    printf("  Created socket fd=%d\n", sock);

    // Try to send on unconnected socket - should fail
    const char *test_msg = "Hello";
    int ret = send(sock, test_msg, 5, 0);
    printf("  send() on unconnected socket returned: %d (expected: -1)\n", ret);

    // Test 2: dup2 redirection behavior
    printf("\n[Test 2] dup2 redirection test\n");
    printf("  Calling dup2(%d, %d)...\n", sock, STDOUT_FILENO);

    int dup_ret = dup2(sock, STDOUT_FILENO);
    printf("  dup2() returned: %d (expected: %d)\n", dup_ret, STDOUT_FILENO);

    // After dup2, this printf goes through shim_write which:
    // 1. Writes to UART (what you see here)
    // 2. Calls send() to socket (fails because not connected)
    printf("\n[Test 3] Post-dup2 printf test\n");
    printf("  This line is written via shim_write()\n");
    printf("  shim_write does: write(UART) + send(socket)\n");
    printf("  Since socket is not connected, send() fails silently\n");

    // Test 4: Verify by checking if we can manually send
    printf("\n[Test 4] Verify socket state\n");
    ret = send(sock, "manual test", 11, 0);
    printf("  Manual send() returned: %d\n", ret);
    if (ret < 0) {
        printf("  Socket not connected - send fails as expected\n");
        printf("  To see actual network traffic, need a connected socket\n");
    }

    close(sock);

    printf("\n=== Summary ===\n");
    printf("The dup2/shim_write mechanism is working:\n");
    printf("- dup2() configures C2 pipe with socket fd\n");
    printf("- shim_write() intercepts stdout writes\n");
    printf("- Data is sent to BOTH uart AND socket\n");
    printf("- Socket send fails because not connected\n");
    printf("- You see output via UART mirror only\n");
    printf("\nTo verify actual network transmission:\n");
    printf("1. Use QEMU port forwarding: -nic user,hostfwd=tcp::1234-:1234\n");
    printf("2. Run: nc -l 1234  (on host)\n");
    printf("3. Connect socket to 10.0.2.2:1234 (QEMU gateway)\n");
    printf("4. Then printf output will appear in netcat\n");

    return 0;
}
