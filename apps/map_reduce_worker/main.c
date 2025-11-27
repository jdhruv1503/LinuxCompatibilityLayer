/**
 * @file main.c
 * @brief Map-Reduce Worker Payload
 *
 * Reads integer data from stdin (streamed from C2 master via redirected socket),
 * calculates sum and count, and prints the result.
 *
 * Protocol:
 * - Input: Numbers separated by newlines (via stdin fd=0 from socket)
 * - Output: "RESULT: SUM=<sum> COUNT=<count>" (via stdout to socket)
 *
 * Note: Uses raw read() on fd=0 instead of fgets() to avoid FILE* complications.
 * The c2_bot redirects stdin to the socket via dup2(), and our shim_read()
 * intercepts reads on fd=0 when stdin is redirected.
 */

// Manual POSIX definitions
typedef int ssize_t;
typedef unsigned int size_t;

extern int printf(const char *fmt, ...);
extern int puts(const char *s);
extern ssize_t read(int fd, void *buf, size_t count);

#define STDIN_FILENO 0

// Simple atoi implementation
static long simple_atol(const char *str) {
    long result = 0;
    int sign = 1;

    // Skip whitespace
    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;

    // Handle sign
    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    // Convert digits
    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }

    return result * sign;
}

// Read a line from fd into buffer (returns bytes read, 0 on EOF)
static int read_line(int fd, char *buf, int max_len) {
    int i = 0;
    while (i < max_len - 1) {
        char c;
        ssize_t ret = read(fd, &c, 1);
        if (ret <= 0) {
            // EOF or error
            break;
        }
        if (c == '\n') {
            break;  // End of line (don't include newline)
        }
        if (c == '\r') {
            continue;  // Skip carriage returns
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

__attribute__((visibility("default")))
int app_main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    puts("WORKER_STARTED");

    long long sum = 0;
    long count = 0;
    char buffer[64];

    // Read lines from stdin (fd=0, redirected to C2 socket by c2_bot)
    // The master sends numbers separated by newlines, then closes the write end
    int len;
    while ((len = read_line(STDIN_FILENO, buffer, sizeof(buffer))) > 0 || len == 0) {
        // Handle the case where we got an empty line but not EOF
        if (len == 0) {
            // Check for actual EOF by trying to read one more byte
            char c;
            ssize_t ret = read(STDIN_FILENO, &c, 1);
            if (ret <= 0) {
                break;  // EOF
            }
            // Got a character, it must be part of next line
            buffer[0] = c;
            len = 1 + read_line(STDIN_FILENO, buffer + 1, sizeof(buffer) - 1);
        }

        // Skip empty lines
        if (len == 0) continue;

        // Parse number and accumulate
        long val = simple_atol(buffer);
        sum += val;
        count++;
    }

    printf("RESULT: SUM=%lld COUNT=%ld\n", sum, count);
    return 0;
}
