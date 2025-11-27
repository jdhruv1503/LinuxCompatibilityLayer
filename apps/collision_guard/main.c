/**
 * @file main.c
 * @brief Collision Guard - V2X Collision Avoidance Logic
 *
 * Receives vehicle telemetry via UDP, calculates Time-To-Collision (TTC),
 * and triggers /dev/buzzer alarm when crash is imminent.
 *
 * This demonstrates:
 * - UDP socket programming in guest ELF
 * - Math library usage (sinf, cosf, sqrtf)
 * - Hardware control via VFS device driver
 */

/*==============================================================================
 * Manual POSIX Definitions (no system headers)
 *============================================================================*/

typedef unsigned int size_t;
typedef int ssize_t;
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

#define AF_INET     2
#define SOCK_DGRAM  2
#define INADDR_ANY  0
#define O_WRONLY    1
#define O_RDWR      2

/*==============================================================================
 * External Symbols (resolved by ELF loader from firmware)
 *============================================================================*/

// Standard I/O
extern int printf(const char *fmt, ...);
extern int puts(const char *s);

// Socket functions
extern int socket(int domain, int type, int protocol);
extern int bind(int sockfd, const void *addr, int addrlen);
extern ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                        void *src_addr, int *addrlen);
extern int close(int fd);

// File I/O
extern int open(const char *path, int flags, ...);
extern ssize_t write(int fd, const void *buf, size_t count);
extern int ioctl(int fd, int cmd, ...);

// Math functions
extern float sinf(float x);
extern float cosf(float x);
extern float sqrtf(float x);
extern float fabsf(float x);

/*==============================================================================
 * Local Helper Functions
 *============================================================================*/

static uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort >> 8) & 0xFF);
}

/*==============================================================================
 * Constants
 *============================================================================*/

#define UDP_PORT               8000
#define M_PI                   3.14159265358979323846f
#define IOCTL_BUZZER_SET_FREQ  0x2001

// Collision thresholds
#define TTC_THRESHOLD          2.0f    // seconds
#define DISTANCE_THRESHOLD     10.0f   // meters

/*==============================================================================
 * Data Structures
 *============================================================================*/

// sockaddr_in structure (Linux-style)
struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char sin_zero[8];
};

// Telemetry packet (must match what the web server sends)
typedef struct __attribute__((packed)) {
    float lat;
    float lon;
    float speed_mps;
    float heading;
    int car_id;
} telemetry_t;

// 2D vector for position/velocity
typedef struct {
    float x;
    float y;
} vec2_t;

/*==============================================================================
 * Coordinate Conversion
 *============================================================================*/

/**
 * @brief Convert geographic coordinates to local Cartesian coordinates
 *
 * Uses equirectangular approximation - accurate for small distances.
 *
 * @param lat Latitude in degrees
 * @param lon Longitude in degrees
 * @param ref_lat Reference latitude (origin)
 * @param ref_lon Reference longitude (origin)
 * @return Position in meters relative to reference point
 */
static vec2_t geo_to_local(float lat, float lon, float ref_lat, float ref_lon) {
    const float R = 6371000.0f;  // Earth radius in meters
    float lat_rad = ref_lat * (M_PI / 180.0f);

    float x = (lon - ref_lon) * (M_PI / 180.0f) * R * cosf(lat_rad);
    float y = (lat - ref_lat) * (M_PI / 180.0f) * R;

    vec2_t result = {x, y};
    return result;
}

/**
 * @brief Convert heading angle to velocity vector
 *
 * @param speed Speed in m/s
 * @param heading_deg Heading in degrees (0 = North, 90 = East)
 * @return Velocity vector in m/s
 */
static vec2_t heading_to_velocity(float speed, float heading_deg) {
    float rad = heading_deg * (M_PI / 180.0f);
    vec2_t result = {
        speed * sinf(rad),   // East component
        speed * cosf(rad)    // North component
    };
    return result;
}

/**
 * @brief Calculate Euclidean distance between two points
 */
static float distance(vec2_t a, vec2_t b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    return sqrtf(dx * dx + dy * dy);
}

/**
 * @brief Calculate magnitude of relative velocity vector
 */
static float relative_speed(vec2_t v1, vec2_t v2) {
    float dvx = v2.x - v1.x;
    float dvy = v2.y - v1.y;
    return sqrtf(dvx * dvx + dvy * dvy);
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
    puts("  Collision Guard - V2X Safety System");
    puts("============================================");
    printf("Listening for telemetry on UDP port %d\n", UDP_PORT);
    puts("");

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        puts("ERROR: Failed to create UDP socket");
        return 1;
    }

    // Bind to port
    struct sockaddr_in addr;
    for (int i = 0; i < (int)sizeof(addr); i++) {
        ((char*)&addr)[i] = 0;
    }
    addr.sin_family = AF_INET;
    addr.sin_port = htons(UDP_PORT);
    addr.sin_addr = INADDR_ANY;

    if (bind(sock, &addr, sizeof(addr)) < 0) {
        puts("ERROR: Failed to bind UDP socket");
        close(sock);
        return 1;
    }

    printf("UDP socket bound to port %d\n", UDP_PORT);

    // Open buzzer device
    int buzzer_fd = open("/dev/buzzer", O_WRONLY);
    if (buzzer_fd < 0) {
        puts("WARNING: /dev/buzzer not available (virtual mode)");
    } else {
        // Set alarm frequency to 2kHz
        ioctl(buzzer_fd, IOCTL_BUZZER_SET_FREQ, 2000);
        puts("Buzzer initialized at 2000 Hz");
    }

    puts("");
    puts("Waiting for telemetry data...");
    puts("(Open web dashboard and start simulation)");
    puts("");

    // State tracking
    vec2_t pos_ego = {0, 0};
    vec2_t vel_ego = {0, 0};
    vec2_t pos_target = {0, 0};
    vec2_t vel_target = {0, 0};

    float ref_lat = 0, ref_lon = 0;
    int has_reference = 0;
    int alarm_active = 0;
    int packet_count = 0;

    // Main loop
    telemetry_t pkt;
    while (1) {
        ssize_t n = recvfrom(sock, &pkt, sizeof(pkt), 0, 0, 0);
        if (n <= 0) continue;

        packet_count++;

        // Establish reference point on first packet
        if (!has_reference) {
            ref_lat = pkt.lat;
            ref_lon = pkt.lon;
            has_reference = 1;
            printf("Reference point set: %.6f, %.6f\n\n", ref_lat, ref_lon);
        }

        // Convert to local Cartesian coordinates
        vec2_t pos = geo_to_local(pkt.lat, pkt.lon, ref_lat, ref_lon);
        vec2_t vel = heading_to_velocity(pkt.speed_mps, pkt.heading);

        // Update state based on car ID
        if (pkt.car_id == 0) {
            pos_ego = pos;
            vel_ego = vel;
        } else {
            pos_target = pos;
            vel_target = vel;
        }

        // Calculate collision metrics
        float dist = distance(pos_ego, pos_target);
        float rel_vel = relative_speed(vel_ego, vel_target);

        // Time To Collision (simplified linear approximation)
        float ttc = (rel_vel > 0.1f) ? (dist / rel_vel) : 999.0f;

        // Collision detection
        int imminent = (ttc < TTC_THRESHOLD && dist < DISTANCE_THRESHOLD);

        // Debug output (every 10th packet to reduce spam)
        if (packet_count % 10 == 0 || imminent) {
            printf("Car%d: pos(%.1f,%.1f)m vel(%.1f,%.1f)m/s | ",
                   pkt.car_id, pos.x, pos.y, vel.x, vel.y);
            printf("Dist=%.1fm TTC=%.1fs %s\n",
                   dist, ttc,
                   imminent ? "[ALERT!]" : "");
        }

        // Control buzzer
        if (imminent && !alarm_active) {
            puts("");
            puts("!!! COLLISION IMMINENT !!!");
            puts("");
            if (buzzer_fd >= 0) {
                write(buzzer_fd, "1", 1);
            }
            alarm_active = 1;
        } else if (!imminent && alarm_active) {
            puts("--- Collision risk cleared ---");
            if (buzzer_fd >= 0) {
                write(buzzer_fd, "0", 1);
            }
            alarm_active = 0;
        }
    }

    // Cleanup (unreachable in normal operation)
    if (buzzer_fd >= 0) {
        write(buzzer_fd, "0", 1);
        close(buzzer_fd);
    }
    close(sock);

    return 0;
}
