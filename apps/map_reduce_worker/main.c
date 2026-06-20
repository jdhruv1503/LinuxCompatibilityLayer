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
 */

typedef int ssize_t;
typedef unsigned int size_t;

extern int printf(const char *fmt, ...);
extern int puts(const char *s);
extern ssize_t read(int fd, void *buf, size_t count);
extern void *memmove(void *dest, const void *src, size_t n);

#define STDIN_FILENO 0
#define BUFFER_SIZE  1024

// Simple atoi implementation
static long simple_atol(const char *str, int len) {
    long result = 0;
    int sign = 1;
    int i = 0;

    // Skip whitespace
    while (i < len && (str[i] == ' ' || str[i] == '\t' || str[i] == '\n' || str[i] == '\r')) i++;

    if (i == len) return 0;

    // Handle sign
    if (str[i] == '-') {
        sign = -1;
        i++;
    } else if (str[i] == '+') {
        i++;
    }

    // Convert digits
    while (i < len && str[i] >= '0' && str[i] <= '9') {
        result = result * 10 + (str[i] - '0');
        i++;
    }

    return result * sign;
}

__attribute__((visibility("default")))
int app_main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    puts("WORKER_STARTED");

    long long sum = 0;
    long count = 0;
    
    static char buffer[BUFFER_SIZE];
    int buf_len = 0;
    int buf_offset = 0;

    while (1) {
        // If buffer is empty or processed, read more
        if (buf_offset >= buf_len) {
            buf_offset = 0;
            ssize_t n = read(STDIN_FILENO, buffer, BUFFER_SIZE);
            if (n <= 0) break; // EOF or error
            buf_len = n;
        }

        // Find newline
        int line_end = -1;
        for (int i = buf_offset; i < buf_len; i++) {
            if (buffer[i] == '\n') {
                line_end = i;
                break;
            }
        }

        if (line_end != -1) {
            // Found a complete line
            long val = simple_atol(buffer + buf_offset, line_end - buf_offset);
            // Only count if it looks like a number (simple heuristic: length > 0)
            if (line_end > buf_offset) {
                sum += val;
                count++;
            }
            buf_offset = line_end + 1;
        } else {
            // No newline found in current buffer.
            // Move remaining data to beginning
            int remaining = buf_len - buf_offset;
            if (remaining > 0) {
                memmove(buffer, buffer + buf_offset, remaining);
            }
            
            // Read more into the rest of the buffer
            buf_offset = 0;
            ssize_t n = read(STDIN_FILENO, buffer + remaining, BUFFER_SIZE - remaining);
            if (n <= 0) {
                // EOF with partial line? Process it
                if (remaining > 0) {
                     long val = simple_atol(buffer, remaining);
                     sum += val;
                     count++;
                }
                break;
            }
            buf_len = remaining + n;
        }
    }

    printf("RESULT: SUM=%lld COUNT=%ld\n", sum, count);
    return 0;
}
