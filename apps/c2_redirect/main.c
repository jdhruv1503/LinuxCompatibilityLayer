/**
 * @file main.c
 * @brief C2 Redirect Test - Connects socket and redirects stdout
 *
 * This test:
 * 1. Creates a TCP socket
 * 2. Connects to QEMU gateway (10.0.2.2) on specified port
 * 3. Uses dup2() to redirect stdout to the socket
 * 4. Prints messages that should appear on both UART and network
 *
 * To test: python tools/build_and_run.py --sim --capture 12345
 */

#define AF_INET         2
#define SOCK_STREAM     1
#define STDOUT_FILENO   1

typedef unsigned char uint8_t;
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
extern int puts(const char *s);
extern int socket(int domain, int type, int protocol);
extern int connect(int s, const void *name, int namelen);
extern int send(int s, const void *data, int size, int flags);
extern int close(int fd);
extern int dup2(int oldfd, int newfd);

// htons - host to network short
static uint16_t my_htons(uint16_t h) {
    return ((h & 0xFF) << 8) | ((h >> 8) & 0xFF);
}

// Convert IP string "10.0.2.2" to uint32_t in network byte order
static uint32_t ip_to_uint32(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return ((uint32_t)a) | ((uint32_t)b << 8) | ((uint32_t)c << 16) | ((uint32_t)d << 24);
}

// Port to connect to (must match --capture argument)
#define C2_PORT 12345

__attribute__((visibility("default")))
int app_main(int argc, char *argv[]) {
    puts("=== C2 Redirect Test ===");
    puts("This test connects to the capture server and redirects stdout");
    puts("");

    // Create socket
    printf("Creating TCP socket...\n");
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("ERROR: socket() failed, returned %d\n", sock);
        return 1;
    }
    printf("Socket created: fd=%d\n", sock);

    // Connect to capture server via guestfwd (10.0.2.100)
    // QEMU guestfwd maps 10.0.2.100:PORT to host's 127.0.0.1:PORT
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = my_htons(C2_PORT);
    addr.sin_addr.s_addr = ip_to_uint32(10, 0, 2, 100);  // QEMU guestfwd target

    printf("Connecting to 10.0.2.100:%d (guestfwd -> host)...\n", C2_PORT);
    int ret = connect(sock, &addr, sizeof(addr));
    if (ret < 0) {
        printf("ERROR: connect() failed, returned %d\n", ret);
        printf("Make sure you're running with --capture %d\n", C2_PORT);
        close(sock);
        return 1;
    }
    printf("Connected successfully!\n\n");

    // Test direct send first
    const char *test_msg = "DIRECT_SEND: Hello from guest via send()\n";
    ret = send(sock, test_msg, 41, 0);
    printf("Direct send() returned: %d (expected: 41)\n", ret);

    // Now redirect stdout to the socket
    printf("\nCalling dup2(%d, %d) to redirect stdout...\n", sock, STDOUT_FILENO);
    ret = dup2(sock, STDOUT_FILENO);
    printf("dup2() returned: %d\n", ret);

    // After dup2, printf should go to BOTH UART and socket
    puts("\n=== Messages after dup2 (should appear on both UART and socket) ===");
    printf("REDIRECT_TEST: Line 1 via printf\n");
    printf("REDIRECT_TEST: Line 2 via printf\n");
    printf("REDIRECT_TEST: Line 3 via printf\n");
    puts("REDIRECT_TEST: Line via puts");

    // Print some structured data
    printf("\nStructured output test:\n");
    for (int i = 1; i <= 3; i++) {
        printf("  Item %d: value=%d\n", i, i * 100);
    }

    printf("\n=== End of test ===\n");

    // Clean up
    close(sock);

    return 0;
}
