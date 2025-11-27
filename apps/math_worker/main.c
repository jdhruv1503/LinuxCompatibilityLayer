/**
 * @file main.c
 * @brief Math Worker - Distributed Computation with Math Functions
 *
 * Map-Reduce worker that demonstrates math library integration.
 * Computes: sum of sin^2(x) + cos^2(x) for all input values
 * (Should equal COUNT since sin^2 + cos^2 = 1 for all x)
 *
 * Also computes geometric mean using logarithms and exponentials.
 *
 * Protocol:
 * - Input: Numbers separated by newlines (via stdin fd=0)
 * - Output: "RESULT: TRIG_SUM=<sum> GEO_MEAN=<mean> COUNT=<n>"
 */

/*==============================================================================
 * Manual Definitions
 *============================================================================*/

typedef int ssize_t;
typedef unsigned int size_t;

#define STDIN_FILENO 0

/*==============================================================================
 * External Symbols
 *============================================================================*/

extern int printf(const char *fmt, ...);
extern int puts(const char *s);
extern ssize_t read(int fd, void *buf, size_t count);

// Math functions
extern float sinf(float x);
extern float cosf(float x);
extern float sqrtf(float x);
extern float logf(float x);
extern float expf(float x);
extern float fabsf(float x);
extern float powf(float x, float y);

/*==============================================================================
 * Helper Functions
 *============================================================================*/

// Simple string to long conversion
static long simple_atol(const char *str) {
    long result = 0;
    int sign = 1;

    while (*str == ' ' || *str == '\t' || *str == '\n' || *str == '\r') str++;

    if (*str == '-') {
        sign = -1;
        str++;
    } else if (*str == '+') {
        str++;
    }

    while (*str >= '0' && *str <= '9') {
        result = result * 10 + (*str - '0');
        str++;
    }

    return result * sign;
}

// Read a line from fd into buffer
static int read_line(int fd, char *buf, int max_len) {
    int i = 0;
    while (i < max_len - 1) {
        char c;
        ssize_t ret = read(fd, &c, 1);
        if (ret <= 0) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/*==============================================================================
 * Constants
 *============================================================================*/

#define M_PI 3.14159265358979323846f

/*==============================================================================
 * Main Entry Point
 *============================================================================*/

__attribute__((visibility("default")))
int app_main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    puts("MATH_WORKER_STARTED");

    // Statistics (use floats for ESP32 HW FPU performance)
    long count = 0;
    float trig_sum = 0.0f;      // Sum of sin^2(x) + cos^2(x)
    float log_sum = 0.0f;       // Sum of log(|x|) for geometric mean
    long linear_sum = 0;        // Regular sum for comparison
    float rms_sum = 0.0f;       // Sum of squares for RMS

    char buffer[64];
    int len;

    // Read values from stdin
    while ((len = read_line(STDIN_FILENO, buffer, sizeof(buffer))) > 0 || len == 0) {
        if (len == 0) {
            char c;
            ssize_t ret = read(STDIN_FILENO, &c, 1);
            if (ret <= 0) break;
            buffer[0] = c;
            len = 1 + read_line(STDIN_FILENO, buffer + 1, sizeof(buffer) - 1);
        }

        if (len == 0) continue;

        // Parse number
        long val = simple_atol(buffer);
        float fval = (float)val;

        // Regular sum
        linear_sum += val;
        count++;

        // Trigonometric identity: sin^2(x) + cos^2(x) = 1
        // Convert value to radians for interesting trig values
        float rad = fval * (M_PI / 180.0f);
        float s = sinf(rad);
        float c_val = cosf(rad);
        trig_sum += (s * s + c_val * c_val);  // Should accumulate to COUNT

        // For geometric mean: sum of logs
        if (fabsf(fval) > 0.001f) {
            log_sum += logf(fabsf(fval));
        }

        // RMS calculation (use float for HW FPU)
        rms_sum += fval * fval;
    }

    // Calculate results (all float operations for HW FPU)
    float trig_avg = (count > 0) ? (trig_sum / (float)count) : 0.0f;

    // Geometric mean = exp(mean of logs)
    float geo_mean = 0.0f;
    if (count > 0) {
        geo_mean = expf(log_sum / (float)count);
    }

    // RMS = sqrt(mean of squares)
    float rms = 0.0f;
    if (count > 0) {
        rms = sqrtf(rms_sum / (float)count);
    }

    // Output results
    // TRIG_AVG should be ~1.0 (verifies sin^2 + cos^2 = 1)
    // LINEAR_SUM is the regular sum
    // GEO_MEAN is the geometric mean of absolute values
    // RMS is the root mean square
    printf("RESULT: LINEAR_SUM=%ld TRIG_AVG=%.4f GEO_MEAN=%.2f RMS=%.2f COUNT=%ld\n",
           linear_sum, trig_avg, geo_mean, rms, count);

    return 0;
}
