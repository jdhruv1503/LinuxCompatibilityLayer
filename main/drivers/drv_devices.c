/**
 * @file drv_devices.c
 * @brief VFS device drivers for Linux Compatibility Layer
 *
 * Implements device drivers including:
 * - C2 pipe for stdout redirection (/dev/c2)
 * - Virtual collision sensor (/dev/collision)
 * - PWM buzzer control (/dev/buzzer)
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

#include "drivers.h"
#include "vfs_c2_pipe.h"

static const char *TAG = "drv_devices";

/*==============================================================================
 * C2 Pipe Device (/dev/c2)
 *============================================================================*/

driver_result_t driver_dev_c2_pipe_init(void)
{
    driver_result_t result = { .err = ESP_OK, .message = NULL };

    ESP_LOGI(TAG, "Initializing /dev/c2 device driver...");

    // Use the existing VFS C2 pipe implementation
    vfs_c2_pipe_register();

    result.message = "C2 pipe device initialized";
    return result;
}

/*==============================================================================
 * Collision Sensor Device (/dev/collision) - Demo Driver
 *============================================================================*/

typedef struct {
    QueueHandle_t data_queue;
    TimerHandle_t sim_timer;
    int update_rate_ms;
    int alert_threshold_cm;
    bool enabled;
    bool is_open;
    uint32_t simulated_distance;
} collision_ctx_t;

static collision_ctx_t s_collision_ctx = {
    .data_queue = NULL,
    .sim_timer = NULL,
    .update_rate_ms = 100,
    .alert_threshold_cm = 30,
    .enabled = true,
    .is_open = false,
    .simulated_distance = 100,
};

static void collision_sim_timer_cb(TimerHandle_t timer)
{
    if (!s_collision_ctx.enabled || !s_collision_ctx.is_open) {
        return;
    }

    // Simulate distance measurement with noise
    uint32_t distance = s_collision_ctx.simulated_distance;
    int noise = (esp_random() % 10) - 5;
    distance = (distance + noise > 0) ? distance + noise : 1;

    xQueueSend(s_collision_ctx.data_queue, &distance, 0);
}

static int collision_open(const char *path, int flags, int mode)
{
    ESP_LOGD(TAG, "collision_open(path='%s')", path);

    if (s_collision_ctx.is_open) {
        errno = EBUSY;
        return -1;
    }

    s_collision_ctx.is_open = true;

    if (s_collision_ctx.sim_timer) {
        xTimerStart(s_collision_ctx.sim_timer, 0);
    }

    return 0;
}

static int collision_close(int fd)
{
    ESP_LOGD(TAG, "collision_close(fd=%d)", fd);

    if (s_collision_ctx.sim_timer) {
        xTimerStop(s_collision_ctx.sim_timer, 0);
    }

    if (s_collision_ctx.data_queue) {
        xQueueReset(s_collision_ctx.data_queue);
    }

    s_collision_ctx.is_open = false;
    return 0;
}

static ssize_t collision_read(int fd, void *dst, size_t size)
{
    if (!s_collision_ctx.is_open) {
        errno = EBADF;
        return -1;
    }

    if (size < sizeof(uint32_t)) {
        errno = EINVAL;
        return -1;
    }

    uint32_t distance;
    TickType_t timeout = pdMS_TO_TICKS(s_collision_ctx.update_rate_ms * 2);

    if (xQueueReceive(s_collision_ctx.data_queue, &distance, timeout) == pdTRUE) {
        memcpy(dst, &distance, sizeof(uint32_t));
        return sizeof(uint32_t);
    }

    errno = EAGAIN;
    return -1;
}

static ssize_t collision_write(int fd, const void *data, size_t size)
{
    if (size >= sizeof(uint32_t)) {
        uint32_t value;
        memcpy(&value, data, sizeof(uint32_t));
        s_collision_ctx.simulated_distance = value;
        ESP_LOGD(TAG, "Injected distance: %lu cm", (unsigned long)value);
        return sizeof(uint32_t);
    }

    errno = EINVAL;
    return -1;
}

static int collision_ioctl(int fd, int cmd, va_list args)
{
    switch (cmd) {
        case IOCTL_COLLISION_SET_RATE: {
            int rate = va_arg(args, int);
            if (rate < 10 || rate > 10000) {
                errno = EINVAL;
                return -1;
            }
            s_collision_ctx.update_rate_ms = rate;
            if (s_collision_ctx.sim_timer) {
                xTimerChangePeriod(s_collision_ctx.sim_timer, pdMS_TO_TICKS(rate), 0);
            }
            ESP_LOGD(TAG, "Collision rate set to %d ms", rate);
            return 0;
        }

        case IOCTL_COLLISION_GET_RATE: {
            int *rate_ptr = va_arg(args, int *);
            if (rate_ptr) {
                *rate_ptr = s_collision_ctx.update_rate_ms;
            }
            return 0;
        }

        case IOCTL_COLLISION_SET_THRESHOLD: {
            int threshold = va_arg(args, int);
            s_collision_ctx.alert_threshold_cm = threshold;
            return 0;
        }

        case IOCTL_COLLISION_ENABLE: {
            int enable = va_arg(args, int);
            s_collision_ctx.enabled = (enable != 0);
            return 0;
        }

        case IOCTL_COLLISION_INJECT: {
            uint32_t distance = va_arg(args, uint32_t);
            s_collision_ctx.simulated_distance = distance;
            return 0;
        }

        default:
            errno = EINVAL;
            return -1;
    }
}

static int collision_fstat(int fd, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    return 0;
}

driver_result_t driver_dev_collision_init(void)
{
    driver_result_t result = { .err = ESP_OK, .message = NULL };

    ESP_LOGI(TAG, "Initializing /dev/collision device driver...");

    // Create data queue
    s_collision_ctx.data_queue = xQueueCreate(10, sizeof(uint32_t));
    if (!s_collision_ctx.data_queue) {
        result.err = ESP_ERR_NO_MEM;
        result.message = "Failed to create collision data queue";
        return result;
    }

    // Create simulation timer
    s_collision_ctx.sim_timer = xTimerCreate(
        "collision_sim",
        pdMS_TO_TICKS(s_collision_ctx.update_rate_ms),
        pdTRUE,
        NULL,
        collision_sim_timer_cb
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
        result.err = err;
        result.message = "Failed to register /dev/collision";
        ESP_LOGE(TAG, "%s", result.message);
        return result;
    }

    result.message = "Collision sensor device initialized";
    ESP_LOGI(TAG, "%s", result.message);
    return result;
}

void driver_collision_inject_data(uint32_t distance_cm)
{
    s_collision_ctx.simulated_distance = distance_cm;

    if (s_collision_ctx.is_open && s_collision_ctx.data_queue) {
        xQueueSend(s_collision_ctx.data_queue, &distance_cm, 0);
    }
}

/*==============================================================================
 * Buzzer Device (/dev/buzzer) - Demo Driver
 *============================================================================*/

// Note: This is a virtual buzzer for QEMU simulation.
// On real hardware, this would use LEDC PWM driver.

typedef struct {
    bool is_on;
    uint32_t frequency;
    uint32_t duty;
} buzzer_ctx_t;

static buzzer_ctx_t s_buzzer_ctx = {
    .is_on = false,
    .frequency = 4000,
    .duty = 50,
};

static int buzzer_open(const char *path, int flags, int mode)
{
    ESP_LOGD(TAG, "buzzer_open()");
    return 0;
}

static int buzzer_close(int fd)
{
    ESP_LOGD(TAG, "buzzer_close()");
    s_buzzer_ctx.is_on = false;
    return 0;
}

static ssize_t buzzer_write(int fd, const void *data, size_t size)
{
    if (size == 0) return 0;

    const char *cmd = (const char *)data;

    if (cmd[0] == '1' || cmd[0] == 1) {
        s_buzzer_ctx.is_on = true;
        ESP_LOGD(TAG, "Buzzer ON (freq=%lu Hz)", (unsigned long)s_buzzer_ctx.frequency);
    } else {
        s_buzzer_ctx.is_on = false;
        ESP_LOGD(TAG, "Buzzer OFF");
    }

    return size;
}

static ssize_t buzzer_read(int fd, void *data, size_t size)
{
    if (size == 0) return 0;

    char *buf = (char *)data;
    buf[0] = s_buzzer_ctx.is_on ? '1' : '0';
    return 1;
}

static int buzzer_ioctl(int fd, int cmd, va_list args)
{
    switch (cmd) {
        case IOCTL_BUZZER_SET_FREQ: {
            int freq = va_arg(args, int);
            if (freq < 100 || freq > 20000) {
                errno = EINVAL;
                return -1;
            }
            s_buzzer_ctx.frequency = freq;
            ESP_LOGD(TAG, "Buzzer frequency set to %d Hz", freq);
            return 0;
        }

        case IOCTL_BUZZER_GET_FREQ: {
            int *freq_ptr = va_arg(args, int *);
            if (freq_ptr) {
                *freq_ptr = s_buzzer_ctx.frequency;
            }
            return 0;
        }

        case IOCTL_BUZZER_SET_DUTY: {
            int duty = va_arg(args, int);
            if (duty < 0 || duty > 100) {
                errno = EINVAL;
                return -1;
            }
            s_buzzer_ctx.duty = duty;
            return 0;
        }

        default:
            errno = EINVAL;
            return -1;
    }
}

driver_result_t driver_dev_buzzer_init(void)
{
    driver_result_t result = { .err = ESP_OK, .message = NULL };

    ESP_LOGI(TAG, "Initializing /dev/buzzer device driver...");

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
        result.err = err;
        result.message = "Failed to register /dev/buzzer";
        ESP_LOGE(TAG, "%s", result.message);
        return result;
    }

    result.message = "Buzzer device initialized";
    ESP_LOGI(TAG, "%s", result.message);
    return result;
}

/*==============================================================================
 * Unified Driver Initialization
 *============================================================================*/

esp_err_t drivers_init_all(bool use_sdcard)
{
    driver_result_t result;

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Driver Subsystem Initialization");
    ESP_LOGI(TAG, "============================================");

    // 1. Initialize Network
    result = driver_network_openeth_init();
    if (result.err != ESP_OK) {
        ESP_LOGE(TAG, "Network driver failed: %s", result.message);
        return result.err;
    }

    // 2. Initialize Filesystem
    if (use_sdcard) {
        result = driver_fs_sdcard_init();
        if (result.err != ESP_OK) {
            ESP_LOGW(TAG, "SD card init failed, falling back to LittleFS");
            result = driver_fs_littlefs_init();
        }
    } else {
        result = driver_fs_littlefs_init();
    }

    if (result.err != ESP_OK) {
        ESP_LOGE(TAG, "Filesystem driver failed: %s", result.message);
        return result.err;
    }

    // 3. Initialize Device Drivers
    result = driver_dev_c2_pipe_init();
    if (result.err != ESP_OK) {
        ESP_LOGW(TAG, "C2 pipe driver failed: %s", result.message);
        // Continue - not critical
    }

    // Optional demo drivers
    result = driver_dev_collision_init();
    if (result.err != ESP_OK) {
        ESP_LOGW(TAG, "Collision driver failed: %s", result.message);
    }

    result = driver_dev_buzzer_init();
    if (result.err != ESP_OK) {
        ESP_LOGW(TAG, "Buzzer driver failed: %s", result.message);
    }

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  All drivers initialized successfully");
    ESP_LOGI(TAG, "============================================");

    return ESP_OK;
}
