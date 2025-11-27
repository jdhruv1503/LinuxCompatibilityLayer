# Task 06: Driver Abstraction Layer - Implementation Summary

## Overview

This task implements a modular driver abstraction layer for the Linux Compatibility Layer, separating hardware/subsystem initialization from the main kernel binding loop. This enables:

1. **Modularity**: Each driver is self-contained and can be conditionally compiled
2. **Portability**: Easy to swap implementations (e.g., LittleFS vs SD card)
3. **Maintainability**: Clean separation of concerns

## Architecture

```
main/
├── main.c                    # Kernel main - calls drivers_init_all()
├── drivers/
│   ├── drivers.h             # Unified driver interface
│   ├── drv_network.c         # Network driver (OpenEth/EMAC)
│   ├── drv_fs_littlefs.c     # LittleFS filesystem driver
│   ├── drv_fs_sdcard.c       # SD card FATFS driver (optional)
│   └── drv_devices.c         # VFS device drivers (/dev/*)
├── syscalls/                 # POSIX syscall shims
└── vfs_drivers/              # VFS C2 pipe driver
```

## Driver Interface

All drivers implement a common interface defined in `drivers.h`:

```c
typedef struct {
    esp_err_t err;           // ESP_OK on success
    const char *message;     // Human-readable status message
} driver_result_t;

// Example driver init function signature:
driver_result_t driver_network_openeth_init(void);
driver_result_t driver_fs_littlefs_init(void);
driver_result_t driver_fs_sdcard_init(void);
```

## Unified Initialization

The `drivers_init_all()` function initializes all drivers in the correct order:

```c
esp_err_t drivers_init_all(bool use_sdcard)
{
    // 1. Network (OpenEth for QEMU)
    driver_network_openeth_init();

    // 2. Filesystem (LittleFS or SD card)
    if (use_sdcard) {
        driver_fs_sdcard_init();  // Falls back to LittleFS if fails
    } else {
        driver_fs_littlefs_init();
    }

    // 3. Device drivers (/dev/c2, /dev/collision, /dev/buzzer)
    driver_dev_c2_pipe_init();
    driver_dev_collision_init();
    driver_dev_buzzer_init();

    return ESP_OK;
}
```

## Drivers Implemented

### 1. Network Driver (`drv_network.c`)

- **OpenEth**: For QEMU simulation (uses `esp_eth_mac_new_openeth`)
- **ESP32 EMAC**: For real hardware (uses `esp_eth_mac_new_esp32`)
- Automatically selects based on `__has_include("esp_eth_mac_openeth.h")`
- Provides `driver_network_wait_for_ip()` for blocking until DHCP completes

### 2. LittleFS Filesystem Driver (`drv_fs_littlefs.c`)

- Mounts at `/linux` mount point
- Uses `linux_fs` partition label
- Format on mount failure (configurable)
- Provides `driver_fs_littlefs_get_info()` for usage statistics

### 3. SD Card Filesystem Driver (`drv_fs_sdcard.c`)

- Alternative to LittleFS for real hardware deployments
- Uses SDMMC interface with FAT filesystem
- **Not supported in QEMU** - returns `ESP_ERR_NOT_SUPPORTED`
- Configurable for 1-bit or 4-bit mode
- Provides `driver_fs_sdcard_get_info()` for capacity/free space

### 4. Device Drivers (`drv_devices.c`)

#### `/dev/c2` - C2 Pipe
- Stdout redirection to network socket
- Wraps existing `vfs_c2_pipe.c` implementation

#### `/dev/collision` - Virtual Collision Sensor
- Demo driver for distance sensor simulation
- Uses FreeRTOS queue for blocking read operations
- Supports ioctl commands:
  - `IOCTL_COLLISION_SET_RATE` (0x1001)
  - `IOCTL_COLLISION_GET_RATE` (0x1002)
  - `IOCTL_COLLISION_SET_THRESHOLD` (0x1003)
  - `IOCTL_COLLISION_ENABLE` (0x1004)
  - `IOCTL_COLLISION_INJECT` (0x1005)

#### `/dev/buzzer` - Virtual Buzzer
- Demo driver for PWM buzzer control
- Write "1" to turn on, "0" to turn off
- Supports ioctl commands:
  - `IOCTL_BUZZER_SET_FREQ` (0x2001)
  - `IOCTL_BUZZER_GET_FREQ` (0x2002)
  - `IOCTL_BUZZER_SET_DUTY` (0x2003)

## Main.c Simplification

The new `main.c` is significantly cleaner:

```c
void app_main(void)
{
    // ... banner and logging setup ...

    // Initialize all drivers via unified driver subsystem
    esp_err_t ret = drivers_init_all(USE_SDCARD_FILESYSTEM);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Driver initialization failed!");
        return;
    }

    // List available ELF files
    list_elf_files();

    // Load and run default ELF
    load_and_run_elf(DEFAULT_ELF_PATH, argc, argv);
}
```

## Configuration Options

In `main.c`:

```c
// Set to true to use SD card instead of LittleFS
// For QEMU simulation, this MUST be false
#define USE_SDCARD_FILESYSTEM false

// Default ELF to execute on boot
#define DEFAULT_ELF_PATH "/linux/c2_redirect.elf"
```

## Files Added/Modified

### New Files
| File | Purpose |
|------|---------|
| `main/drivers/drivers.h` | Unified driver interface header |
| `main/drivers/drv_network.c` | Network driver (OpenEth/EMAC) |
| `main/drivers/drv_fs_littlefs.c` | LittleFS filesystem driver |
| `main/drivers/drv_fs_sdcard.c` | SD card FATFS driver |
| `main/drivers/drv_devices.c` | VFS device drivers |

### Modified Files
| File | Changes |
|------|---------|
| `main/main.c` | Refactored to use driver subsystem |
| `main/CMakeLists.txt` | Added new driver source files |

## CMakeLists.txt Update

```cmake
idf_component_register(
    SRCS
        "main.c"
        "syscalls/shim_unistd.c"
        "syscalls/shim_socket.c"
        "syscalls/shim_process.c"
        "vfs_drivers/vfs_c2_pipe.c"
        "drivers/drv_network.c"
        "drivers/drv_fs_littlefs.c"
        "drivers/drv_fs_sdcard.c"
        "drivers/drv_devices.c"
    INCLUDE_DIRS
        "."
        "syscalls"
        "vfs_drivers"
        "drivers"
)
```

## Testing Results

QEMU simulation output confirms all drivers initialize successfully:

```
I (1927) kernel_main: ============================================
I (1927) kernel_main:   Linux Compatibility Layer - ESP32
I (1927) kernel_main:   ELF Loader Subsystem v2.0
I (1927) kernel_main: ============================================
I (1927) kernel_main: Initializing driver subsystem...
I (1927) drv_devices: ============================================
I (1927) drv_devices:   Driver Subsystem Initialization
I (1927) drv_devices: ============================================
I (1937) drv_network: Initializing network driver (OpenEth for QEMU)...
I (2287) drv_network: Using OpenEth MAC (QEMU optimized)
I (2287) drv_network: Installing Ethernet driver...
I (2317) drv_network: Attaching to netif...
I (2317) drv_network: Starting Ethernet...
I (2417) drv_network: Network driver initialized successfully
I (2417) drv_fs_littlefs: Initializing LittleFS filesystem driver...
I (6317) drv_fs_littlefs: LittleFS mounted at /linux
I (6317) drv_fs_littlefs:   Total: 1536 KB, Used: 40 KB, Free: 1496 KB
I (6317) drv_devices: C2 pipe device initialized
I (6317) drv_devices: Collision sensor device initialized
I (6317) drv_devices: Buzzer device initialized
I (6327) drv_devices: ============================================
I (6327) drv_devices:   All drivers initialized successfully
I (6327) drv_devices: ============================================
```

## Usage for Guest ELFs

Guest ELF applications can use standard POSIX APIs to interact with drivers:

```c
// Collision sensor example
int fd = open("/dev/collision", O_RDONLY);
ioctl(fd, IOCTL_COLLISION_SET_RATE, 50);  // 20Hz update

uint32_t distance;
read(fd, &distance, sizeof(distance));    // Blocks until data ready

close(fd);

// Buzzer example
int bz = open("/dev/buzzer", O_WRONLY);
ioctl(bz, IOCTL_BUZZER_SET_FREQ, 1000);   // 1kHz
write(bz, "1", 1);                         // Turn on
sleep(1);
write(bz, "0", 1);                         // Turn off
close(bz);
```

## Future Improvements

1. **Additional Drivers**: Add I2C, SPI, ADC drivers for real sensors
2. **Hot-plug Support**: Add SD card insertion/removal detection
3. **Driver Registry**: Dynamic driver loading from configuration
4. **Power Management**: Add driver suspend/resume hooks
