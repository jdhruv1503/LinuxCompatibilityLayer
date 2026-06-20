/**
 * @file main.c
 * @brief Linux Compatibility Layer - Kernel Main
 *
 * This is the main entry point for the Linux Compatibility Layer (TLCL).
 * It initializes all driver subsystems and loads guest ELF binaries.
 *
 * Architecture:
 * - Drivers are modular and initialized via drivers.h interface
 * - Network: OpenEth for QEMU, internal EMAC for real hardware
 * - Filesystem: LittleFS (QEMU/flash) or SD card (real hardware)
 * - Devices: VFS-based device drivers (/dev/c2, /dev/collision, etc.)
 */

#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>

#include "lwip/sockets.h"

#include "esp_log.h"
#include "esp_elf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Driver subsystem
#include "drivers/drivers.h"



static const char *TAG = "kernel_main";

extern int lcl_heap_snprint_all(char *buf, size_t buflen);
extern void lcl_heap_print_all(void);

/*==============================================================================
 * Configuration
 *============================================================================*/

// Set to true to use SD card instead of LittleFS
// For QEMU simulation, this MUST be false
#define USE_SDCARD_FILESYSTEM false

// Default ELF to execute on boot
// Updated via build_and_run.py --set-elf <app_name>
#define DEFAULT_ELF_PATH "/linux/energy_mgmt.elf"

#include <math.h>

// Dummy function to force linking of math symbols
void force_link_math(void) {
    volatile float f = 1.0f;
    volatile double d = 1.0;
    f = sinf(f) + cosf(f) + tanf(f) + sqrtf(f) + powf(f, 2.0f) + expf(f) + logf(f);
    d = sin(d) + cos(d) + tan(d) + sqrt(d) + pow(d, 2.0) + exp(d) + log(d);
    printf("Math check: %f %f\n", f, d);
}

/*==============================================================================
 * ELF Loader
 *============================================================================*/

/**
 * @brief Load and execute an ELF binary from the filesystem
 *
 * @param filepath Path to the ELF file (e.g., "/linux/hello_world.elf")
 * @param argc Argument count to pass to the ELF main()
 * @param argv Argument values to pass to the ELF main()
 * @return int Return value from the ELF program, or negative on error
 */
int load_and_run_elf(const char *filepath, int argc, char *argv[])
{
    esp_elf_t elf;
    int ret;

    ESP_LOGI(TAG, "Loading ELF: %s", filepath);

    // 1. Initialize the ELF structure
    ret = esp_elf_init(&elf);
    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to init ELF loader: %d", ret);
        return -1;
    }

    // 2. Read ELF file from filesystem
    FILE *f = fopen(filepath, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open ELF file: %s", filepath);
        esp_elf_deinit(&elf);
        return -1;
    }

    // Get file size
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);

    ESP_LOGI(TAG, "ELF file size: %u bytes", (unsigned int)size);

    // Allocate buffer and read file
    uint8_t *elf_data = malloc(size);
    if (!elf_data) {
        ESP_LOGE(TAG, "Failed to allocate %u bytes for ELF data", (unsigned int)size);
        fclose(f);
        esp_elf_deinit(&elf);
        return -1;
    }

    size_t bytes_read = fread(elf_data, 1, size, f);
    fclose(f);

    if (bytes_read != size) {
        ESP_LOGE(TAG, "Failed to read ELF file: got %u of %u bytes",
                 (unsigned int)bytes_read, (unsigned int)size);
        free(elf_data);
        esp_elf_deinit(&elf);
        return -1;
    }

    // 3. Relocate ELF from buffer
    ret = esp_elf_relocate(&elf, elf_data);

    // Free the raw file buffer after relocation
    free(elf_data);

    if (ret != 0) {
        ESP_LOGE(TAG, "Failed to relocate ELF: %d", ret);
        esp_elf_deinit(&elf);
        return -1;
    }

    // 4. Execute
    ESP_LOGI(TAG, "Starting ELF...");

    if (elf.entry) {
        ret = elf.entry(argc, argv);
        ESP_LOGI(TAG, "ELF exited with: %d", ret);
    } else {
        ESP_LOGE(TAG, "No entry point found in ELF");
        ret = -1;
    }

    // 5. Cleanup
    esp_elf_deinit(&elf);
    return ret;
}

/*==============================================================================
 * Filesystem Utilities
 *============================================================================*/

/**
 * @brief List ELF files in the Linux filesystem
 */
static void list_elf_files(void)
{
    DIR *d = opendir(LINUX_FS_MOUNT_POINT);
    if (d) {
        ESP_LOGI(TAG, "Files in %s:", LINUX_FS_MOUNT_POINT);
        struct dirent *dir;
        while ((dir = readdir(d)) != NULL) {
            ESP_LOGI(TAG, "  %s", dir->d_name);
        }
        closedir(d);
    } else {
        ESP_LOGE(TAG, "Failed to open %s directory", LINUX_FS_MOUNT_POINT);
    }
}

/*==============================================================================
 * Application Entry Point
 *============================================================================*/

void app_main(void)
{
    // Ensure math symbols are linked
    force_link_math();

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "  Linux Compatibility Layer - ESP32");
    ESP_LOGI(TAG, "  ELF Loader Subsystem v2.0");
    ESP_LOGI(TAG, "============================================");

    // Set log levels - use ESP_LOG_WARN for production, ESP_LOG_DEBUG for debugging
    // These logs go directly to UART, not through shim_write (no infinite loop risk)
    esp_log_level_set("lwip", ESP_LOG_WARN);
    esp_log_level_set("esp_netif", ESP_LOG_WARN);
    esp_log_level_set("shim_socket", ESP_LOG_WARN);
    esp_log_level_set("shim_unistd", ESP_LOG_WARN);
    esp_log_level_set("drv_network", ESP_LOG_WARN);
    esp_log_level_set("drv_fs_littlefs", ESP_LOG_WARN);
    esp_log_level_set("drv_devices", ESP_LOG_WARN);
    esp_log_level_set("kernel_main", ESP_LOG_INFO);
    esp_log_level_set("c2_bot", ESP_LOG_INFO);
    esp_log_level_set("c2_server", ESP_LOG_INFO);
    esp_log_level_set("telemetry_bridge", ESP_LOG_INFO);


    // Initialize all drivers via unified driver subsystem
    ESP_LOGI(TAG, "Initializing driver subsystem...");
    esp_err_t ret = drivers_init_all(USE_SDCARD_FILESYSTEM);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Driver initialization failed!");
        return;
    }



    // List available ELF files
    list_elf_files();

    // Run default ELF if it exists
    struct stat st;
    if (stat(DEFAULT_ELF_PATH, &st) == 0) {
        ESP_LOGI(TAG, "Found ELF: %s", DEFAULT_ELF_PATH);

        // Give a small delay for serial output to flush
        vTaskDelay(pdMS_TO_TICKS(100));

        // Extract filename for argv[0]
        const char *filename = strrchr(DEFAULT_ELF_PATH, '/');
        filename = filename ? filename + 1 : DEFAULT_ELF_PATH;

        // Run the ELF with test arguments
        char *argv[] = {(char *)filename, NULL};
        int result = load_and_run_elf(DEFAULT_ELF_PATH, 1, argv);

        ESP_LOGI(TAG, "ELF returned: %d", result);
    } else {
        ESP_LOGW(TAG, "No ELF found at %s", DEFAULT_ELF_PATH);
        ESP_LOGI(TAG, "To test the ELF loader:");
        ESP_LOGI(TAG, "  1. Build a guest ELF with: tools/build_guest_app.bat <app> (Windows) or make -f tools/Makefile.guest APP=apps/<app> (Linux/macOS)");
        ESP_LOGI(TAG, "  2. Copy it to the 'data' folder");
        ESP_LOGI(TAG, "  3. Rebuild and flash the firmware");
    }

    ESP_LOGI(TAG, "============================================");
    ESP_LOGI(TAG, "Kernel idle - All services started");
    ESP_LOGI(TAG, "============================================");
}
