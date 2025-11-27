## **1\. The "Everything is a File" Philosophy**

In a traditional UNIX/Linux environment, hardware devices are abstracted as files located in the /dev directory. Applications interact with sensors, serial ports, and accelerators using standard system calls (open, read, write, ioctl) rather than proprietary APIs.

This project replicates this model on the ESP32.

* **Kernel Space (Firmware):** The ESP-IDF firmware implements a Custom VFS Driver that manages the low-level hardware (GPIOs, I2C, Timers).  
* **User Space (Linux ELF):** The loaded binary simply opens a file path (e.g., /dev/collision) and reads bytes, completely unaware of the underlying hardware complexity.

## **2\. Case Study: Collision Avoidance Driver**

We will build a driver for a hypothetical collision avoidance sensor (e.g., LiDAR or Ultrasonic).

* **Path:** /dev/collision  
* **Read:** Blocks the calling task until a new distance measurement is ready. Returns the distance in centimeters.  
* **Write:** (Optional) Could be used to trigger a single-shot measurement.  
* **Ioctl:** Configures the sensor parameters (e.g., update frequency, alert threshold).

### **2.1 Architecture Overview**

\+-----------------------+                    \+------------------------+  
|   User Space (ELF)    |                    |  Kernel Space (ESP32)  |  
|                       |    Syscall         |                        |  
|   int fd \= open(...)  \+-------------------\>|  vfs\_collision\_open()  |  
|                       |                    |                        |  
|   ioctl(fd, SET\_RATE) \+-------------------\>|  vfs\_collision\_ioctl() |  
|                       |                    |                        |  
|   read(fd, \&dist, 4\)  \+-------------------\>|  vfs\_collision\_read()  |  
|      (Blocks)         |                    |    (Waits on Queue)    |  
\+-----------------------+                    \+-----------+------------+  
                                                         |  
                                                 Hardware Interrupt  
                                            (Sensor Data Ready) \-\> \[ISR\]

## **3\. Kernel Space Implementation (The Firmware)**

The driver is implemented as a standard C component in the ESP-IDF firmware. It must implement the esp\_vfs\_t interface.

### **3.1 Driver Context & Synchronization**

We use a FreeRTOS Queue to bridge the asynchronous hardware interrupts (or timer tasks) with the synchronous read() call.

\#include "freertos/FreeRTOS.h"  
\#include "freertos/queue.h"  
\#include "esp\_vfs.h"

\#define SENSOR\_QUEUE\_LEN 10

typedef struct {  
    QueueHandle\_t data\_queue;  // Stores incoming distance measurements  
    int update\_rate\_ms;        // Configurable update rate  
} collision\_ctx\_t;

static collision\_ctx\_t s\_ctx;

### **3.2 The Blocking Read**

When the Linux app calls read(), it expects data. If no data is ready, the operating system should suspend the process. In our Unikernel model, this maps to **blocking the FreeRTOS task**.

static ssize\_t collision\_read(int fd, void \*dst, size\_t size) {  
    if (size \< sizeof(uint32\_t)) {  
        return \-1; // Buffer too small  
    }

    uint32\_t distance\_cm;  
      
    // xQueueReceive blocks the calling task (the Linux ELF) until data arrives.  
    // portMAX\_DELAY means wait forever.  
    if (xQueueReceive(s\_ctx.data\_queue, \&distance\_cm, portMAX\_DELAY) \== pdTRUE) {  
        memcpy(dst, \&distance\_cm, sizeof(uint32\_t));  
        return sizeof(uint32\_t); // Return number of bytes read  
    }

    return \-1; // Should not happen with portMAX\_DELAY  
}

### **3.3 Configuration via Ioctl**

Standard read/write are for data. ioctl is for metadata and configuration. We define a custom command code.

\#define IOCTL\_COLLISION\_SET\_RATE  0x1001

static int collision\_ioctl(int fd, int cmd, va\_list args) {  
    switch (cmd) {  
        case IOCTL\_COLLISION\_SET\_RATE:  
            int new\_rate \= va\_arg(args, int);  
            s\_ctx.update\_rate\_ms \= new\_rate;  
            // logic to update hardware timer...  
            return 0;  
        default:  
            return \-1; // Unknown command  
    }  
}

### **3.4 Registration**

In app\_main, we register this structure.

void mount\_collision\_driver() {  
    s\_ctx.data\_queue \= xQueueCreate(SENSOR\_QUEUE\_LEN, sizeof(uint32\_t));

    esp\_vfs\_t vfs \= {  
        .flags \= ESP\_VFS\_FLAG\_DEFAULT,  
        .write \= NULL, // Not used  
        .open \= \&collision\_open, // Standard stub returning 0  
        .close \= \&collision\_close, // Standard stub  
        .read \= \&collision\_read,  
        .ioctl \= \&collision\_ioctl,  
    };  
      
    ESP\_ERROR\_CHECK(esp\_vfs\_register("/dev/collision", \&vfs, NULL));  
}

## **4\. User Space Implementation (The Linux ELF)**

The guest application knows nothing about FreeRTOS queues or GPIOs. It simply uses standard headers.

### **4.1 The Header Contract**

The app needs to know the command codes. Ideally, share a header file collision\_io.h.

// collision\_io.h  
\#define IOCTL\_COLLISION\_SET\_RATE  0x1001

### **4.2 The Application Logic**

This code is compiled with xtensa-esp32-elf-gcc and loaded dynamically.

\#include \<stdio.h\>  
\#include \<fcntl.h\>  
\#include \<unistd.h\>  
\#include \<sys/ioctl.h\>  
\#include "collision\_io.h"

int main() {  
    printf("Starting Collision Avoidance System...\\n");

    // 1\. Open the device  
    int fd \= open("/dev/collision", O\_RDONLY);  
    if (fd \< 0\) {  
        perror("Failed to open sensor");  
        return 1;  
    }

    // 2\. Configure Sensor (User Space \-\> Kernel Space)  
    int rate\_ms \= 50; // 20Hz  
    if (ioctl(fd, IOCTL\_COLLISION\_SET\_RATE, rate\_ms) \< 0\) {  
        perror("Failed to configure sensor");  
        return 1;  
    }

    // 3\. Data Loop  
    uint32\_t distance \= 0;  
    while (1) {  
        // This call BLOCKS until the firmware pushes new data  
        int bytes \= read(fd, \&distance, sizeof(distance));  
          
        if (bytes \> 0\) {  
            if (distance \< 30\) {  
                printf("\[ALERT\] Obstacle Detected\! Distance: %d cm\\n", distance);  
                // Trigger avoidance algorithm...  
            }  
        }  
    }

    close(fd);  
    return 0;  
}

## **5\. Summary of Roles**

| Feature | Kernel Space (ESP Firmware) | User Space (Linux ELF) |
| :---- | :---- | :---- |
| **Hardware Access** | Direct (GPIO, Timer, I2C) | Indirect (File Descriptor) |
| **Timing** | Hardware Timers / Interrupts | Driven by read() return speed |
| **Synchronization** | xQueueSendFromISR | Blocking read() |
| **Complexity** | High (Drivers, Registers) | Low (Business Logic) |

This model allows the Linux payload to contain *only* the sophisticated collision avoidance algorithms (e.g., Kalman Filters), while the ESP32 firmware handles the "dirty work" of hardware interfacing, ensuring the system remains modular and crash-resilient.

---

## **6\. Complete Collision Sensor Driver Implementation**

Create `main/drivers/vfs_collision.c`:

```c
/**
 * @file vfs_collision.c
 * @brief Virtual collision sensor driver for Demo2
 *
 * Provides /dev/collision - simulates a distance sensor using
 * FreeRTOS queues for data delivery and ioctl for configuration.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include "esp_vfs.h"
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "vfs_collision";

/*==============================================================================
 * IOCTL Command Definitions
 *============================================================================*/

#define IOCTL_COLLISION_SET_RATE      0x1001  // Set update rate (ms)
#define IOCTL_COLLISION_GET_RATE      0x1002  // Get current rate
#define IOCTL_COLLISION_SET_THRESHOLD 0x1003  // Set alert threshold (cm)
#define IOCTL_COLLISION_ENABLE        0x1004  // Enable/disable sensor
#define IOCTL_COLLISION_INJECT        0x1005  // Inject test data (for simulation)

/*==============================================================================
 * Driver Context
 *============================================================================*/

typedef struct {
    QueueHandle_t data_queue;       // Distance measurements
    TimerHandle_t sim_timer;        // Simulation timer
    int update_rate_ms;             // Update rate in milliseconds
    int alert_threshold_cm;         // Alert threshold
    bool enabled;                   // Sensor enabled flag
    bool is_open;                   // Device open flag
    uint32_t simulated_distance;    // For QEMU simulation
} collision_ctx_t;

static collision_ctx_t s_ctx = {
    .data_queue = NULL,
    .sim_timer = NULL,
    .update_rate_ms = 100,
    .alert_threshold_cm = 30,
    .enabled = true,
    .is_open = false,
    .simulated_distance = 100,
};

/*==============================================================================
 * Simulation Timer Callback
 *============================================================================*/

static void sim_timer_callback(TimerHandle_t timer) {
    if (!s_ctx.enabled || !s_ctx.is_open) {
        return;
    }

    // Simulate distance measurement (for QEMU without real hardware)
    // In real hardware, this would read from GPIO/ADC/I2C
    uint32_t distance = s_ctx.simulated_distance;

    // Add some noise for realism
    int noise = (esp_random() % 10) - 5;
    distance = (distance + noise > 0) ? distance + noise : 1;

    // Push to queue (non-blocking from timer context)
    xQueueSend(s_ctx.data_queue, &distance, 0);
}

/*==============================================================================
 * VFS Operations
 *============================================================================*/

static int collision_open(const char *path, int flags, int mode) {
    ESP_LOGI(TAG, "collision_open(path='%s')", path);

    if (s_ctx.is_open) {
        errno = EBUSY;
        return -1;
    }

    s_ctx.is_open = true;

    // Start simulation timer
    if (s_ctx.sim_timer) {
        xTimerStart(s_ctx.sim_timer, 0);
    }

    return 0;
}

static int collision_close(int fd) {
    ESP_LOGI(TAG, "collision_close(fd=%d)", fd);

    // Stop simulation timer
    if (s_ctx.sim_timer) {
        xTimerStop(s_ctx.sim_timer, 0);
    }

    // Clear any pending data
    xQueueReset(s_ctx.data_queue);

    s_ctx.is_open = false;
    return 0;
}

static ssize_t collision_read(int fd, void *dst, size_t size) {
    if (!s_ctx.is_open) {
        errno = EBADF;
        return -1;
    }

    if (size < sizeof(uint32_t)) {
        errno = EINVAL;
        return -1;
    }

    uint32_t distance;

    // Block until data is available (or timeout)
    TickType_t timeout = pdMS_TO_TICKS(s_ctx.update_rate_ms * 2);
    if (xQueueReceive(s_ctx.data_queue, &distance, timeout) == pdTRUE) {
        memcpy(dst, &distance, sizeof(uint32_t));
        return sizeof(uint32_t);
    }

    // Timeout - no data available
    errno = EAGAIN;
    return -1;
}

static ssize_t collision_write(int fd, const void *data, size_t size) {
    // Write can be used to inject test data
    if (size >= sizeof(uint32_t)) {
        uint32_t value;
        memcpy(&value, data, sizeof(uint32_t));
        s_ctx.simulated_distance = value;
        ESP_LOGD(TAG, "Injected distance: %lu cm", value);
        return sizeof(uint32_t);
    }

    errno = EINVAL;
    return -1;
}

static int collision_ioctl(int fd, int cmd, va_list args) {
    switch (cmd) {
        case IOCTL_COLLISION_SET_RATE: {
            int rate = va_arg(args, int);
            if (rate < 10 || rate > 10000) {
                errno = EINVAL;
                return -1;
            }
            s_ctx.update_rate_ms = rate;

            // Update timer period
            if (s_ctx.sim_timer) {
                xTimerChangePeriod(s_ctx.sim_timer, pdMS_TO_TICKS(rate), 0);
            }

            ESP_LOGI(TAG, "Update rate set to %d ms", rate);
            return 0;
        }

        case IOCTL_COLLISION_GET_RATE: {
            int *rate_ptr = va_arg(args, int *);
            if (rate_ptr) {
                *rate_ptr = s_ctx.update_rate_ms;
            }
            return 0;
        }

        case IOCTL_COLLISION_SET_THRESHOLD: {
            int threshold = va_arg(args, int);
            s_ctx.alert_threshold_cm = threshold;
            ESP_LOGI(TAG, "Alert threshold set to %d cm", threshold);
            return 0;
        }

        case IOCTL_COLLISION_ENABLE: {
            int enable = va_arg(args, int);
            s_ctx.enabled = (enable != 0);
            ESP_LOGI(TAG, "Sensor %s", s_ctx.enabled ? "enabled" : "disabled");
            return 0;
        }

        case IOCTL_COLLISION_INJECT: {
            uint32_t distance = va_arg(args, uint32_t);
            s_ctx.simulated_distance = distance;
            return 0;
        }

        default:
            errno = EINVAL;
            return -1;
    }
}

static int collision_fstat(int fd, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    return 0;
}

/*==============================================================================
 * Driver Registration
 *============================================================================*/

void vfs_collision_register(void) {
    // Create data queue
    s_ctx.data_queue = xQueueCreate(10, sizeof(uint32_t));
    if (!s_ctx.data_queue) {
        ESP_LOGE(TAG, "Failed to create data queue");
        return;
    }

    // Create simulation timer
    s_ctx.sim_timer = xTimerCreate(
        "collision_sim",
        pdMS_TO_TICKS(s_ctx.update_rate_ms),
        pdTRUE,  // Auto-reload
        NULL,
        sim_timer_callback
    );

    // Register VFS driver
    esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open = &collision_open,
        .close = &collision_close,
        .read = &collision_read,
        .write = &collision_write,
        .ioctl = &collision_ioctl,
        .fstat = &collision_fstat,
    };

    esp_err_t err = esp_vfs_register("/dev/collision", &vfs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /dev/collision: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Registered /dev/collision driver");
    }
}

/*==============================================================================
 * External API for Data Injection (Used by HTTP/WebSocket bridge)
 *============================================================================*/

void collision_inject_data(uint32_t distance_cm) {
    s_ctx.simulated_distance = distance_cm;

    // Immediately push to queue if device is open
    if (s_ctx.is_open && s_ctx.data_queue) {
        xQueueSend(s_ctx.data_queue, &distance_cm, 0);
    }
}
```

---

## **7\. Buzzer/Alarm Driver for Demo2**

Create `main/drivers/vfs_buzzer.c`:

```c
/**
 * @file vfs_buzzer.c
 * @brief PWM buzzer driver for collision alarm
 *
 * Provides /dev/buzzer - controls a PWM buzzer via LEDC peripheral.
 * Write "1" to turn on, "0" to turn off.
 * ioctl to set frequency.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "esp_vfs.h"
#include "esp_log.h"
#include "driver/ledc.h"

static const char *TAG = "vfs_buzzer";

/*==============================================================================
 * Configuration
 *============================================================================*/

#define BUZZER_GPIO         5           // GPIO pin for buzzer
#define BUZZER_TIMER        LEDC_TIMER_0
#define BUZZER_MODE         LEDC_LOW_SPEED_MODE
#define BUZZER_CHANNEL      LEDC_CHANNEL_0
#define BUZZER_DUTY_RES     LEDC_TIMER_13_BIT
#define BUZZER_DEFAULT_FREQ 4000        // 4kHz default

#define IOCTL_BUZZER_SET_FREQ   0x2001
#define IOCTL_BUZZER_GET_FREQ   0x2002
#define IOCTL_BUZZER_SET_DUTY   0x2003

/*==============================================================================
 * Driver Context
 *============================================================================*/

typedef struct {
    bool initialized;
    bool is_on;
    uint32_t frequency;
    uint32_t duty;          // 0-8191 for 13-bit resolution
} buzzer_ctx_t;

static buzzer_ctx_t s_ctx = {
    .initialized = false,
    .is_on = false,
    .frequency = BUZZER_DEFAULT_FREQ,
    .duty = 4096,  // 50% duty cycle
};

/*==============================================================================
 * Hardware Initialization
 *============================================================================*/

static void buzzer_hw_init(void) {
    if (s_ctx.initialized) return;

    // Configure timer
    ledc_timer_config_t timer_conf = {
        .speed_mode = BUZZER_MODE,
        .timer_num = BUZZER_TIMER,
        .duty_resolution = BUZZER_DUTY_RES,
        .freq_hz = s_ctx.frequency,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    // Configure channel
    ledc_channel_config_t channel_conf = {
        .speed_mode = BUZZER_MODE,
        .channel = BUZZER_CHANNEL,
        .timer_sel = BUZZER_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = BUZZER_GPIO,
        .duty = 0,  // Start off
        .hpoint = 0,
    };
    ledc_channel_config(&channel_conf);

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "Buzzer initialized on GPIO %d", BUZZER_GPIO);
}

/*==============================================================================
 * VFS Operations
 *============================================================================*/

static int buzzer_open(const char *path, int flags, int mode) {
    ESP_LOGD(TAG, "buzzer_open()");
    buzzer_hw_init();
    return 0;
}

static int buzzer_close(int fd) {
    ESP_LOGD(TAG, "buzzer_close()");
    // Turn off buzzer on close
    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, 0);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
    s_ctx.is_on = false;
    return 0;
}

static ssize_t buzzer_write(int fd, const void *data, size_t size) {
    if (size == 0) return 0;

    const char *cmd = (const char *)data;
    uint32_t duty = 0;

    // "1" or any non-zero starts buzzer, "0" stops it
    if (cmd[0] == '1' || cmd[0] == 1) {
        duty = s_ctx.duty;
        s_ctx.is_on = true;
        ESP_LOGD(TAG, "Buzzer ON");
    } else {
        duty = 0;
        s_ctx.is_on = false;
        ESP_LOGD(TAG, "Buzzer OFF");
    }

    ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, duty);
    ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);

    return size;
}

static ssize_t buzzer_read(int fd, void *data, size_t size) {
    if (size == 0) return 0;

    // Return current state
    char *buf = (char *)data;
    buf[0] = s_ctx.is_on ? '1' : '0';

    return 1;
}

static int buzzer_ioctl(int fd, int cmd, va_list args) {
    switch (cmd) {
        case IOCTL_BUZZER_SET_FREQ: {
            int freq = va_arg(args, int);
            if (freq < 100 || freq > 20000) {
                errno = EINVAL;
                return -1;
            }
            s_ctx.frequency = freq;
            ledc_set_freq(BUZZER_MODE, BUZZER_TIMER, freq);
            ESP_LOGI(TAG, "Frequency set to %d Hz", freq);
            return 0;
        }

        case IOCTL_BUZZER_GET_FREQ: {
            int *freq_ptr = va_arg(args, int *);
            if (freq_ptr) {
                *freq_ptr = s_ctx.frequency;
            }
            return 0;
        }

        case IOCTL_BUZZER_SET_DUTY: {
            int duty = va_arg(args, int);
            if (duty < 0 || duty > 8191) {
                errno = EINVAL;
                return -1;
            }
            s_ctx.duty = duty;
            if (s_ctx.is_on) {
                ledc_set_duty(BUZZER_MODE, BUZZER_CHANNEL, duty);
                ledc_update_duty(BUZZER_MODE, BUZZER_CHANNEL);
            }
            return 0;
        }

        default:
            errno = EINVAL;
            return -1;
    }
}

/*==============================================================================
 * Driver Registration
 *============================================================================*/

void vfs_buzzer_register(void) {
    esp_vfs_t vfs = {
        .flags = ESP_VFS_FLAG_DEFAULT,
        .open = &buzzer_open,
        .close = &buzzer_close,
        .read = &buzzer_read,
        .write = &buzzer_write,
        .ioctl = &buzzer_ioctl,
    };

    esp_err_t err = esp_vfs_register("/dev/buzzer", &vfs, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register /dev/buzzer: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Registered /dev/buzzer driver");
    }
}
```

---

## **8\. Header Files**

Create `main/drivers/drivers.h`:

```c
#ifndef DRIVERS_H
#define DRIVERS_H

#include <stdint.h>

/*==============================================================================
 * Collision Sensor IOCTL Commands
 *============================================================================*/

#define IOCTL_COLLISION_SET_RATE      0x1001
#define IOCTL_COLLISION_GET_RATE      0x1002
#define IOCTL_COLLISION_SET_THRESHOLD 0x1003
#define IOCTL_COLLISION_ENABLE        0x1004
#define IOCTL_COLLISION_INJECT        0x1005

/*==============================================================================
 * Buzzer IOCTL Commands
 *============================================================================*/

#define IOCTL_BUZZER_SET_FREQ   0x2001
#define IOCTL_BUZZER_GET_FREQ   0x2002
#define IOCTL_BUZZER_SET_DUTY   0x2003

/*==============================================================================
 * Driver Registration Functions
 *============================================================================*/

void vfs_collision_register(void);
void vfs_buzzer_register(void);

// Data injection for simulation/testing
void collision_inject_data(uint32_t distance_cm);

#endif // DRIVERS_H
```

---

## **9\. QEMU Simulation for Demo2**

For QEMU simulation, buzzer and LED hardware are not physically present but the VFS drivers still work. The driver code controls GPIO 5 (buzzer) and GPIO 4 (LED).

**Running in QEMU:**

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

# Run QEMU
qemu-system-xtensa -nographic -machine esp32 \
    -drive file=merged-flash.bin,if=mtd,format=raw \
    -no-reboot
```

**Note:** In QEMU, GPIO outputs are not visually represented but driver code executes correctly. Check serial output for driver state changes.

---

## **10\. CMakeLists.txt Update**

```cmake
idf_component_register(
    SRCS
        "main.c"
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
)
```

---

## **11\. Usage in app\_main()**

```c
#include "drivers.h"

void app_main(void) {
    // Initialize LittleFS
    // ...

    // Initialize WiFi
    // ...

    // Register hardware drivers
    vfs_collision_register();
    vfs_buzzer_register();

    // Start ELF loader / C2 server
    // ...
}
```

---

## **12\. Testing the Drivers**

### **12.1 Buzzer Test Payload**

```c
// apps/buzzer_test/main.c
extern int open(const char *path, int flags, ...);
extern int write(int fd, const void *buf, int size);
extern int close(int fd);
extern void sleep(int seconds);

int main(void) {
    int fd = open("/dev/buzzer", 2);  // O_RDWR
    if (fd < 0) return 1;

    // Beep pattern
    for (int i = 0; i < 5; i++) {
        write(fd, "1", 1);  // ON
        sleep(1);
        write(fd, "0", 1);  // OFF
        sleep(1);
    }

    close(fd);
    return 0;
}
```

### **12.2 Collision Sensor Test**

```c
// apps/collision_test/main.c
extern int printf(const char *fmt, ...);
extern int open(const char *path, int flags, ...);
extern int read(int fd, void *buf, int size);
extern int ioctl(int fd, int cmd, ...);
extern int close(int fd);

#define IOCTL_COLLISION_SET_RATE 0x1001

int main(void) {
    int fd = open("/dev/collision", 0);  // O_RDONLY
    if (fd < 0) return 1;

    // Set 50ms update rate (20 Hz)
    ioctl(fd, IOCTL_COLLISION_SET_RATE, 50);

    unsigned int distance;
    for (int i = 0; i < 20; i++) {
        if (read(fd, &distance, 4) > 0) {
            printf("Distance: %u cm\n", distance);
        }
    }

    close(fd);
    return 0;
}
```