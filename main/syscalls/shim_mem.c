/**
 * Per-guest memory accounting for LinuxCompatibilityLayer.
 * Tracks heap usage per FreeRTOS task (guest) for lcl ps output.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define MAX_GUESTS 8

typedef struct {
    TaskHandle_t task;
    size_t heap_used;
    size_t heap_peak;
    const char *name;
} guest_heap_ctx_t;

static const char *TAG = "shim_mem";
static guest_heap_ctx_t g_guests[MAX_GUESTS];
static int g_guest_count = 0;

static guest_heap_ctx_t *find_guest(TaskHandle_t task) {
    for (int i = 0; i < g_guest_count; i++) {
        if (g_guests[i].task == task) {
            return &g_guests[i];
        }
    }
    return NULL;
}

void lcl_heap_register(const char *name) {
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    guest_heap_ctx_t *ctx = find_guest(task);
    if (ctx) {
        ctx->name = name;
        ESP_LOGD(TAG, "updated guest task=%p name=%s", (void *)task, name ? name : "(null)");
        return;
    }

    if (g_guest_count >= MAX_GUESTS) {
        ESP_LOGW(TAG, "guest table full, cannot register task=%p", (void *)task);
        return;
    }

    g_guests[g_guest_count].task = task;
    g_guests[g_guest_count].heap_used = 0;
    g_guests[g_guest_count].heap_peak = 0;
    g_guests[g_guest_count].name = name;
    g_guest_count++;

    ESP_LOGI(TAG, "registered guest task=%p name=%s", (void *)task, name ? name : "(unnamed)");
}

void lcl_heap_track_alloc(size_t size) {
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    guest_heap_ctx_t *ctx = find_guest(task);
    if (!ctx) {
        lcl_heap_register(pcTaskGetName(task));
        ctx = find_guest(task);
        if (!ctx) {
            return;
        }
    }

    ctx->heap_used += size;
    if (ctx->heap_used > ctx->heap_peak) {
        ctx->heap_peak = ctx->heap_used;
    }

    ESP_LOGD(TAG, "alloc task=%p size=%u used=%u peak=%u",
             (void *)task,
             (unsigned)size,
             (unsigned)ctx->heap_used,
             (unsigned)ctx->heap_peak);
}

void lcl_heap_track_free(size_t size) {
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    guest_heap_ctx_t *ctx = find_guest(task);
    if (!ctx) {
        return;
    }

    if (size > ctx->heap_used) {
        ctx->heap_used = 0;
    } else {
        ctx->heap_used -= size;
    }

    ESP_LOGD(TAG, "free task=%p size=%u used=%u peak=%u",
             (void *)task,
             (unsigned)size,
             (unsigned)ctx->heap_used,
             (unsigned)ctx->heap_peak);
}

int lcl_heap_snprint_all(char *buf, size_t buflen) {
    if (!buf || buflen == 0) {
        return 0;
    }

    int written = snprintf(buf, buflen, "PID/TASK          NAME           HEAP_USED  HEAP_PEAK\n");
    if (written < 0 || (size_t)written >= buflen) {
        return (int)buflen - 1;
    }

    size_t off = (size_t)written;
    for (int i = 0; i < g_guest_count; i++) {
        int n = snprintf(buf + off, buflen - off, "0x%08x  %-14s %-9u %-9u\n",
                         (unsigned)(uintptr_t)g_guests[i].task,
                         g_guests[i].name ? g_guests[i].name : "(unnamed)",
                         (unsigned)g_guests[i].heap_used,
                         (unsigned)g_guests[i].heap_peak);
        if (n <= 0) {
            break;
        }
        if ((size_t)n >= buflen - off) {
            off = buflen - 1;
            break;
        }
        off += (size_t)n;
    }

    return (int)off;
}

void lcl_heap_print_all(void) {
    char out[512];
    lcl_heap_snprint_all(out, sizeof(out));
    ESP_LOGI(TAG, "\n%s", out);
}
