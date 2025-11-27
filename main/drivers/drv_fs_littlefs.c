/**
 * @file drv_fs_littlefs.c
 * @brief LittleFS filesystem driver for Linux Compatibility Layer
 *
 * Implements filesystem abstraction using LittleFS for flash storage.
 * LittleFS is ideal for embedded systems with its power-loss resilience
 * and wear leveling capabilities.
 */

#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_littlefs.h"
#include "esp_log.h"

#include "drivers.h"

static const char *TAG = "drv_fs_littlefs";

/*==============================================================================
 * LittleFS Driver Implementation
 *============================================================================*/

driver_result_t driver_fs_littlefs_init(void)
{
    driver_result_t result = { .err = ESP_OK, .message = NULL };

    ESP_LOGI(TAG, "Initializing LittleFS filesystem driver...");

    esp_vfs_littlefs_conf_t conf = {
        .base_path = LINUX_FS_MOUNT_POINT,
        .partition_label = LINUX_FS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            result.message = "Failed to mount or format filesystem";
        } else if (ret == ESP_ERR_NOT_FOUND) {
            result.message = "Failed to find LittleFS partition 'linux_fs'";
        } else {
            result.message = "Failed to initialize LittleFS";
        }
        result.err = ret;
        ESP_LOGE(TAG, "%s (%s)", result.message, esp_err_to_name(ret));
        return result;
    }

    // Get and log filesystem info
    size_t total = 0, used = 0;
    esp_littlefs_info(LINUX_FS_PARTITION_LABEL, &total, &used);

    ESP_LOGI(TAG, "LittleFS mounted at %s", LINUX_FS_MOUNT_POINT);
    ESP_LOGI(TAG, "  Total: %u KB, Used: %u KB, Free: %u KB",
             (unsigned)(total / 1024),
             (unsigned)(used / 1024),
             (unsigned)((total - used) / 1024));

    result.message = "LittleFS filesystem initialized successfully";
    return result;
}

driver_result_t driver_fs_littlefs_deinit(void)
{
    driver_result_t result = { .err = ESP_OK, .message = NULL };

    ESP_LOGI(TAG, "Unmounting LittleFS filesystem...");

    esp_err_t ret = esp_vfs_littlefs_unregister(LINUX_FS_PARTITION_LABEL);
    if (ret != ESP_OK) {
        result.err = ret;
        result.message = "Failed to unmount LittleFS";
        ESP_LOGE(TAG, "%s", result.message);
        return result;
    }

    result.message = "LittleFS filesystem unmounted";
    ESP_LOGI(TAG, "%s", result.message);
    return result;
}

esp_err_t driver_fs_littlefs_get_info(size_t *total_bytes, size_t *used_bytes)
{
    if (!total_bytes || !used_bytes) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = esp_littlefs_info(LINUX_FS_PARTITION_LABEL, total_bytes, used_bytes);
    return ret;
}

/*==============================================================================
 * Filesystem Utilities
 *============================================================================*/

/**
 * @brief List files in the Linux filesystem (for debugging)
 */
void driver_fs_list_files(void)
{
    DIR *d = opendir(LINUX_FS_MOUNT_POINT);
    if (d) {
        ESP_LOGI(TAG, "Files in %s:", LINUX_FS_MOUNT_POINT);
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            // Just log the filename - no need to stat for basic listing
            ESP_LOGI(TAG, "  %s", dir->d_name);
        }
        closedir(d);
    } else {
        ESP_LOGW(TAG, "Failed to open %s directory", LINUX_FS_MOUNT_POINT);
    }
}
