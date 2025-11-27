/**
 * @file main.c
 * @brief Minimal Hello World guest ELF for ESP32
 */

/* Mark entry point as visible to ensure it's exported */
__attribute__((visibility("default")))
int app_main(int argc, char *argv[])
{
    return 42;
}
