/**
 * @file drv_network.c
 * @brief Network driver abstraction for Linux Compatibility Layer
 *
 * Implements Ethernet initialization for OpenEth (QEMU) and standard ESP32 EMAC.
 * Provides a unified interface for network subsystem setup.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

// Check if OpenEth is available (QEMU mode)
#if __has_include("esp_eth_mac_openeth.h")
#include "esp_eth_mac_openeth.h"
#define USE_OPENETH 1
#else
#include "esp_eth_mac_esp.h"
#define USE_OPENETH 0
#endif

#include "drivers.h"

static const char *TAG = "drv_network";

/*==============================================================================
 * Event Groups for IP Synchronization
 *============================================================================*/

static EventGroupHandle_t s_network_event_group = NULL;
#define NETWORK_GOT_IP_BIT BIT0

/*==============================================================================
 * Event Handlers
 *============================================================================*/

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");

    if (s_network_event_group) {
        xEventGroupSetBits(s_network_event_group, NETWORK_GOT_IP_BIT);
    }
}

/*==============================================================================
 * OpenEth Driver (QEMU Compatible)
 *============================================================================*/

driver_result_t driver_network_openeth_init(void)
{
    driver_result_t result = { .err = ESP_OK, .message = NULL };

    ESP_LOGI(TAG, "Initializing network driver (OpenEth for QEMU)...");

    // Create event group for IP synchronization
    s_network_event_group = xEventGroupCreate();
    if (!s_network_event_group) {
        result.err = ESP_ERR_NO_MEM;
        result.message = "Failed to create event group";
        return result;
    }

    // Initialize NVS (required for network)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        result.err = ret;
        result.message = "NVS flash init failed";
        return result;
    }

    // Initialize TCP/IP stack
    ret = esp_netif_init();
    if (ret != ESP_OK) {
        result.err = ret;
        result.message = "esp_netif_init failed";
        return result;
    }

    // Create default event loop
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        result.err = ret;
        result.message = "Event loop creation failed";
        return result;
    }

    // Create Ethernet netif instance
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    if (!eth_netif) {
        result.err = ESP_FAIL;
        result.message = "Failed to create esp_netif";
        return result;
    }

    // Register IP event handler
    ret = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                     &got_ip_event_handler, NULL);
    if (ret != ESP_OK) {
        result.err = ret;
        result.message = "Failed to register IP event handler";
        return result;
    }

#if USE_OPENETH
    // OpenEth MAC configuration (QEMU)
    ESP_LOGI(TAG, "Using OpenEth MAC (QEMU optimized)");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.autonego_timeout_ms = 100;
    esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
#else
    // Internal ESP32 EMAC (real hardware)
    ESP_LOGI(TAG, "Using Internal ESP32 EMAC");

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_config, &esp32_emac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = -1;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
#endif

    if (!mac || !phy) {
        result.err = ESP_FAIL;
        result.message = "Failed to create MAC or PHY";
        return result;
    }

    // Install Ethernet driver
    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;

    ESP_LOGI(TAG, "Installing Ethernet driver...");
    ret = esp_eth_driver_install(&config, &eth_handle);
    if (ret != ESP_OK) {
        result.err = ret;
        result.message = "Ethernet driver install failed";
        return result;
    }

    if (!eth_handle) {
        result.err = ESP_FAIL;
        result.message = "eth_handle is NULL after install";
        return result;
    }

    // Attach to netif
    ESP_LOGI(TAG, "Attaching to netif...");
    ret = esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle));
    if (ret != ESP_OK) {
        result.err = ret;
        result.message = "Failed to attach netif";
        return result;
    }

    // Start Ethernet
    ESP_LOGI(TAG, "Starting Ethernet...");
    ret = esp_eth_start(eth_handle);
    if (ret != ESP_OK) {
        result.err = ret;
        result.message = "Ethernet start failed";
        return result;
    }

    result.message = "Network driver initialized successfully";
    ESP_LOGI(TAG, "%s", result.message);
    return result;
}

/*==============================================================================
 * Network Utilities
 *============================================================================*/

bool driver_network_wait_for_ip(uint32_t timeout_ms)
{
    if (!s_network_event_group) {
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_network_event_group,
        NETWORK_GOT_IP_BIT,
        pdFALSE,  // Don't clear on exit
        pdTRUE,   // Wait for all bits
        pdMS_TO_TICKS(timeout_ms)
    );

    return (bits & NETWORK_GOT_IP_BIT) != 0;
}
