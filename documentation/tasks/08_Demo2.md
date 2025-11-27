## **1\. Objective**

This document outlines the "Edge-OS Collision Avoidance" demo. This scenario demonstrates the system's capacity for "Cyber-Physical" interaction, where a high-level Linux application performs real-time vector mathematics to detect collision threats and triggers physical hardware (a buzzer/alarm) via a custom kernel driver.

## **2\. Architecture Overview**

The system mimics a V2X (Vehicle-to-Everything) On-Board Unit.

1. **Sensors (Simulated):** A Web Dashboard running on a smartphone/PC acts as the GPS/IMU source, sending telemetry to the ESP32.  
2. **Base OS (Firmware):** Receives telemetry via HTTP/WebSockets and forwards it to the Linux App via a localhost UDP socket.  
3. **Edge App (Linux ELF):** Calculates relative velocity and Time-To-Collision (TTC).  
4. **Actuator (Hardware):** If a crash is imminent, the App writes to /dev/buzzer to trigger a physical alarm.

## **3\. Component 1: The Hardware Driver (/dev/buzzer)**

We implement a VFS driver that wraps the ESP32's LEDC (PWM) peripheral. This allows the Linux app to control the frequency and state of a buzzer using standard file I/O.

### **3.1 Driver Implementation (main/drivers/vfs\_buzzer.c)**

\#include \<stdio.h\>  
\#include \<string.h\>  
\#include "esp\_vfs.h"  
\#include "driver/ledc.h"  
\#include "esp\_err.h"

\#define BUZZER\_TIMER       LEDC\_TIMER\_0  
\#define BUZZER\_MODE        LEDC\_LOW\_SPEED\_MODE  
\#define BUZZER\_CHANNEL     LEDC\_CHANNEL\_0  
\#define BUZZER\_DUTY\_RES    LEDC\_TIMER\_13\_BIT  
\#define BUZZER\_OUTPUT\_IO   (5) // GPIO pin for Buzzer/LED  
\#define BUZZER\_FREQUENCY   (4000)

// Custom IOCTL Command  
\#define IOCTL\_BUZZER\_SET\_FREQ  0x2001

static int buzzer\_open(const char \*path, int flags, int mode) {  
    // Initialize hardware on first open  
    ledc\_timer\_config\_t ledc\_timer \= {  
        .speed\_mode       \= BUZZER\_MODE,  
        .timer\_num        \= BUZZER\_TIMER,  
        .duty\_resolution  \= BUZZER\_DUTY\_RES,  
        .freq\_hz          \= BUZZER\_FREQUENCY,  
        .clk\_cfg          \= LEDC\_AUTO\_CLK  
    };  
    ledc\_timer\_config(\&ledc\_timer);

    ledc\_channel\_config\_t ledc\_channel \= {  
        .speed\_mode     \= BUZZER\_MODE,  
        .channel        \= BUZZER\_CHANNEL,  
        .timer\_sel      \= BUZZER\_TIMER,  
        .intr\_type      \= LEDC\_INTR\_DISABLE,  
        .gpio\_num       \= BUZZER\_OUTPUT\_IO,  
        .duty           \= 0, // Start off  
        .hpoint         \= 0  
    };  
    ledc\_channel\_config(\&ledc\_channel);  
      
    return 0; // Return virtual FD 0  
}

static ssize\_t buzzer\_write(int fd, const void \*data, size\_t size) {  
    const char \*cmd \= (const char \*)data;  
    uint32\_t duty \= 0;  
      
    // "1" turns it on (50% duty), anything else turns it off  
    if (size \> 0 && cmd\[0\] \== '1') {  
        duty \= 4000; // \~50% of 13-bit (8192)  
    }

    ledc\_set\_duty(BUZZER\_MODE, BUZZER\_CHANNEL, duty);  
    ledc\_update\_duty(BUZZER\_MODE, BUZZER\_CHANNEL);  
      
    return size;  
}

static int buzzer\_ioctl(int fd, int cmd, va\_list args) {  
    if (cmd \== IOCTL\_BUZZER\_SET\_FREQ) {  
        int freq \= va\_arg(args, int);  
        ledc\_set\_freq(BUZZER\_MODE, BUZZER\_TIMER, freq);  
        return 0;  
    }  
    return \-1;  
}

// Registration Wrapper  
void mount\_buzzer\_driver(void) {  
    esp\_vfs\_t vfs \= {  
        .flags \= ESP\_VFS\_FLAG\_DEFAULT,  
        .open \= \&buzzer\_open,  
        .write \= \&buzzer\_write,  
        .ioctl \= \&buzzer\_ioctl,  
    };  
    ESP\_ERROR\_CHECK(esp\_vfs\_register("/dev/buzzer", \&vfs, NULL));  
}

## **4\. Component 2: The Linux Application (collision\_guard.c)**

This ELF binary performs the logic. It listens for telemetry packets and calculates if a collision is imminent. Note the use of standard math and networking headers.

### **4.1 Source Code**

\#include \<stdio.h\>  
\#include \<stdlib.h\>  
\#include \<string.h\>  
\#include \<unistd.h\>  
\#include \<fcntl.h\>  
\#include \<math.h\>  
\#include \<sys/socket.h\>  
\#include \<netinet/in.h\>  
\#include \<sys/ioctl.h\>

\#define PORT 8000  
\#define IOCTL\_BUZZER\_SET\_FREQ  0x2001

// Telemetry Packet Structure  
struct packet {  
    float lat;  
    float lon;  
    float speed\_mps; // Meters per second  
    float heading;   // Degrees  
    int car\_id;  
};

// Simple vector struct  
typedef struct {  
    float x;  
    float y;  
} vec2\_t;

// Convert Lat/Lon to Meters relative to a reference point (Simulated Equirectangular)  
vec2\_t geo\_to\_meters(float lat, float lon, float ref\_lat, float ref\_lon) {  
    float R \= 6371000.0; // Earth radius in meters  
    float x \= (lon \- ref\_lon) \* (M\_PI / 180.0f) \* R \* cosf(ref\_lat \* (M\_PI / 180.0f));  
    float y \= (lat \- ref\_lat) \* (M\_PI / 180.0f) \* R;  
    return (vec2\_t){x, y};  
}

int main() {  
    printf("Starting Collision Guard Logic...\\n");

    // 1\. Setup UDP Socket  
    int sockfd \= socket(AF\_INET, SOCK\_DGRAM, 0);  
    if (sockfd \< 0\) {  
        perror("Socket creation failed");  
        return 1;  
    }

    struct sockaddr\_in servaddr \= {  
        .sin\_family \= AF\_INET,  
        .sin\_addr.s\_addr \= INADDR\_ANY,  
        .sin\_port \= htons(PORT)  
    };

    if (bind(sockfd, (const struct sockaddr \*)\&servaddr, sizeof(servaddr)) \< 0\) {  
        perror("Bind failed");  
        return 1;  
    }

    // 2\. Open Buzzer  
    int bz\_fd \= open("/dev/buzzer", O\_WRONLY);  
    if (bz\_fd \< 0\) perror("Warning: Buzzer not found");

    printf("Listening for telemetry on port %d...\\n", PORT);

    struct packet p;  
    vec2\_t pos\_ego \= {0,0}, pos\_target \= {0,0};  
    vec2\_t vel\_ego \= {0,0}, vel\_target \= {0,0};  
    int has\_ref \= 0;  
    float ref\_lat \= 0, ref\_lon \= 0;

    while (1) {  
        int n \= recvfrom(sockfd, \&p, sizeof(p), 0, NULL, NULL);  
        if (n \> 0\) {  
            // Establish reference point for local Cartesian grid  
            if (\!has\_ref) {  
                ref\_lat \= p.lat;  
                ref\_lon \= p.lon;  
                has\_ref \= 1;  
            }

            // Convert to Local Cartesian  
            vec2\_t pos \= geo\_to\_meters(p.lat, p.lon, ref\_lat, ref\_lon);  
              
            // Calculate Velocity Vector (Speed \+ Heading)  
            float rad \= p.heading \* (M\_PI / 180.0f);  
            vec2\_t vel \= {  
                .x \= p.speed\_mps \* sinf(rad),  
                .y \= p.speed\_mps \* cosf(rad)  
            };

            // Update internal state  
            if (p.car\_id \== 0\) { // EGO Car (Us)  
                pos\_ego \= pos;  
                vel\_ego \= vel;  
            } else { // TARGET Car (Them)  
                pos\_target \= pos;  
                vel\_target \= vel;  
            }

            // \--- COLLISION LOGIC \---  
              
            // 1\. Relative Position vector  
            float dx \= pos\_target.x \- pos\_ego.x;  
            float dy \= pos\_target.y \- pos\_ego.y;  
            float dist \= sqrtf(dx\*dx \+ dy\*dy);

            // 2\. Relative Velocity vector  
            float dvx \= vel\_target.x \- vel\_ego.x;  
            float dvy \= vel\_target.y \- vel\_ego.y;  
            float v\_rel \= sqrtf(dvx\*dvx \+ dvy\*dvy);

            // 3\. Time To Collision (Simple approximation)  
            // TTC \= Distance / Closing Speed  
            float ttc \= (v\_rel \> 0.1) ? (dist / v\_rel) : 999.0;

            if (ttc \< 2.0 && dist \< 5.0) {  
                printf("\[ALARM\] CRASH IMMINENT\! TTC: %.2fs Dist: %.2fm\\n", ttc, dist);  
                if (bz\_fd \>= 0\) write(bz\_fd, "1", 1);  
            } else {  
                if (bz\_fd \>= 0\) write(bz\_fd, "0", 1);  
            }  
        }  
    }  
    close(bz\_fd);  
    close(sockfd);  
    return 0;  
}

## **5\. Component 3: Visualization & Data Bridge**

The Linux ELF is purely computational. To get real data into it (without attaching a real GPS to the ESP32 pins), we use the Firmware as a bridge.

1. **Web Dashboard:** A simple index.html hosted by the ESP32 (using esp\_http\_server).  
2. **Data Ingress:** The browser sends JSON packets via HTTP POST to /api/telemetry.  
3. **The Bridge:** The Firmware's HTTP handler parses the JSON and sends the raw struct via UDP to 127.0.0.1:8000.

*Note: Since the Linux app shares the IP stack (Unikernel), 127.0.0.1 traffic is routed internally by LwIP without leaving the chip.*

## **6\. Testing Plan**

1. **Flash Firmware:** Ensure mount\_buzzer\_driver() is called in app\_main and the WiFi is provisioned.  
2. **Upload ELF:** Use the C2 Master script from Demo 1 to upload collision\_guard.elf to the ESP32.  
3. **Start Simulation:**  
   * Open the ESP32's IP address in a browser.  
   * Click "Start Simulation" (starts sending "Ego Car" data).  
   * Open a second tab or use a second device. Click "Start Simulation" with ID 1 ("Target Car").  
4. **Verification:**
   * Watch the Serial Monitor (stdout redirected). You should see distance calculations.
   * "Drive" the virtual cars together (by modifying coordinates in the JS console or UI).
   * **Success:** When they get close, the physical LED (GPIO 5) on the ESP32 should light up/buzz, and the console should print [ALARM] CRASH IMMINENT!

---

## **7\. Complete Telemetry Bridge Implementation**

Create `main/telemetry_bridge.c`:

```c
/**
 * @file telemetry_bridge.c
 * @brief HTTP to UDP bridge for Demo2 telemetry
 *
 * Receives JSON telemetry from web dashboard via HTTP POST,
 * converts to binary struct, and forwards to Linux app via UDP.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h"
#include "lwip/sockets.h"

static const char *TAG = "telemetry_bridge";

#define TELEMETRY_UDP_PORT  8000
#define TELEMETRY_HTTP_PORT 80

/*==============================================================================
 * Telemetry Packet (must match collision_guard.c)
 *============================================================================*/

typedef struct __attribute__((packed)) {
    float lat;
    float lon;
    float speed_mps;
    float heading;
    int car_id;
} telemetry_packet_t;

// UDP socket for forwarding to Linux app
static int s_udp_sock = -1;
static struct sockaddr_in s_udp_dest;

/*==============================================================================
 * UDP Forwarder
 *============================================================================*/

static void udp_init(void) {
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP socket");
        return;
    }

    // Send to localhost (Linux app listening on same stack)
    memset(&s_udp_dest, 0, sizeof(s_udp_dest));
    s_udp_dest.sin_family = AF_INET;
    s_udp_dest.sin_port = htons(TELEMETRY_UDP_PORT);
    s_udp_dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1

    ESP_LOGI(TAG, "UDP forwarder initialized (127.0.0.1:%d)", TELEMETRY_UDP_PORT);
}

static void forward_telemetry(telemetry_packet_t *pkt) {
    if (s_udp_sock < 0) return;

    int sent = sendto(s_udp_sock, pkt, sizeof(*pkt), 0,
                      (struct sockaddr *)&s_udp_dest, sizeof(s_udp_dest));

    if (sent < 0) {
        ESP_LOGW(TAG, "UDP send failed: %d", errno);
    }
}

/*==============================================================================
 * HTTP Handlers
 *============================================================================*/

// Embedded web dashboard HTML
static const char *INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Collision Avoidance Demo</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; padding: 20px; background: #1a1a2e; color: #eee; }
        .card { background: #16213e; padding: 20px; margin: 10px 0; border-radius: 10px; }
        button { background: #e94560; color: white; border: none; padding: 15px 30px;
                 font-size: 18px; border-radius: 5px; cursor: pointer; margin: 5px; }
        button:hover { background: #ff6b6b; }
        button.stop { background: #4a4e69; }
        .slider { width: 100%; margin: 10px 0; }
        #status { color: #0f3460; font-weight: bold; }
        .coords { font-family: monospace; font-size: 14px; color: #94a3b8; }
    </style>
</head>
<body>
    <h1>V2X Collision Avoidance</h1>

    <div class="card">
        <h2>Car ID: <span id="carId">0</span> (Ego)</h2>
        <button onclick="setCarId(0)">Ego Car (0)</button>
        <button onclick="setCarId(1)">Target Car (1)</button>
    </div>

    <div class="card">
        <h3>Position</h3>
        <label>Latitude: <span id="latVal">37.7749</span></label>
        <input type="range" id="lat" class="slider" min="37.770" max="37.780" step="0.0001" value="37.7749">
        <label>Longitude: <span id="lonVal">-122.4194</span></label>
        <input type="range" id="lon" class="slider" min="-122.425" max="-122.415" step="0.0001" value="-122.4194">
    </div>

    <div class="card">
        <h3>Motion</h3>
        <label>Speed (m/s): <span id="speedVal">0</span></label>
        <input type="range" id="speed" class="slider" min="0" max="30" step="1" value="0">
        <label>Heading (°): <span id="headingVal">0</span></label>
        <input type="range" id="heading" class="slider" min="0" max="360" step="5" value="0">
    </div>

    <div class="card">
        <button onclick="startSim()">Start Streaming</button>
        <button onclick="stopSim()" class="stop">Stop</button>
        <p id="status">Stopped</p>
    </div>

    <script>
        let carId = 0;
        let interval = null;

        function setCarId(id) {
            carId = id;
            document.getElementById('carId').textContent = id;
        }

        // Update display values
        ['lat', 'lon', 'speed', 'heading'].forEach(id => {
            document.getElementById(id).oninput = function() {
                document.getElementById(id + 'Val').textContent = this.value;
            };
        });

        function sendTelemetry() {
            const data = {
                lat: parseFloat(document.getElementById('lat').value),
                lon: parseFloat(document.getElementById('lon').value),
                speed_mps: parseFloat(document.getElementById('speed').value),
                heading: parseFloat(document.getElementById('heading').value),
                car_id: carId
            };

            fetch('/api/telemetry', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify(data)
            });
        }

        function startSim() {
            if (interval) return;
            interval = setInterval(sendTelemetry, 100);  // 10 Hz
            document.getElementById('status').textContent = 'Streaming...';
            document.getElementById('status').style.color = '#10b981';
        }

        function stopSim() {
            if (interval) {
                clearInterval(interval);
                interval = null;
            }
            document.getElementById('status').textContent = 'Stopped';
            document.getElementById('status').style.color = '#ef4444';
        }
    </script>
</body>
</html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
    return ESP_OK;
}

static esp_err_t telemetry_handler(httpd_req_t *req) {
    char buf[256];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);

    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }

    buf[received] = '\0';

    // Parse JSON
    cJSON *json = cJSON_Parse(buf);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Extract fields
    telemetry_packet_t pkt = {0};
    cJSON *lat = cJSON_GetObjectItem(json, "lat");
    cJSON *lon = cJSON_GetObjectItem(json, "lon");
    cJSON *speed = cJSON_GetObjectItem(json, "speed_mps");
    cJSON *heading = cJSON_GetObjectItem(json, "heading");
    cJSON *car_id = cJSON_GetObjectItem(json, "car_id");

    if (lat) pkt.lat = lat->valuedouble;
    if (lon) pkt.lon = lon->valuedouble;
    if (speed) pkt.speed_mps = speed->valuedouble;
    if (heading) pkt.heading = heading->valuedouble;
    if (car_id) pkt.car_id = car_id->valueint;

    cJSON_Delete(json);

    // Forward to Linux app via UDP
    forward_telemetry(&pkt);

    // Respond
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

/*==============================================================================
 * HTTP Server
 *============================================================================*/

void telemetry_bridge_start(void) {
    // Initialize UDP forwarder
    udp_init();

    // Start HTTP server
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    // Register handlers
    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
    };
    httpd_register_uri_handler(server, &index_uri);

    httpd_uri_t telemetry_uri = {
        .uri = "/api/telemetry",
        .method = HTTP_POST,
        .handler = telemetry_handler,
    };
    httpd_register_uri_handler(server, &telemetry_uri);

    ESP_LOGI(TAG, "HTTP server started on port %d", config.server_port);
}
```

---

## **8\. Complete app\_main() for Demo2**

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_vfs_littlefs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "drivers.h"

static const char *TAG = "main";

// Forward declarations
extern void c2_server_start(void);
extern void telemetry_bridge_start(void);

// WiFi credentials (change for your network or use QEMU networking)
#define WIFI_SSID       "YourSSID"        // Change for real hardware
#define WIFI_PASSWORD   "YourPassword"    // Empty for open networks

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void) {
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // Wait for connection
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);
}

static void littlefs_init(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/linux",
        .partition_label = "linux_fs",
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(ret));
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Linux Compatibility Layer - Demo2 (Collision Avoidance) ===");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize filesystem
    littlefs_init();

    // Register hardware drivers
    vfs_collision_register();
    vfs_buzzer_register();

    // Initialize WiFi
    wifi_init();

    // Start telemetry bridge (HTTP server)
    telemetry_bridge_start();

    // Start C2 server for ELF upload
    c2_server_start();

    ESP_LOGI(TAG, "System ready.");
    ESP_LOGI(TAG, "1. Open ESP32 IP in browser for dashboard");
    ESP_LOGI(TAG, "2. Upload collision_guard.elf via C2 (port 9000)");
    ESP_LOGI(TAG, "3. Use dashboard to simulate vehicle positions");
}
```

---

## **9\. Complete collision\_guard.c (Guest ELF)**

Create `apps/collision_guard/main.c`:

```c
/**
 * @file collision_guard.c
 * @brief Collision avoidance algorithm for Demo2
 *
 * Receives vehicle telemetry via UDP, calculates Time-To-Collision,
 * and triggers /dev/buzzer alarm when crash is imminent.
 */

// External function declarations (resolved by symbol table)
extern int printf(const char *fmt, ...);
extern int socket(int domain, int type, int protocol);
extern int bind(int sockfd, const void *addr, int addrlen);
extern int recvfrom(int sockfd, void *buf, int len, int flags,
                    void *src_addr, int *addrlen);
extern int open(const char *path, int flags, ...);
extern int write(int fd, const void *buf, int count);
extern int close(int fd);
extern int ioctl(int fd, int cmd, ...);
extern float sqrtf(float x);
extern float sinf(float x);
extern float cosf(float x);
extern void setvbuf(void *stream, char *buf, int mode, int size);

// Constants
#define AF_INET     2
#define SOCK_DGRAM  2
#define INADDR_ANY  0
#define O_WRONLY    1
#define _IONBF      2
#define M_PI        3.14159265358979323846f

#define UDP_PORT    8000
#define IOCTL_BUZZER_SET_FREQ  0x2001

// Telemetry packet (must match firmware)
typedef struct {
    float lat;
    float lon;
    float speed_mps;
    float heading;
    int car_id;
} telemetry_t;

// 2D vector
typedef struct {
    float x, y;
} vec2_t;

// sockaddr_in structure
struct sockaddr_in {
    short sin_family;
    unsigned short sin_port;
    unsigned int sin_addr;
    char sin_zero[8];
};

// Convert geo coordinates to local meters
static vec2_t geo_to_local(float lat, float lon, float ref_lat, float ref_lon) {
    const float R = 6371000.0f;
    float x = (lon - ref_lon) * (M_PI / 180.0f) * R * cosf(ref_lat * (M_PI / 180.0f));
    float y = (lat - ref_lat) * (M_PI / 180.0f) * R;
    return (vec2_t){x, y};
}

// Convert heading to velocity vector
static vec2_t heading_to_velocity(float speed, float heading_deg) {
    float rad = heading_deg * (M_PI / 180.0f);
    return (vec2_t){
        speed * sinf(rad),
        speed * cosf(rad)
    };
}

// Calculate distance between two points
static float distance(vec2_t a, vec2_t b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    return sqrtf(dx * dx + dy * dy);
}

// Calculate relative velocity magnitude
static float relative_speed(vec2_t v1, vec2_t v2) {
    float dvx = v2.x - v1.x;
    float dvy = v2.y - v1.y;
    return sqrtf(dvx * dvx + dvy * dvy);
}

int main(void) {
    // Disable stdout buffering for real-time output
    setvbuf((void*)1, 0, _IONBF, 0);  // stdout = fd 1

    printf("\n=== Collision Guard Started ===\n");
    printf("Listening for telemetry on UDP port %d\n\n", UDP_PORT);

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        printf("ERROR: Failed to create socket\n");
        return 1;
    }

    // Bind to port
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = (UDP_PORT >> 8) | (UDP_PORT << 8),  // htons
        .sin_addr = INADDR_ANY,
    };

    if (bind(sock, &addr, sizeof(addr)) < 0) {
        printf("ERROR: Bind failed\n");
        close(sock);
        return 1;
    }

    // Open buzzer device
    int buzzer_fd = open("/dev/buzzer", O_WRONLY);
    if (buzzer_fd < 0) {
        printf("WARNING: Buzzer not available\n");
    } else {
        // Set alarm frequency
        ioctl(buzzer_fd, IOCTL_BUZZER_SET_FREQ, 2000);
    }

    // State tracking
    vec2_t pos_ego = {0, 0};
    vec2_t vel_ego = {0, 0};
    vec2_t pos_target = {0, 0};
    vec2_t vel_target = {0, 0};

    float ref_lat = 0, ref_lon = 0;
    int has_reference = 0;
    int alarm_active = 0;

    // Main loop
    telemetry_t pkt;
    while (1) {
        int n = recvfrom(sock, &pkt, sizeof(pkt), 0, 0, 0);
        if (n <= 0) continue;

        // Establish reference point on first packet
        if (!has_reference) {
            ref_lat = pkt.lat;
            ref_lon = pkt.lon;
            has_reference = 1;
            printf("Reference point set: %.6f, %.6f\n", ref_lat, ref_lon);
        }

        // Convert to local coordinates
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

        // Time To Collision (simplified)
        float ttc = (rel_vel > 0.1f) ? (dist / rel_vel) : 999.0f;

        // Collision detection thresholds
        int imminent = (ttc < 2.0f && dist < 10.0f);

        // Debug output (every packet)
        printf("Car%d: pos(%.1f,%.1f) vel(%.1f,%.1f) | Dist=%.1fm TTC=%.1fs %s\n",
               pkt.car_id,
               pos.x, pos.y,
               vel.x, vel.y,
               dist, ttc,
               imminent ? "[ALERT!]" : "");

        // Control buzzer
        if (imminent && !alarm_active) {
            printf("\n!!! COLLISION IMMINENT !!!\n\n");
            if (buzzer_fd >= 0) write(buzzer_fd, "1", 1);
            alarm_active = 1;
        } else if (!imminent && alarm_active) {
            if (buzzer_fd >= 0) write(buzzer_fd, "0", 1);
            alarm_active = 0;
        }
    }

    // Cleanup (unreachable)
    if (buzzer_fd >= 0) {
        write(buzzer_fd, "0", 1);
        close(buzzer_fd);
    }
    close(sock);

    return 0;
}
```

---

## **10\. QEMU Configuration for Demo2**

**Running Demo2 in QEMU with networking:**

```bash
# Build firmware
idf.py build

# Create merged flash binary
cd build
python -m esptool --chip esp32 merge_bin \
    -o merged-flash.bin \
    --flash_mode dio \
    --flash_size 4MB \
    0x1000 bootloader/bootloader.bin \
    0x8000 partition_table/partition-table.bin \
    0xd000 ota_data_initial.bin \
    0x10000 linux_compat_layer.bin \
    0x190000 linux_fs.bin

# Pad to 4MB
dd if=/dev/zero bs=1 count=$((4194304 - $(stat -c%s merged-flash.bin))) >> merged-flash.bin

# Run QEMU with networking
qemu-system-xtensa -nographic -machine esp32 \
    -drive file=merged-flash.bin,if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=tcp::80-:80,hostfwd=tcp::9000-:9000 \
    -no-reboot
```

**Port Forwarding:**
- Port 80: Web dashboard (HTTP)
- Port 9000: C2 server (ELF upload)

**Note:** In QEMU, buzzer/LED hardware isn't simulated but driver code executes correctly. Check serial output for driver state changes.

---

## **11\. CMakeLists.txt for Demo2**

```cmake
idf_component_register(
    SRCS
        "main.c"
        "c2_server.c"
        "telemetry_bridge.c"
        "elf_loader.c"
        "syscalls/shim_unistd.c"
        "syscalls/shim_socket.c"
        "syscalls/shim_process.c"
        "vfs_drivers/vfs_c2_pipe.c"
        "drivers/vfs_collision.c"
        "drivers/vfs_buzzer.c"
    INCLUDE_DIRS
        "."
        "syscalls"
        "vfs_drivers"
        "drivers"
    REQUIRES
        esp_http_server
        json
)
```

---

## **12\. Testing Demo2**

### **Step-by-Step Test Procedure**

1. **Build firmware:**
   ```bash
   idf.py build
   ```

2. **Run in QEMU with networking:**
   ```bash
   # See section 10 for full QEMU command with port forwarding
   qemu-system-xtensa -nographic -machine esp32 \
       -drive file=build/merged-flash.bin,if=mtd,format=raw \
       -nic user,model=open_eth,hostfwd=tcp::80-:80,hostfwd=tcp::9000-:9000 \
       -no-reboot
   ```

3. **Wait for boot messages:**
   ```
   I (xxx) main: System ready.
   I (xxx) c2_server: C2 Server listening on port 9000
   ```

4. **Upload collision_guard.elf:**
   ```bash
   # Use localhost since QEMU forwards ports
   python tools/c2_master.py apps/collision_guard/payload.elf localhost
   ```

5. **Open web dashboard:**
   Navigate to `http://localhost/` in browser

6. **Simulate collision:**
   - Open two browser tabs/windows
   - Tab 1: Set Car ID = 0 (Ego), start streaming
   - Tab 2: Set Car ID = 1 (Target), start streaming
   - Adjust sliders to bring cars close together

7. **Verify alarm:**
   - Watch serial output for distance calculations
   - When TTC < 2s and distance < 10m: alarm triggers
   - "[ALARM!]" appears in output

---

## **13\. Troubleshooting**

| Issue | Cause | Solution |
|-------|-------|----------|
| No telemetry received | UDP not working | Check localhost routing in LwIP |
| Buzzer doesn't sound | GPIO mismatch | Verify GPIO 5 in diagram.json |
| "Bind failed" | Port already in use | Restart simulation |
| Positions don't update | JSON parsing error | Check browser console for errors |
| TTC always 999 | No relative velocity | Both cars need non-zero speed |
| Connection refused | Port not forwarded | Check QEMU hostfwd configuration |