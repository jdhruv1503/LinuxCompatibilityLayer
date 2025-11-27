/**
 * @file apps/c2_bot/main.c
 * @brief Command & Control Server Guest App
 *
 * Guest ELF application that listens on TCP port 9000 for incoming
 * ELF payloads, saves them to the filesystem, and streams stdout
 * back to the client using POSIX dup2() for redirection.
 *
 * Protocol:
 * 1. Master connects to Bot on port 9000
 * 2. Master sends 4-byte size header (little-endian)
 * 3. Master streams ELF binary data
 * 4. Bot saves to /linux/payload.elf
 * 5. Bot uses dup2() to redirect stdout/stderr to socket
 * 6. Bot sends completion message and closes connection
 */

/*==============================================================================
 * Manual POSIX Definitions (no system headers)
 *============================================================================*/

#define AF_INET         2
#define SOCK_STREAM     1
#define IPPROTO_TCP     6
#define INADDR_ANY      0
#define SOL_SOCKET      1
#define SO_REUSEADDR    2

#define STDOUT_FILENO   1
#define STDERR_FILENO   2

typedef unsigned int size_t;
typedef int ssize_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned int socklen_t;

struct in_addr {
    uint32_t s_addr;
};

// Standard Linux-style sockaddr_in (shim layer translates to BSD for LwIP)
struct sockaddr_in {
    uint16_t sin_family;   // AF_INET
    uint16_t sin_port;     // Port in network byte order
    struct in_addr sin_addr;
    char     sin_zero[8];  // Padding
};

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

/*==============================================================================
 * External Symbols (resolved by ELF loader from firmware)
 *============================================================================*/

// Standard I/O
extern int printf(const char *fmt, ...);
extern int puts(const char *s);

// File I/O
typedef struct FILE_s FILE;
extern FILE *fopen(const char *filename, const char *mode);
extern int fclose(FILE *stream);
extern size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

// Socket functions
extern int socket(int domain, int type, int protocol);
extern int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
extern int listen(int sockfd, int backlog);
extern int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
extern ssize_t recv(int sockfd, void *buf, size_t len, int flags);
extern ssize_t send(int sockfd, const void *buf, size_t len, int flags);
extern int close(int fd);
extern int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);

// POSIX dup2 for stdout redirection
extern int dup2(int oldfd, int newfd);

// POSIX execve for executing ELF files
extern int execve(const char *path, char *const argv[], char *const envp[]);

// POSIX waitpid for waiting on child processes
extern int waitpid(int pid, int *status, int options);

/*==============================================================================
 * Local Helper Functions
 *============================================================================*/

static int local_strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

static uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) |
           ((hostlong & 0xFF00) << 8) |
           ((hostlong >> 8) & 0xFF00) |
           ((hostlong >> 24) & 0xFF);
}

/*==============================================================================
 * Configuration
 *============================================================================*/

#define C2_PORT             9000
#define C2_RECV_BUF_SIZE    512
#define C2_PAYLOAD_PATH     "payload.elf"
#define C2_MAX_PAYLOAD_SIZE (1024 * 1024)  // 1MB max

/*==============================================================================
 * C2 Protocol Handler
 *============================================================================*/

static int c2_receive_payload(int client_sock) {
    // 1. Receive size header (4 bytes, little-endian)
    uint32_t payload_size = 0;
    ssize_t received = recv(client_sock, &payload_size, sizeof(payload_size), 0);

    if (received != (ssize_t)sizeof(payload_size)) {
        printf("[Bot] Failed to receive size header (got %d bytes)\n", (int)received);
        return -1;
    }

    printf("[Bot] Receiving payload: %u bytes\n", (unsigned int)payload_size);

    // Sanity check
    if (payload_size == 0 || payload_size > C2_MAX_PAYLOAD_SIZE) {
        printf("[Bot] Invalid payload size: %u\n", (unsigned int)payload_size);
        return -1;
    }

    // 2. Open file for writing
    FILE *f = fopen(C2_PAYLOAD_PATH, "wb");
    if (!f) {
        printf("[Bot] Failed to open %s for writing\n", C2_PAYLOAD_PATH);
        return -1;
    }

    // 3. Stream data to file
    uint8_t buf[C2_RECV_BUF_SIZE];
    uint32_t remaining = payload_size;
    uint32_t total_written = 0;

    while (remaining > 0) {
        int to_read = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        ssize_t len = recv(client_sock, buf, to_read, 0);

        if (len <= 0) {
            printf("[Bot] Connection lost during transfer\n");
            fclose(f);
            return -1;
        }

        size_t written = fwrite(buf, 1, len, f);
        if (written != (size_t)len) {
            printf("[Bot] Write error\n");
            fclose(f);
            return -1;
        }

        remaining -= len;
        total_written += written;
    }

    fclose(f);
    printf("[Bot] Payload saved: %u bytes to %s\n", total_written, C2_PAYLOAD_PATH);

    return 0;
}

static void c2_handle_client(int client_sock) {
    printf("[Bot] Client connected\n");

    // Receive and save payload
    if (c2_receive_payload(client_sock) != 0) {
        printf("[Bot] Failed to receive payload\n");
        close(client_sock);
        return;
    }

    // Use POSIX dup2() to redirect stdout/stderr to socket
    // This is the proper POSIX way - kernel handles it
    dup2(client_sock, STDOUT_FILENO);
    dup2(client_sock, STDERR_FILENO);

    // Now all printf output goes to the socket
    printf("[Bot] Payload received. Executing...\n");

    // Execute the payload using execve
    // Note: execve uses spawn model - it creates a new task and returns immediately
    // The path needs to be the full filesystem path: /linux/payload.elf
    // (The file was saved as "payload.elf" which gets translated to "/linux/payload.elf")
    char *argv[] = { "payload.elf", 0 };
    char *envp[] = { 0 };
    int exec_ret = execve("/linux/payload.elf", argv, envp);
    
    // In spawn model, execve returns 0 on success (task created)
    // The payload will run in a separate task and its stdout/stderr
    // will be redirected to the socket via dup2 we called earlier
    if (exec_ret < 0) {
        printf("[Bot] execve failed\n");
        close(client_sock);
        return;
    }
    
    printf("[Bot] Payload execution started\n");
    
    // Wait for the payload to complete using POSIX waitpid
    // waitpid(-1, ...) waits for any child process
    int child_status = 0;
    int waited_pid = waitpid(-1, &child_status, 0);
    
    if (waited_pid > 0) {
        printf("\n[Bot] Payload completed. Return code: %d\n", child_status);
    } else {
        printf("\n[Bot] waitpid failed\n");
    }
    
    // Close the socket after payload completes
    close(client_sock);
    printf("[Bot] Client disconnected\n");
}

/*==============================================================================
 * C2 Server Main Loop
 *============================================================================*/

static int c2_server_main(void) {
    // Create listening socket
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        printf("[Bot] Failed to create socket\n");
        return -1;
    }

    // Allow socket reuse
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind to port (Linux-style struct, shim layer handles BSD translation)
    struct sockaddr_in server_addr;
    // Zero-initialize to ensure sin_zero is clean
    for (int i = 0; i < (int)sizeof(server_addr); i++) {
        ((char*)&server_addr)[i] = 0;
    }
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(C2_PORT);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        printf("[Bot] Failed to bind to port %d\n", C2_PORT);
        close(listen_sock);
        return -1;
    }

    // Start listening
    if (listen(listen_sock, 1) < 0) {
        printf("[Bot] Failed to listen\n");
        close(listen_sock);
        return -1;
    }

    puts("============================================");
    printf("  C2 Server listening on port %d\n", C2_PORT);
    puts("============================================");

    // Main accept loop
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        puts("[Bot] Waiting for payload...");

        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);

        if (client_sock < 0) {
            puts("[Bot] Accept failed");
            continue;
        }

        // Handle client (blocking - one at a time)
        c2_handle_client(client_sock);
    }

    close(listen_sock);
    return 0;
}

/*==============================================================================
 * Guest App Entry Point
 *============================================================================*/

__attribute__((visibility("default")))
int app_main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    puts("========================================");
    puts("  C2 Bot - Command & Control Server");
    puts("========================================");
    printf("Listening for ELF payloads on port %d\n", C2_PORT);
    puts("Send payloads with: python tools/c2_master.py <elf> <ip>");
    puts("========================================");

    // Run the C2 server
    int ret = c2_server_main();

    puts("[Bot] C2 server shutting down");
    return ret;
}
