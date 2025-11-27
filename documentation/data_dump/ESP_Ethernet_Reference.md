# ESP-IDF Ethernet Driver Reference

## Overview

ESP-IDF provides Ethernet driver support through the `esp_eth` component. For QEMU simulation, use the OpenEth virtual MAC instead of the hardware ESP32 EMAC.

## Key APIs

### MAC Layer

```c
#include "esp_eth.h"
#include "esp_eth_mac.h"

// For real hardware (ESP32 internal EMAC)
eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
esp32_emac_config.smi_gpio.mdc_num = GPIO_NUM_23;
esp32_emac_config.smi_gpio.mdio_num = GPIO_NUM_18;
esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);

// For QEMU (OpenEth virtual MAC)
eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_config);
```

### PHY Layer

```c
#include "esp_eth_phy.h"

eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
phy_config.phy_addr = 1;
phy_config.reset_gpio_num = -1;  // No reset GPIO for QEMU

// Generic PHY (works with most PHYs including QEMU)
esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_config);

// Or specific PHY drivers:
esp_eth_phy_t *phy = esp_eth_phy_new_dp83848(&phy_config);
esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
```

### Driver Installation

```c
esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
esp_eth_handle_t eth_handle = NULL;

ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));
```

### Network Interface Integration

```c
#include "esp_netif.h"

// Create default Ethernet netif
esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
esp_netif_t *eth_netif = esp_netif_new(&cfg);

// Create glue and attach
esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
ESP_ERROR_CHECK(esp_netif_attach(eth_netif, glue));

// Start Ethernet
ESP_ERROR_CHECK(esp_eth_start(eth_handle));
```

### Event Handling

```c
#include "esp_event.h"

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Up");
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Ethernet Link Down");
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
}

// Register handlers
ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                           &eth_event_handler, NULL));
ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                           &got_ip_handler, NULL));
```

## Kconfig Options

```ini
# Enable Ethernet
CONFIG_ETH_ENABLED=y

# For QEMU (OpenEth)
CONFIG_ETH_USE_OPENETH=y
CONFIG_ETH_USE_ESP32_EMAC=n

# For real hardware
CONFIG_ETH_USE_ESP32_EMAC=y
CONFIG_ETH_USE_OPENETH=n
```

## QEMU Command Line

```bash
# Basic networking (user mode NAT)
qemu-system-xtensa -nographic -machine esp32 \
    -drive file=merged-flash.bin,if=mtd,format=raw \
    -nic user,model=open_eth

# With port forwarding (host:8080 -> guest:80)
qemu-system-xtensa -nographic -machine esp32 \
    -drive file=merged-flash.bin,if=mtd,format=raw \
    -nic user,model=open_eth,hostfwd=tcp::8080-:80
```

## References

- ESP-IDF Ethernet API: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/network/esp_eth.html
- ESP-IDF QEMU Guide: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/qemu.html
- Basic Ethernet Example: https://github.com/espressif/esp-idf/tree/master/examples/ethernet/basic
