/**
 * @file drivers.h
 * @brief Unified driver interface for Linux Compatibility Layer
 *
 * This header defines the driver abstraction layer that allows modular
 * hardware/subsystem configuration. Each driver implements a common
 * init/deinit interface and can be conditionally compiled.
 */

#ifndef DRIVERS_H
#define DRIVERS_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * Driver Type Definitions
 *============================================================================*/

/**
 * @brief Driver types for identification
 */
typedef enum {
    DRIVER_TYPE_NETWORK,     // Network interface drivers
    DRIVER_TYPE_FILESYSTEM,  // Filesystem drivers (LittleFS, FATFS, etc.)
    DRIVER_TYPE_DEVICE,      // Device drivers (/dev/*)
} driver_type_t;

/**
 * @brief Driver initialization result
 */
typedef struct {
    esp_err_t err;           // ESP_OK on success
    const char *message;     // Human-readable status message
} driver_result_t;

/*==============================================================================
 * Network Driver Interface
 *============================================================================*/

/**
 * @brief Initialize OpenEth network driver for QEMU
 *
 * Sets up Ethernet with OpenEth MAC (QEMU compatible) and DP83848 PHY.
 * Registers IP event handlers for DHCP.
 *
 * @return Driver result with status
 */
driver_result_t driver_network_openeth_init(void);

/**
 * @brief Wait for network to obtain IP address
 *
 * Blocks until DHCP assigns an IP or timeout expires.
 *
 * @param timeout_ms Maximum time to wait in milliseconds
 * @return true if IP obtained, false on timeout
 */
bool driver_network_wait_for_ip(uint32_t timeout_ms);

/*==============================================================================
 * Filesystem Driver Interface
 *============================================================================*/

/**
 * @brief Mount point for the Linux filesystem
 *
 * All guest ELFs access files relative to this path.
 */
#define LINUX_FS_MOUNT_POINT "/linux"

/**
 * @brief Partition label for filesystem storage
 */
#define LINUX_FS_PARTITION_LABEL "linux_fs"

/**
 * @brief Initialize LittleFS filesystem driver
 *
 * Mounts LittleFS partition at LINUX_FS_MOUNT_POINT.
 * Formats partition if mount fails (configurable).
 *
 * @return Driver result with status
 */
driver_result_t driver_fs_littlefs_init(void);

/**
 * @brief Deinitialize LittleFS filesystem driver
 *
 * Unmounts the filesystem and releases resources.
 *
 * @return Driver result with status
 */
driver_result_t driver_fs_littlefs_deinit(void);

/**
 * @brief Get LittleFS filesystem usage statistics
 *
 * @param total_bytes Output: total filesystem size in bytes
 * @param used_bytes Output: used space in bytes
 * @return ESP_OK on success
 */
esp_err_t driver_fs_littlefs_get_info(size_t *total_bytes, size_t *used_bytes);

/**
 * @brief Initialize SD card FATFS filesystem driver
 *
 * Mounts SD card via SDMMC interface at LINUX_FS_MOUNT_POINT.
 * Uses FAT filesystem compatible with most SD cards.
 *
 * Note: For QEMU simulation, this driver is not available.
 * Use LittleFS for QEMU testing.
 *
 * @return Driver result with status
 */
driver_result_t driver_fs_sdcard_init(void);

/**
 * @brief Deinitialize SD card filesystem driver
 *
 * Unmounts the SD card and releases SDMMC resources.
 *
 * @return Driver result with status
 */
driver_result_t driver_fs_sdcard_deinit(void);

/**
 * @brief Get SD card filesystem usage statistics
 *
 * @param total_bytes Output: total filesystem size in bytes
 * @param free_bytes Output: free space in bytes
 * @return ESP_OK on success
 */
esp_err_t driver_fs_sdcard_get_info(size_t *total_bytes, size_t *free_bytes);

/*==============================================================================
 * Device Driver Interface (VFS Devices)
 *============================================================================*/

/**
 * @brief Initialize C2 pipe virtual device driver
 *
 * Registers /dev/c2 for stdout redirection to network socket.
 *
 * @return Driver result with status
 */
driver_result_t driver_dev_c2_pipe_init(void);

/*==============================================================================
 * Collision Sensor IOCTL Commands (Demo Driver)
 *============================================================================*/

#define IOCTL_COLLISION_SET_RATE      0x1001
#define IOCTL_COLLISION_GET_RATE      0x1002
#define IOCTL_COLLISION_SET_THRESHOLD 0x1003
#define IOCTL_COLLISION_ENABLE        0x1004
#define IOCTL_COLLISION_INJECT        0x1005

/*==============================================================================
 * Buzzer IOCTL Commands (Demo Driver)
 *============================================================================*/

#define IOCTL_BUZZER_SET_FREQ   0x2001
#define IOCTL_BUZZER_GET_FREQ   0x2002
#define IOCTL_BUZZER_SET_DUTY   0x2003

/*==============================================================================
 * Demo Device Drivers (Optional)
 *============================================================================*/

/**
 * @brief Initialize collision sensor driver
 *
 * Registers /dev/collision - a virtual distance sensor for Demo2.
 * Uses FreeRTOS queue for blocking read operations.
 *
 * @return Driver result with status
 */
driver_result_t driver_dev_collision_init(void);

/**
 * @brief Initialize buzzer driver
 *
 * Registers /dev/buzzer - PWM buzzer control for alarm demos.
 *
 * @return Driver result with status
 */
driver_result_t driver_dev_buzzer_init(void);

/**
 * @brief Inject distance data into collision sensor (for testing)
 *
 * @param distance_cm Distance value in centimeters
 */
void driver_collision_inject_data(uint32_t distance_cm);

/*==============================================================================
 * Driver Subsystem Initialization
 *============================================================================*/

/**
 * @brief Initialize all core drivers
 *
 * This function initializes the standard driver set:
 * - Network (OpenEth for QEMU)
 * - Filesystem (LittleFS by default, SD card optional)
 * - VFS devices (/dev/c2, etc.)
 *
 * @param use_sdcard If true, use SD card instead of LittleFS
 * @return ESP_OK if all drivers initialized successfully
 */
esp_err_t drivers_init_all(bool use_sdcard);

#ifdef __cplusplus
}
#endif

#endif // DRIVERS_H
