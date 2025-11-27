/**
 * @file drv_fs_sdcard.c
 * @brief SD Card filesystem driver for Linux Compatibility Layer
 *
 * Implements filesystem abstraction using FAT on SD card via SDMMC interface.
 * This driver is for real hardware deployments - it does NOT work in QEMU.
 *
 * Note: For QEMU simulation, use LittleFS driver instead.
 */

#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"

// Include ff.h for FATFS types
#include "ff.h"

// SDMMC driver - only available on real hardware with SDMMC support
#if defined(SOC_SDMMC_HOST_SUPPORTED) && SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#define SDCARD_SUPPORTED 1
#else
#define SDCARD_SUPPORTED 0
#endif

#include "drivers.h"

static const char *TAG = "drv_fs_sdcard";

/*==============================================================================
 * SD Card Configuration
 *============================================================================*/

// Default GPIO assignments for SD card (ESP32 default SDMMC pins)
#define SDCARD_PIN_CLK      GPIO_NUM_14
#define SDCARD_PIN_CMD      GPIO_NUM_15
#define SDCARD_PIN_D0       GPIO_NUM_2
#define SDCARD_PIN_D1       GPIO_NUM_4
#define SDCARD_PIN_D2       GPIO_NUM_12
#define SDCARD_PIN_D3       GPIO_NUM_13

// Use 1-bit mode for broader compatibility
#define SDCARD_USE_1BIT_MODE 1

/*==============================================================================
 * Driver State
 *============================================================================*/

#if SDCARD_SUPPORTED
static sdmmc_card_t *s_card = NULL;
#endif

/*==============================================================================
 * SD Card Driver Implementation
 *============================================================================*/

driver_result_t driver_fs_sdcard_init(void)
{
    driver_result_t result = { .err = ESP_OK, .message = NULL };

#if !SDCARD_SUPPORTED
    result.err = ESP_ERR_NOT_SUPPORTED;
    result.message = "SD card not supported on this platform (QEMU?)";
    ESP_LOGW(TAG, "%s", result.message);
    return result;
#else
    ESP_LOGI(TAG, "Initializing SD card filesystem driver...");

    // Mount configuration
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,  // Don't format, preserve data
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    // SDMMC host configuration
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    // Reduce frequency for stability
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    // Slot configuration with GPIO pins
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();

#if SDCARD_USE_1BIT_MODE
    // Use 1-bit mode for broader compatibility
    slot_config.width = 1;
    ESP_LOGI(TAG, "Using 1-bit SD card mode");
#else
    // 4-bit mode for higher speed
    slot_config.width = 4;
    ESP_LOGI(TAG, "Using 4-bit SD card mode");
#endif

    // Enable internal pullups
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting SD card at %s...", LINUX_FS_MOUNT_POINT);

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(
        LINUX_FS_MOUNT_POINT,
        &host,
        &slot_config,
        &mount_config,
        &s_card
    );

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            result.message = "Failed to mount filesystem on SD card";
        } else if (ret == ESP_ERR_INVALID_STATE) {
            result.message = "SD card already mounted";
        } else if (ret == ESP_ERR_NO_MEM) {
            result.message = "Not enough memory for SD card operation";
        } else {
            result.message = "Failed to initialize SD card";
        }
        result.err = ret;
        ESP_LOGE(TAG, "%s (%s)", result.message, esp_err_to_name(ret));
        return result;
    }

    // Card successfully mounted, print card info
    ESP_LOGI(TAG, "SD card mounted successfully");
    sdmmc_card_print_info(stdout, s_card);

    // Get and log filesystem info
    FATFS *fs;
    DWORD free_clusters;
    if (f_getfree("0:", &free_clusters, &fs) == FR_OK) {
        uint64_t total_bytes = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512;
        uint64_t free_bytes = (uint64_t)free_clusters * fs->csize * 512;

        ESP_LOGI(TAG, "  Total: %llu MB, Free: %llu MB",
                 total_bytes / (1024 * 1024),
                 free_bytes / (1024 * 1024));
    }

    result.message = "SD card filesystem initialized successfully";
    return result;
#endif
}

driver_result_t driver_fs_sdcard_deinit(void)
{
    driver_result_t result = { .err = ESP_OK, .message = NULL };

#if !SDCARD_SUPPORTED
    result.err = ESP_ERR_NOT_SUPPORTED;
    result.message = "SD card not supported on this platform";
    return result;
#else
    if (!s_card) {
        result.err = ESP_ERR_INVALID_STATE;
        result.message = "SD card not mounted";
        return result;
    }

    ESP_LOGI(TAG, "Unmounting SD card...");

    esp_err_t ret = esp_vfs_fat_sdcard_unmount(LINUX_FS_MOUNT_POINT, s_card);
    if (ret != ESP_OK) {
        result.err = ret;
        result.message = "Failed to unmount SD card";
        ESP_LOGE(TAG, "%s", result.message);
        return result;
    }

    s_card = NULL;
    result.message = "SD card unmounted successfully";
    ESP_LOGI(TAG, "%s", result.message);
    return result;
#endif
}

esp_err_t driver_fs_sdcard_get_info(size_t *total_bytes, size_t *free_bytes)
{
#if !SDCARD_SUPPORTED
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (!total_bytes || !free_bytes) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_card) {
        return ESP_ERR_INVALID_STATE;
    }

    FATFS *fs;
    DWORD free_clusters;
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res != FR_OK) {
        return ESP_FAIL;
    }

    *total_bytes = (size_t)((uint64_t)(fs->n_fatent - 2) * fs->csize * 512);
    *free_bytes = (size_t)((uint64_t)free_clusters * fs->csize * 512);

    return ESP_OK;
#endif
}

/*==============================================================================
 * SD Card Utilities (for hardware debugging)
 *============================================================================*/

#if SDCARD_SUPPORTED
/**
 * @brief Check if SD card is present and readable
 */
bool driver_sdcard_is_present(void)
{
    return s_card != NULL;
}

/**
 * @brief Get SD card size in bytes
 */
uint64_t driver_sdcard_get_capacity(void)
{
    if (!s_card) {
        return 0;
    }
    return (uint64_t)s_card->csd.capacity * 512;
}
#endif
