## **1\. Objective**

This document details the implementation of the "Distributed Command & Control" demo. In this scenario, the ESP32 acts as a "Bot" waiting for instructions. A "Master" (PC) pushes a compiled Linux ELF binary over the network. The ESP32 receives the binary, writes it to the LittleFS partition, dynamically loads it, and streams the application's stdout back to the Master in real-time.

## **2\. Architecture Overview**

### **2.1 The Master (c2\_master.py)**

The Master is a Python script running on the host machine. Its responsibilities are:

1. Read a local compiled ELF file.  
2. Connect to the ESP32 via TCP (Port 9000).  
3. Send a protocol header (Magic Bytes \+ Binary Size).  
4. Stream the ELF data.  
5. Enter a "Listening Mode" to display the output returned by the ESP32.

#### **Source Code: c2\_master.py**

import socket  
import struct  
import sys  
import time

def send\_payload(ip, port, filepath):  
    try:  
        \# 1\. Read the ELF binary  
        with open(filepath, "rb") as f:  
            elf\_data \= f.read()  
          
        print(f"\[Master\] Loaded payload: {len(elf\_data)} bytes")

        \# 2\. Connect to Bot  
        sock \= socket.socket(socket.AF\_INET, socket.SOCK\_STREAM)  
        sock.connect((ip, port))  
        print(f"\[Master\] Connected to Bot at {ip}:{port}")

        \# 3\. Send Header (SIZE as 4-byte integer)  
        \# Format: 'I' \= unsigned int (4 bytes)  
        header \= struct.pack('\<I', len(elf\_data))  
        sock.sendall(header)

        \# 4\. Send Binary Data  
        sock.sendall(elf\_data)  
        print("\[Master\] Payload sent. Waiting for execution output...")

        \# 5\. Listen Loop (Receive stdout from Bot)  
        while True:  
            data \= sock.recv(1024)  
            if not data:  
                break  
            \# Print received data without adding extra newlines  
            sys.stdout.write(data.decode('utf-8', errors='replace'))  
            sys.stdout.flush()

    except Exception as e:  
        print(f"\\n\[Master\] Error: {e}")  
    finally:  
        sock.close()

if \_\_name\_\_ \== "\_\_main\_\_":  
    if len(sys.argv) \< 3:  
        print("Usage: python c2\_master.py \<payload.elf\> \<ESP\_IP\>")  
        sys.exit(1)  
      
    send\_payload(sys.argv\[2\], 9000, sys.argv\[1\])

### **2.2 The Bot (ESP32 Firmware)**

The firmware requires a dedicated FreeRTOS task, c2\_loader\_task, which acts as the TCP server.

#### **Task Logic**

1. **Listen:** Create a socket bound to port 9000\.  
2. **Accept:** Wait for the Master to connect.  
3. **Receive Header:** Read the first 4 bytes to determine ELF size.  
4. **Stream to Disk:** Open /linux/payload.elf via the VFS. Read from the socket and write to the file in chunks until the full size is received.  
5. **Configure Redirection:** Set a global/task-local variable g\_c2\_socket\_fd to the active client socket.  
6. **Execute:** Call the ELF Loader (elf\_loader\_load) on /linux/payload.elf.  
7. **Cleanup:** Close the socket and file when the ELF returns.

#### **Firmware Snippet (main/c2\_server.c)**

\#include "lwip/sockets.h"  
\#include "esp\_log.h"  
\#include "esp\_vfs.h"

// Global variable for redirection (See Section 3\)  
int g\_c2\_socket\_fd \= \-1;

void c2\_server\_task(void \*pvParameters) {  
    int listen\_sock \= socket(AF\_INET, SOCK\_STREAM, IPPROTO\_IP);  
    struct sockaddr\_in server\_addr \= {  
        .sin\_family \= AF\_INET,  
        .sin\_addr.s\_addr \= htonl(INADDR\_ANY),  
        .sin\_port \= htons(9000)  
    };  
      
    bind(listen\_sock, (struct sockaddr \*)\&server\_addr, sizeof(server\_addr));  
    listen(listen\_sock, 1);

    while (1) {  
        ESP\_LOGI("C2", "Waiting for payload on port 9000...");  
        struct sockaddr\_in source\_addr;  
        socklen\_t addr\_len \= sizeof(source\_addr);  
        int client\_sock \= accept(listen\_sock, (struct sockaddr \*)\&source\_addr, \&addr\_len);

        if (client\_sock \< 0\) continue;

        // 1\. Receive Size Header  
        uint32\_t payload\_size \= 0;  
        recv(client\_sock, \&payload\_size, sizeof(payload\_size), 0);  
          
        // 2\. Stream to File  
        FILE \*f \= fopen("/linux/payload.elf", "wb");  
        uint8\_t buf\[128\];  
        int remaining \= payload\_size;  
        while (remaining \> 0\) {  
            int to\_read \= (remaining \> sizeof(buf)) ? sizeof(buf) : remaining;  
            int len \= recv(client\_sock, buf, to\_read, 0);  
            fwrite(buf, 1, len, f);  
            remaining \-= len;  
        }  
        fclose(f);  
        ESP\_LOGI("C2", "Payload saved. Executing...");

        // 3\. Set Redirection Global  
        g\_c2\_socket\_fd \= client\_sock;

        // 4\. Run the ELF  
        elf\_loader\_load("/linux/payload.elf");

        // 5\. Cleanup  
        g\_c2\_socket\_fd \= \-1;  
        close(client\_sock);  
        ESP\_LOGI("C2", "Execution finished.");  
    }  
}

## **3\. Execution & Redirection (Shim Layer Modification)**

To route the printf output from the payload back to the Master, we modify the shim\_write function in shim\_unistd.c.

Normally, STDOUT\_FILENO (1) maps to the UART. We intercept this.

#### **Modified shim\_write**

extern int g\_c2\_socket\_fd; // Defined in c2\_server.c

ssize\_t shim\_write(int fd, const void \*data, size\_t size) {  
    // Check if we are writing to stdout and if C2 is active  
    if (fd \== STDOUT\_FILENO) {  
        // Option A: Send to BOTH UART and Socket (Mirroring)  
        // Option B: Send ONLY to Socket (Stealth Mode)  
          
        // Write to UART for local debugging  
        esp\_vfs\_write(1, data, size); 

        // If C2 is connected, stream it back  
        if (g\_c2\_socket\_fd \>= 0\) {  
            // Use LwIP send() directly  
            int sent \= send(g\_c2\_socket\_fd, data, size, 0);  
            if (sent \< 0\) {  
                // Connection lost? Ignore error to keep app running  
            }  
        }  
        return size;  
    }

    // Default VFS behavior for other FDs  
    return esp\_vfs\_write(fd, data, size);  
}

## **4\. The Payload (payload.c)**

This is the Linux application source code. It simulates a WiFi scanner. Note that it includes standard headers and uses standard printf.

\#include \<stdio.h\>  
\#include \<unistd.h\>  
\#include \<stdlib.h\>

int main() {  
    printf("\\n--- ESP32 C2 Payload Active \---\\n");  
    printf("Command: SCAN\_WIFI\\n");  
    printf("Initializing Wireless Interface...\\n");  
      
    // Simulate processing delay  
    sleep(1);   
      
    printf("\[+\] Scanning...\\n");  
    sleep(1);

    // Mock Results  
    printf("------------------------------------------------\\n");  
    printf("SSID                 | RSSI | CHAN | SECURITY   \\n");  
    printf("------------------------------------------------\\n");  
    printf("FBI\_Surveillance\_Van | \-45  | 6    | WPA2       \\n");  
    printf("Linksys\_Home         | \-80  | 1    | OPEN       \\n");  
    printf("Starbucks\_Guest      | \-65  | 11   | WPA2       \\n");  
    printf("------------------------------------------------\\n");  
    printf("\[+\] Scan Complete. 3 Networks found.\\n");  
      
    printf("Exiting payload.\\n");  
    return 0;  
}

## **5\. Testing Plan**

### **Step 1: Compile the Payload**

Use the Xtensa toolchain to compile the payload as a Position Independent Executable (PIE) or relocatable object.

xtensa-esp32-elf-gcc \-mlongcalls \-fno-common \-c apps/c2\_payload/payload.c \-o payload.o  
xtensa-esp32-elf-ld \-r payload.o \-o payload.elf

### **Step 2: Flash the Firmware**

Build and flash the ESP32 firmware containing the c2\_loader\_task.

idf.py build flash monitor

*Wait until the serial monitor shows "C2: Waiting for payload on port 9000..."*

### **Step 3: Run the Master**

On your PC, execute the python script. Replace \<ESP\_IP\> with the actual IP address printed in the serial monitor.

python c2\_master.py payload.elf 192.168.1.105

### **Step 4: Verification**

Observe the Python console. You should see:

\[Master\] Loaded payload: 4096 bytes  
\[Master\] Connected to Bot at 192.168.1.105:9000  
\[Master\] Payload sent. Waiting for execution output...

\--- ESP32 C2 Payload Active \---  
Command: SCAN\_WIFI  
Initializing Wireless Interface...  
\[+\] Scanning...  
...  
\[+\] Scan Complete. 3 Networks found.  
Exiting payload.

This confirms the ELF was transmitted, loaded, executed, and its output was successfully redirected over the network.

---

## **6\. Complete c2\_server.c Implementation**

Create `main/c2_server.c`:

```c
/**
 * @file c2_server.c
 * @brief Command & Control server for Demo1
 *
 * Receives ELF payloads over TCP, saves to filesystem,
 * executes them, and streams stdout back to the master.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "esp_vfs.h"

static const char *TAG = "c2_server";

#define C2_PORT             9000
#define C2_RECV_BUF_SIZE    512
#define C2_PAYLOAD_PATH     "/linux/payload.elf"
#define C2_TASK_STACK_SIZE  8192

/*==============================================================================
 * Global C2 Socket (for stdout redirection)
 *============================================================================*/

// This is accessed by shim_write() to redirect stdout
int g_c2_socket_fd = -1;

/*==============================================================================
 * ELF Loader Interface
 *============================================================================*/

// Forward declaration - implemented in elf_loader.c
extern int elf_loader_run(const char *path, int argc, char *argv[]);

/*==============================================================================
 * C2 Protocol Handler
 *============================================================================*/

static int c2_receive_payload(int client_sock) {
    // 1. Receive size header (4 bytes, little-endian)
    uint32_t payload_size = 0;
    int received = recv(client_sock, &payload_size, sizeof(payload_size), MSG_WAITALL);

    if (received != sizeof(payload_size)) {
        ESP_LOGE(TAG, "Failed to receive size header");
        return -1;
    }

    ESP_LOGI(TAG, "Receiving payload: %lu bytes", payload_size);

    // Sanity check
    if (payload_size == 0 || payload_size > 1024 * 1024) {  // Max 1MB
        ESP_LOGE(TAG, "Invalid payload size: %lu", payload_size);
        return -1;
    }

    // 2. Open file for writing
    FILE *f = fopen(C2_PAYLOAD_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s for writing: %s",
                 C2_PAYLOAD_PATH, strerror(errno));
        return -1;
    }

    // 3. Stream data to file
    uint8_t buf[C2_RECV_BUF_SIZE];
    uint32_t remaining = payload_size;
    uint32_t total_written = 0;

    while (remaining > 0) {
        int to_read = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        int len = recv(client_sock, buf, to_read, 0);

        if (len <= 0) {
            ESP_LOGE(TAG, "Connection lost during transfer");
            fclose(f);
            return -1;
        }

        size_t written = fwrite(buf, 1, len, f);
        if (written != len) {
            ESP_LOGE(TAG, "Write error: %s", strerror(errno));
            fclose(f);
            return -1;
        }

        remaining -= len;
        total_written += written;

        // Progress logging (every 10%)
        if (total_written % (payload_size / 10 + 1) < C2_RECV_BUF_SIZE) {
            ESP_LOGD(TAG, "Progress: %lu/%lu bytes", total_written, payload_size);
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "Payload saved: %lu bytes written", total_written);

    return 0;
}

static void c2_handle_client(int client_sock, struct sockaddr_in *client_addr) {
    char addr_str[INET_ADDRSTRLEN];
    inet_ntoa_r(client_addr->sin_addr, addr_str, sizeof(addr_str));
    ESP_LOGI(TAG, "Client connected: %s:%d", addr_str, ntohs(client_addr->sin_port));

    // Receive and save payload
    if (c2_receive_payload(client_sock) != 0) {
        ESP_LOGE(TAG, "Failed to receive payload");
        close(client_sock);
        return;
    }

    // Configure stdout redirection
    g_c2_socket_fd = client_sock;
    ESP_LOGI(TAG, "Stdout redirection enabled (fd=%d)", client_sock);

    // Send acknowledgment
    const char *ack = "[Bot] Payload received. Executing...\n";
    send(client_sock, ack, strlen(ack), 0);

    // Execute the ELF
    ESP_LOGI(TAG, "Executing payload...");

    char *argv[] = {"payload.elf", NULL};
    int ret = elf_loader_run(C2_PAYLOAD_PATH, 1, argv);

    ESP_LOGI(TAG, "Payload returned: %d", ret);

    // Send completion message
    char done_msg[64];
    snprintf(done_msg, sizeof(done_msg), "\n[Bot] Execution complete. Return code: %d\n", ret);
    send(client_sock, done_msg, strlen(done_msg), 0);

    // Cleanup
    g_c2_socket_fd = -1;
    close(client_sock);
    ESP_LOGI(TAG, "Client disconnected");
}

/*==============================================================================
 * C2 Server Task
 *============================================================================*/

void c2_server_task(void *pvParameters) {
    // Create listening socket
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Failed to create socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    // Allow socket reuse
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind to port
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(C2_PORT),
    };

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind: %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    // Start listening
    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(TAG, "Failed to listen: %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "C2 Server listening on port %d", C2_PORT);

    // Main accept loop
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        ESP_LOGI(TAG, "Waiting for payload...");
        int client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &addr_len);

        if (client_sock < 0) {
            ESP_LOGE(TAG, "Accept failed: %d", errno);
            continue;
        }

        // Handle client (blocking - one at a time)
        c2_handle_client(client_sock, &client_addr);
    }

    // Should never reach here
    close(listen_sock);
    vTaskDelete(NULL);
}

/*==============================================================================
 * Initialization
 *============================================================================*/

void c2_server_start(void) {
    xTaskCreate(
        c2_server_task,
        "c2_server",
        C2_TASK_STACK_SIZE,
        NULL,
        5,  // Priority
        NULL
    );
}
```

---

## **7\. Complete c2\_master.py**

Create `tools/c2_master.py`:

```python
#!/usr/bin/env python3
"""
C2 Master - Command & Control client for ESP32 Linux Compatibility Layer

Sends ELF payloads to the ESP32 and receives execution output.

Usage:
    python c2_master.py <payload.elf> <ESP_IP> [port]

Example:
    python c2_master.py apps/hello_world/payload.elf 192.168.1.105
"""

import socket
import struct
import sys
import os
import argparse
import time

DEFAULT_PORT = 9000
RECV_TIMEOUT = 30  # seconds


def send_payload(ip: str, port: int, filepath: str, verbose: bool = True):
    """Send an ELF payload to the ESP32 and receive output."""

    # Validate file
    if not os.path.exists(filepath):
        print(f"[Master] Error: File not found: {filepath}")
        return 1

    # Read ELF binary
    with open(filepath, "rb") as f:
        elf_data = f.read()

    if verbose:
        print(f"[Master] Loaded payload: {len(elf_data)} bytes")
        print(f"[Master] Connecting to {ip}:{port}...")

    # Connect to ESP32
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(10)  # Connection timeout

    try:
        sock.connect((ip, port))
    except socket.timeout:
        print(f"[Master] Error: Connection timeout")
        return 1
    except ConnectionRefusedError:
        print(f"[Master] Error: Connection refused. Is the ESP32 running?")
        return 1

    if verbose:
        print(f"[Master] Connected!")

    # Send size header (4 bytes, little-endian unsigned int)
    header = struct.pack('<I', len(elf_data))
    sock.sendall(header)

    # Send ELF data
    sock.sendall(elf_data)

    if verbose:
        print(f"[Master] Payload sent. Waiting for execution output...\n")
        print("=" * 60)

    # Receive output
    sock.settimeout(RECV_TIMEOUT)

    try:
        while True:
            data = sock.recv(1024)
            if not data:
                break

            # Print received data (stdout from ESP32)
            text = data.decode('utf-8', errors='replace')
            sys.stdout.write(text)
            sys.stdout.flush()

    except socket.timeout:
        if verbose:
            print("\n[Master] Receive timeout (this may be normal)")
    except KeyboardInterrupt:
        if verbose:
            print("\n[Master] Interrupted by user")
    finally:
        sock.close()

    if verbose:
        print("=" * 60)
        print("[Master] Connection closed")

    return 0


def main():
    parser = argparse.ArgumentParser(
        description="C2 Master - Send ELF payloads to ESP32"
    )
    parser.add_argument("payload", help="Path to ELF payload file")
    parser.add_argument("ip", help="ESP32 IP address")
    parser.add_argument(
        "-p", "--port",
        type=int,
        default=DEFAULT_PORT,
        help=f"TCP port (default: {DEFAULT_PORT})"
    )
    parser.add_argument(
        "-q", "--quiet",
        action="store_true",
        help="Quiet mode (only show payload output)"
    )

    args = parser.parse_args()

    return send_payload(args.ip, args.port, args.payload, not args.quiet)


if __name__ == "__main__":
    sys.exit(main())
```

---

## **8\. Modified shim\_write for C2 Redirection**

Update `main/syscalls/shim_unistd.c`:

```c
// Import from c2_server.c
extern int g_c2_socket_fd;

ssize_t shim_write(int fd, const void *buf, size_t count) {
    // Intercept stdout writes when C2 is active
    if (fd == STDOUT_FILENO) {
        // Always write to UART for local debugging
        ssize_t ret = write(fd, buf, count);

        // If C2 is connected, also send to network
        if (g_c2_socket_fd >= 0) {
            int sent = send(g_c2_socket_fd, buf, count, MSG_DONTWAIT);
            if (sent < 0 && errno != EAGAIN) {
                ESP_LOGW(TAG, "C2 send failed: %d", errno);
            }
        }

        return ret;
    }

    // Intercept stderr as well
    if (fd == STDERR_FILENO && g_c2_socket_fd >= 0) {
        write(fd, buf, count);  // Local
        send(g_c2_socket_fd, buf, count, MSG_DONTWAIT);  // Network
        return count;
    }

    // Normal write for other FDs
    return write(fd, buf, count);
}
```

---

## **9\. Complete app\_main() for Demo1**

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

static const char *TAG = "main";

// Forward declarations
extern void c2_server_start(void);
extern void vfs_c2_pipe_register(void);

// WiFi credentials (change for your network or use QEMU networking)
#define WIFI_SSID       "YourSSID"        // Change for real hardware
#define WIFI_PASSWORD   "YourPassword"    // Empty for open networks

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
    }
}

static void wifi_init(void) {
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
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi initialization complete");
}

static void littlefs_init(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = "/linux",
        .partition_label = "linux_fs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(ret));
        return;
    }

    size_t total = 0, used = 0;
    esp_littlefs_info(conf.partition_label, &total, &used);
    ESP_LOGI(TAG, "LittleFS mounted: %d/%d bytes used", used, total);
}

void app_main(void) {
    ESP_LOGI(TAG, "=== Linux Compatibility Layer - Demo1 (C2) ===");

    // Initialize NVS (required for WiFi)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Initialize filesystem
    littlefs_init();

    // Register VFS drivers
    vfs_c2_pipe_register();

    // Initialize WiFi
    wifi_init();

    // Wait for IP address
    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    // Start C2 server
    c2_server_start();

    ESP_LOGI(TAG, "System ready. C2 server listening on port 9000.");
}
```

---

## **10\. QEMU Network Simulation**

For QEMU simulation, networking uses user-mode NAT:

```bash
# Run QEMU with networking
qemu-system-xtensa -nographic -machine esp32 \
    -drive file=build/merged-flash.bin,if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=tcp::9000-:9000 \
    -no-reboot
```

**QEMU Network Options:**
- `-nic user,model=open_eth`: Creates NAT network
- `hostfwd=tcp::9000-:9000`: Forwards host port 9000 to guest port 9000

**Note:** In QEMU, connect to localhost:9000 instead of ESP32's IP address.

---

## **11\. Testing Checklist**

1. [ ] Build firmware with `idf.py build`
2. [ ] Compile test payload with Xtensa toolchain
3. [ ] Create merged flash binary and run in QEMU
4. [ ] Wait for "C2 Server listening on port 9000"
5. [ ] Run `python c2_master.py payload.elf <IP>`
6. [ ] Verify payload output appears in Python console
7. [ ] Verify payload output also appears in ESP32 serial monitor

---

## **12\. Troubleshooting**

| Issue | Cause | Solution |
|-------|-------|----------|
| Connection refused | C2 server not running | Wait for "listening on port 9000" |
| Connection timeout | Wrong IP or firewall | Check IP, ensure same network |
| No output after "Executing..." | ELF loader failed | Check ELF compilation flags |
| Partial output | Buffering | Add `setvbuf(stdout, NULL, _IONBF, 0)` in payload |
| "Failed to mount LittleFS" | Partition mismatch | Verify partition name in all files |