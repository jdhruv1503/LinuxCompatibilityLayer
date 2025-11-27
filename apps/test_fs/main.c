#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

__attribute__((visibility("default")))
int app_main(int argc, char *argv[]) {
    printf("Guest: Attempting to write to filesystem...\n");

    // 1. Use high-level fopen (which calls shim_open)
    FILE *f = fopen("/guest_log.txt", "w");
    if (!f) {
        printf("Guest: fopen failed!\n");
        return 1;
    }
    fprintf(f, "Hello from the Guest ELF via Shim!\n");
    fclose(f);

    // 2. Verification via low-level open
    int fd = open("/guest_log.txt", O_RDONLY);
    if (fd < 0) {
        printf("Guest: open failed!\n");
        return 1;
    }
    
    char buffer[64];
    int len = read(fd, buffer, sizeof(buffer)-1);
    buffer[len] = '\0';
    close(fd);

    printf("Guest: Read back -> '%s'\n", buffer);
    return 0;
}

