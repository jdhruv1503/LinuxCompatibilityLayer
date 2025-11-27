// apps/tcp_client/main.c

// Manually define standard constants and structs if headers are missing
// in the bare-metal toolchain environment.

#define AF_INET         2
#define SOCK_STREAM     1
#define IPPROTO_TCP     6

typedef unsigned int socklen_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char     sin_zero[8];
};

// External Symbols (provided by Host via Shim)
extern int printf(const char *fmt, ...);
extern int socket(int domain, int type, int protocol);
extern int connect(int s, const struct sockaddr *name, int namelen);
extern int send(int s, const void *data, int size, int flags);
extern int recv(int s, void *mem, int len, int flags);
extern int close(int fd);
extern int strlen(const char *s);

// Simple implementation of htons (Host to Network Short)
static uint16_t my_htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort & 0xFF00) >> 8);
}

// Simple implementation of inet_addr (IP string to uint32_t)
// Note: This parses "A.B.C.D"
static uint32_t my_inet_addr(const char *cp) {
    uint32_t ip = 0;
    int octet = 0;
    int shift = 0;
    
    while (*cp) {
        if (*cp >= '0' && *cp <= '9') {
            octet = octet * 10 + (*cp - '0');
        } else if (*cp == '.') {
            ip |= (octet << shift);
            shift += 8;
            octet = 0;
        }
        cp++;
    }
    ip |= (octet << shift);
    return ip;
}

// Entry point
__attribute__((visibility("default")))
int app_main(int argc, char *argv[]) {
    printf("TCP Client Test Started\n");

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        printf("socket() failed\n");
        return 1;
    }
    printf("Socket created: %d\n", sock);

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = my_htons(80);
    // example.com is 93.184.216.34 -> 34.216.184.93 in little endian (but network byte order is big endian)
    // Wait, LwIP expects network byte order.
    // "93.184.216.34"
    // inet_addr usually returns Network Byte Order.
    // Let's use a simple parsing that constructs it.
    // 93 = 0x5D, 184 = 0xB8, 216 = 0xD8, 34 = 0x22
    // IP: 93.184.216.34
    // Network Order (Big Endian): 0x5D 0xB8 0xD8 0x22
    // U32 (Little Endian Machine): 0x22D8B85D
    
    // Manual assignment to be safe
    uint8_t *ip_bytes = (uint8_t*)&server.sin_addr.s_addr;
    ip_bytes[0] = 93;
    ip_bytes[1] = 184;
    ip_bytes[2] = 216;
    ip_bytes[3] = 34;

    printf("Connecting to 93.184.216.34:80...\n");
    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) < 0) {
        printf("connect() failed\n");
        close(sock);
        return 1;
    }
    printf("Connected!\n");

    const char *request = "GET / HTTP/1.0\r\nHost: example.com\r\n\r\n";
    send(sock, request, strlen(request), 0);
    printf("Request sent.\n");

    char buf[512];
    int len = recv(sock, buf, sizeof(buf)-1, 0);
    if (len > 0) {
        buf[len] = '\0';
        printf("Response received (%d bytes):\n", len);
        // Print first 100 chars
        for(int i=0; i<100 && i<len; i++) {
             printf("%c", buf[i]);
        }
        printf("\n...\n");
    } else {
        printf("Recv failed or closed: %d\n", len);
    }

    close(sock);
    return 0;
}