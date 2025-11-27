/**
 * @file main.c
 * @brief C2 Payload - Simulated WiFi Scanner
 *
 * This is a demonstration payload for the C2 (Command & Control) system.
 * It simulates a WiFi scanner that discovers nearby networks.
 *
 * When executed via the C2 server:
 * - All printf output is redirected to the C2 master via TCP socket
 * - Output also appears on the ESP32's UART for debugging
 */

// External symbols from host shim layer
extern int printf(const char *fmt, ...);
extern int puts(const char *s);
extern unsigned int sleep(unsigned int seconds);

// Simulated sleep using busy wait (for systems without sleep support)
static void delay_ms(int ms) {
    // This is a very rough delay - not accurate but works for demo
    volatile int i;
    for (int j = 0; j < ms; j++) {
        for (i = 0; i < 10000; i++) {
            // Busy wait
        }
    }
}

__attribute__((visibility("default")))
int app_main(int argc, char *argv[]) {
    puts("");
    puts("========================================");
    puts("  ESP32 C2 Payload - WiFi Scanner");
    puts("========================================");
    puts("");

    printf("Command: SCAN_WIFI\n");
    printf("Initializing wireless interface...\n");
    puts("");


    printf("[*] WiFi interface ready\n");
    printf("[*] Starting passive scan...\n");
    puts("");


    printf("[+] Scanning complete!\n");
    puts("");

    // Print mock results in a table format
    puts("----------------------------------------------------------");
    puts("SSID                     | RSSI  | CHAN | SECURITY");
    puts("----------------------------------------------------------");
    printf("%-24s | %-5d | %-4d | %s\n", "FBI_Surveillance_Van", -45, 6, "WPA2-PSK");
    printf("%-24s | %-5d | %-4d | %s\n", "Linksys_Home", -80, 1, "OPEN");
    printf("%-24s | %-5d | %-4d | %s\n", "Starbucks_Guest", -65, 11, "WPA2-EAP");
    printf("%-24s | %-5d | %-4d | %s\n", "NETGEAR-5G-XXXX", -72, 36, "WPA3-SAE");
    printf("%-24s | %-5d | %-4d | %s\n", "Hidden_Network", -88, 6, "WPA2-PSK");
    puts("----------------------------------------------------------");
    puts("");

    printf("[+] Scan complete. 5 networks found.\n");
    puts("");

    // Additional system info
    puts("----------------------------------------------------------");
    puts("System Information:");
    puts("----------------------------------------------------------");
    printf("  Platform:     ESP32 (QEMU simulation)\n");
    printf("  Payload:      c2_payload.elf\n");
    printf("  Arguments:    argc=%d\n", argc);
    if (argc > 0 && argv && argv[0]) {
        printf("  argv[0]:      %s\n", argv[0]);
    }
    puts("----------------------------------------------------------");
    puts("");

    printf("Payload execution complete.\n");

    return 0;
}
