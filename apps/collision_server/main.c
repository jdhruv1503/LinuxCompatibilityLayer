/**
 * @file main.c
 * @brief Collision Server - HTTP to UDP Telemetry Bridge
 *
 * Guest ELF application that:
 * 1. Serves web dashboard (index.html) from LittleFS via HTTP
 * 2. Accepts POST /api/telemetry with JSON telemetry data
 * 3. Forwards telemetry to collision_guard via UDP localhost
 *
 * Architecture:
 * [Phone/Browser] --HTTP POST--> [Collision Server] --UDP--> [Collision Guard]
 *                 <--HTML/JS----
 */

/*==============================================================================
 * Manual POSIX Definitions
 *============================================================================*/

typedef unsigned int size_t;
typedef int ssize_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef int socklen_t;

#define AF_INET         2
#define SOCK_STREAM     1
#define SOCK_DGRAM      2
#define IPPROTO_TCP     6
#define INADDR_ANY      0
#define INADDR_LOOPBACK 0x7F000001
#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define O_RDONLY        0
#define O_WRONLY        1
#define O_CREAT         0x0200

#ifndef NULL
#define NULL ((void*)0)
#endif

/*==============================================================================
 * External Symbols
 *============================================================================*/

// Standard I/O
extern int printf(const char *fmt, ...);
extern int puts(const char *s);
extern int snprintf(char *str, size_t size, const char *fmt, ...);

// Socket functions
extern int socket(int domain, int type, int protocol);
extern int bind(int sockfd, const void *addr, socklen_t addrlen);
extern int listen(int sockfd, int backlog);
extern int accept(int sockfd, void *addr, socklen_t *addrlen);
extern ssize_t recv(int sockfd, void *buf, size_t len, int flags);
extern ssize_t send(int sockfd, const void *buf, size_t len, int flags);
extern ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
                      const void *dest_addr, socklen_t addrlen);
extern int close(int fd);
extern int setsockopt(int sockfd, int level, int optname,
                      const void *optval, socklen_t optlen);

// File I/O
extern int open(const char *path, int flags, ...);
extern ssize_t read(int fd, void *buf, size_t count);
extern ssize_t write(int fd, const void *buf, size_t count);

// String functions
extern void *memset(void *s, int c, size_t n);
extern void *memcpy(void *dest, const void *src, size_t n);
extern int strcmp(const char *s1, const char *s2);
extern int strncmp(const char *s1, const char *s2, size_t n);
extern char *strstr(const char *haystack, const char *needle);
extern size_t strlen(const char *s);
extern char *strchr(const char *s, int c);

// Math for parsing
extern double strtod(const char *str, char **endptr);

// POSIX execve for executing ELF files
extern int execve(const char *path, char *const argv[], char *const envp[]);

/*==============================================================================
 * Helper Functions
 *============================================================================*/

static uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

static uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0xFF) << 24) |
           ((hostlong & 0xFF00) << 8) |
           ((hostlong >> 8) & 0xFF00) |
           ((hostlong >> 24) & 0xFF);
}

// Simple integer to string
static int itoa_simple(int val, char *buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    int neg = 0;
    if (val < 0) {
        neg = 1;
        val = -val;
    }
    char tmp[16];
    int i = 0;
    while (val > 0) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int len = 0;
    if (neg) buf[len++] = '-';
    while (i > 0) buf[len++] = tmp[--i];
    buf[len] = '\0';
    return len;
}

/*==============================================================================
 * Constants
 *============================================================================*/

#define HTTP_PORT           80
#define UDP_PORT            8000
#define RECV_BUF_SIZE       2048
#define INDEX_HTML_PATH     "index.html"
#define COLLISION_LOG_PATH  "collisions.log"
#define MAX_HTML_SIZE       32768
#define MAX_LOG_SIZE        4096

/*==============================================================================
 * Data Structures
 *============================================================================*/

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char sin_zero[8];
};

// Telemetry packet (must match collision_guard)
typedef struct __attribute__((packed)) {
    float lat;
    float lon;
    float speed_mps;
    float heading;
    int car_id;
} telemetry_t;

/*==============================================================================
 * JSON Parser (Simple)
 *============================================================================*/

static float parse_json_float(const char *json, const char *key) {
    char search[64];
    int klen = strlen(key);

    // Build search pattern: "key":
    search[0] = '"';
    for (int i = 0; i < klen && i < 60; i++) {
        search[1 + i] = key[i];
    }
    search[1 + klen] = '"';
    search[2 + klen] = ':';
    search[3 + klen] = '\0';

    const char *pos = strstr(json, search);
    if (!pos) return 0.0f;

    pos += 3 + klen;  // Skip past "key":

    // Skip whitespace
    while (*pos == ' ' || *pos == '\t') pos++;

    return (float)strtod(pos, 0);
}

static int parse_json_int(const char *json, const char *key) {
    return (int)parse_json_float(json, key);
}

/*==============================================================================
 * HTTP Response Helpers
 *============================================================================*/

static const char *HTTP_200_HTML =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html\r\n"
    "Connection: close\r\n"
    "Content-Length: ";

static const char *HTTP_200_JSON =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Connection: close\r\n"
    "Content-Length: 2\r\n\r\nOK";

static const char *HTTP_404 =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Connection: close\r\n"
    "Content-Length: 9\r\n\r\nNot Found";

static const char *HTTP_OPTIONS =
    "HTTP/1.1 200 OK\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
    "Access-Control-Allow-Headers: Content-Type\r\n"
    "Content-Length: 0\r\n\r\n";

/*==============================================================================
 * Global State
 *============================================================================*/

static int g_udp_sock = -1;
static struct sockaddr_in g_udp_dest;
static char g_html_content[MAX_HTML_SIZE];
static int g_html_len = 0;
static char g_collision_log[MAX_LOG_SIZE];
static int g_log_len = 0;
static char g_request_buf[RECV_BUF_SIZE];

/*==============================================================================
 * Collision Log Persistence
 *============================================================================*/

static void load_collision_log(void) {
    int fd = open(COLLISION_LOG_PATH, O_RDONLY);
    if (fd >= 0) {
        g_log_len = read(fd, g_collision_log, MAX_LOG_SIZE - 1);
        if (g_log_len < 0) g_log_len = 0;
        g_collision_log[g_log_len] = '\0';
        close(fd);
        printf("Loaded %d bytes from collision log\n", g_log_len);
    } else {
        g_log_len = 0;
        g_collision_log[0] = '\0';
    }
}

static void save_collision_log(void) {
    int fd = open(COLLISION_LOG_PATH, O_WRONLY);
    if (fd >= 0) {
        write(fd, g_collision_log, g_log_len);
        close(fd);
    }
}

static void append_collision_event(const char *json_event) {
    // Parse time and msg from JSON
    const char *time_str = strstr(json_event, "\"time\":");
    const char *msg_str = strstr(json_event, "\"msg\":");
    const char *type_str = strstr(json_event, "\"type\":");

    if (!time_str || !msg_str) return;

    // Simple extraction (find values after ":")
    time_str = strchr(time_str, ':');
    msg_str = strchr(msg_str, ':');
    if (!time_str || !msg_str) return;
    time_str++;  // Skip ':'
    msg_str++;

    // Skip whitespace and quotes
    while (*time_str == ' ' || *time_str == '"') time_str++;
    while (*msg_str == ' ' || *msg_str == '"') msg_str++;

    // Find end of strings
    char time_buf[32] = {0};
    char msg_buf[128] = {0};
    int i = 0;
    while (*time_str && *time_str != '"' && i < 30) {
        time_buf[i++] = *time_str++;
    }
    i = 0;
    while (*msg_str && *msg_str != '"' && i < 126) {
        msg_buf[i++] = *msg_str++;
    }

    // Append to log as simple text
    char entry[200];
    int entry_len = snprintf(entry, sizeof(entry), "[%s] %s\n", time_buf, msg_buf);

    // Ensure space in log (rotate if needed)
    if (g_log_len + entry_len >= MAX_LOG_SIZE - 1) {
        // Simple rotation: keep last half
        int half = g_log_len / 2;
        for (int j = 0; j < half; j++) {
            g_collision_log[j] = g_collision_log[half + j];
        }
        g_log_len = half;
    }

    // Append entry
    for (int j = 0; entry[j] && g_log_len < MAX_LOG_SIZE - 1; j++) {
        g_collision_log[g_log_len++] = entry[j];
    }
    g_collision_log[g_log_len] = '\0';

    // Save to filesystem
    save_collision_log();
}

/*==============================================================================
 * HTTP Request Handler
 *============================================================================*/

static void handle_request(int client_sock, char *request, int req_len) {
    // Use a static response buffer so we don't blow the tiny FreeRTOS task stack
    static char response_buf[MAX_LOG_SIZE + 256];

    // Parse request line
    char method[16] = {0};
    char path[128] = {0};
    int i = 0, j = 0;

    // Extract method
    while (i < req_len && request[i] != ' ' && j < 15) {
        method[j++] = request[i++];
    }
    method[j] = '\0';

    // Skip space
    while (i < req_len && request[i] == ' ') i++;

    // Extract path
    j = 0;
    while (i < req_len && request[i] != ' ' && request[i] != '?' && j < 127) {
        path[j++] = request[i++];
    }
    path[j] = '\0';

    printf("[HTTP] %s %s\n", method, path);

    // Handle OPTIONS (CORS preflight)
    if (strcmp(method, "OPTIONS") == 0) {
        send(client_sock, HTTP_OPTIONS, strlen(HTTP_OPTIONS), 0);
        return;
    }

    // Handle GET /
    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        if (g_html_len > 0) {
            char header[256];
            char len_str[16];
            itoa_simple(g_html_len, len_str);

            // Build response header
            int hlen = 0;
            const char *h200 = HTTP_200_HTML;
            while (*h200) header[hlen++] = *h200++;
            for (int k = 0; len_str[k]; k++) header[hlen++] = len_str[k];
            header[hlen++] = '\r';
            header[hlen++] = '\n';
            header[hlen++] = '\r';
            header[hlen++] = '\n';

            send(client_sock, header, hlen, 0);
            send(client_sock, g_html_content, g_html_len, 0);
        } else {
            send(client_sock, HTTP_404, strlen(HTTP_404), 0);
        }
        return;
    }

    // Handle POST /api/telemetry
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/telemetry") == 0) {
        // Find request body (after \r\n\r\n)
        const char *body = strstr(request, "\r\n\r\n");
        if (body) {
            body += 4;

            // Parse JSON telemetry
            telemetry_t pkt;
            pkt.lat = parse_json_float(body, "lat");
            pkt.lon = parse_json_float(body, "lon");
            pkt.speed_mps = parse_json_float(body, "speed_mps");
            pkt.heading = parse_json_float(body, "heading");
            pkt.car_id = parse_json_int(body, "car_id");

            // Forward to collision_guard via UDP
            if (g_udp_sock >= 0) {
                sendto(g_udp_sock, &pkt, sizeof(pkt), 0,
                       &g_udp_dest, sizeof(g_udp_dest));
            }
        }

        send(client_sock, HTTP_200_JSON, strlen(HTTP_200_JSON), 0);
        return;
    }

    // Handle POST /api/collision - log a collision event
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/collision") == 0) {
        const char *body = strstr(request, "\r\n\r\n");
        if (body) {
            body += 4;
            append_collision_event(body);
            printf("[Collision] Event logged\n");
        }
        send(client_sock, HTTP_200_JSON, strlen(HTTP_200_JSON), 0);
        return;
    }

    // Handle GET /api/collisions - return collision log as JSON
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/collisions") == 0) {
        // Build JSON response with events array
        int resp_len = snprintf(response_buf, sizeof(response_buf),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n"
            "{\"events\":[");

        // Parse log entries and convert to JSON array
        int first = 1;
        char *line = g_collision_log;
        while (*line) {
            // Find line end
            char *end = line;
            while (*end && *end != '\n') end++;

            // Skip empty lines
            if (end > line) {
                // Extract timestamp and message
                char time_buf[32] = {0};
                char msg_buf[128] = {0};
                const char *t = line;

                // Parse [timestamp] message format
                if (*t == '[') {
                    t++;
                    int i = 0;
                    while (*t && *t != ']' && i < 30) {
                        time_buf[i++] = *t++;
                    }
                    if (*t == ']') t++;
                    while (*t == ' ') t++;
                    i = 0;
                    while (t < end && i < 126) {
                        msg_buf[i++] = *t++;
                    }

                    // Determine type from message
                    const char *type = strstr(msg_buf, "ALERT") ? "collision" : "clear";

                    if (!first) {
                        response_buf[resp_len++] = ',';
                    }
                    first = 0;

                    resp_len += snprintf(response_buf + resp_len, sizeof(response_buf) - resp_len,
                        "{\"type\":\"%s\",\"time\":\"%s\",\"msg\":\"%s\"}",
                        type, time_buf, msg_buf);
                }
            }

            // Move to next line
            if (*end) end++;
            line = end;
        }

        resp_len += snprintf(response_buf + resp_len, sizeof(response_buf) - resp_len, "]}");
        send(client_sock, response_buf, resp_len, 0);
        return;
    }

    // Default: 404
    send(client_sock, HTTP_404, strlen(HTTP_404), 0);
}

/*==============================================================================
 * Main Entry Point
 *============================================================================*/

__attribute__((visibility("default")))
int app_main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    puts("");
    puts("============================================");
    puts("  Collision Server - Telemetry Bridge");
    puts("============================================");
    printf("HTTP server on port %d\n", HTTP_PORT);
    printf("UDP forwarding to localhost:%d\n", UDP_PORT);
    puts("");

    // Load HTML content from filesystem
    puts("Loading dashboard from LittleFS...");
    int html_fd = open(INDEX_HTML_PATH, O_RDONLY);
    if (html_fd >= 0) {
        g_html_len = read(html_fd, g_html_content, MAX_HTML_SIZE - 1);
        close(html_fd);
        if (g_html_len > 0) {
            g_html_content[g_html_len] = '\0';
            printf("Loaded %d bytes from %s\n", g_html_len, INDEX_HTML_PATH);
        }
    }

    if (g_html_len <= 0) {
        puts("WARNING: Could not load index.html from filesystem");
        puts("Dashboard will not be available");
    }

    // Load collision log from filesystem
    load_collision_log();

    // Create UDP socket for forwarding
    g_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_sock >= 0) {
        memset(&g_udp_dest, 0, sizeof(g_udp_dest));
        g_udp_dest.sin_family = AF_INET;
        g_udp_dest.sin_port = htons(UDP_PORT);
        g_udp_dest.sin_addr = htonl(INADDR_LOOPBACK);
        puts("UDP forwarder initialized");
    } else {
        puts("WARNING: Failed to create UDP socket");
    }

    // Start collision guard logic in background
    puts("Spawning /linux/collision_guard.elf...");
    char *guard_argv[] = {"collision_guard.elf", NULL};
    int guard_ret = execve("/linux/collision_guard.elf", guard_argv, NULL);
    if (guard_ret == 0) {
        puts("Collision Guard spawned successfully");
    } else {
        printf("Failed to spawn Collision Guard: %d\n", guard_ret);
    }

    // Create TCP listening socket
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        puts("ERROR: Failed to create TCP socket");
        return 1;
    }

    // Allow address reuse
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind to port
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(HTTP_PORT);
    server_addr.sin_addr = INADDR_ANY;

    if (bind(listen_sock, &server_addr, sizeof(server_addr)) < 0) {
        printf("ERROR: Failed to bind to port %d\n", HTTP_PORT);
        close(listen_sock);
        return 1;
    }

    // Listen for connections
    if (listen(listen_sock, 5) < 0) {
        puts("ERROR: Listen failed");
        close(listen_sock);
        return 1;
    }

    puts("");
    puts("============================================");
    printf("Server ready at http://localhost:%d/\n", HTTP_PORT);
    puts("============================================");
    puts("");

    // Main accept loop
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int client_sock = accept(listen_sock, &client_addr, &addr_len);
        if (client_sock < 0) {
            continue;
        }

        // Receive request
        ssize_t received = recv(client_sock, g_request_buf, RECV_BUF_SIZE - 1, 0);
        if (received > 0) {
            g_request_buf[received] = '\0';
            handle_request(client_sock, g_request_buf, received);
        }

        close(client_sock);
    }

    close(listen_sock);
    if (g_udp_sock >= 0) close(g_udp_sock);

    return 0;
}
