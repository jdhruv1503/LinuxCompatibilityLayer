/**
 * @file shim_process.c
 * @brief Process control shim for Linux compatibility
 *
 * Implements the "spawn model" for process creation on ESP32
 * (which lacks an MMU for true fork/exec).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"

static const char *TAG = "shim_process";

// Forward declaration - implemented in elf_loader.c
// We need to declare the function prototype here. In the task doc, it says it calls `elf_loader_run`.
// Checking main.c: `load_and_run_elf` is static in main.c or similar? 
// Wait, main.c has `load_and_run_elf`. It's not exported.
// The guide says `extern int elf_loader_run(const char *path, int argc, char *argv[]);`
// I will need to expose `load_and_run_elf` from main.c, possibly renaming it or adding a wrapper.
// In `main.c`, `load_and_run_elf` is currently static? No, it's just `int load_and_run_elf(...)`.
// So I can extern it. I'll use `load_and_run_elf` as the symbol name since that's what is in main.c.

extern int load_and_run_elf(const char *path, int argc, char *argv[]);

/*==============================================================================
 * Process ID Emulation
 *============================================================================*/

// Map FreeRTOS task handle to "PID"
pid_t shim_getpid(void) {
    // Use task handle as pseudo-PID
    TaskHandle_t handle = xTaskGetCurrentTaskHandle();
    return (pid_t)(uintptr_t)handle;
}

pid_t shim_getppid(void) {
    // No real parent-child relationship - return 1 (init)
    return 1;
}

/*==============================================================================
 * Fork (NOT SUPPORTED)
 *============================================================================*/

pid_t shim_fork(void) {
    // ESP32 has no MMU - cannot implement fork()
    ESP_LOGW(TAG, "fork() called - NOT SUPPORTED on ESP32");
    errno = ENOSYS;
    return -1;
}

pid_t shim_vfork(void) {
    // vfork also not supported
    ESP_LOGW(TAG, "vfork() called - NOT SUPPORTED on ESP32");
    errno = ENOSYS;
    return -1;
}

/*==============================================================================
 * Process Tracking for waitpid
 *============================================================================*/

// Structure to track a child process
typedef struct child_process {
    pid_t pid;                      // Process ID (task handle)
    TaskHandle_t task_handle;       // FreeRTOS task handle
    SemaphoreHandle_t completion_sem; // Signaled when process completes
    int exit_status;                // Exit status (or -1 if not set)
    bool completed;                  // Whether process has completed
    struct child_process *next;     // Linked list
} child_process_t;

// Global list of child processes (simple linked list)
static child_process_t *s_child_processes = NULL;
static SemaphoreHandle_t s_child_list_mutex = NULL;

// Initialize child process tracking
static void init_child_tracking(void) {
    if (s_child_list_mutex == NULL) {
        s_child_list_mutex = xSemaphoreCreateMutex();
    }
}

// Find a child process by PID
static child_process_t *find_child_process(pid_t pid) {
    if (s_child_list_mutex == NULL) {
        return NULL;
    }

    if (xSemaphoreTake(s_child_list_mutex, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }

    child_process_t *child = s_child_processes;
    while (child != NULL) {
        if (child->pid == pid) {
            xSemaphoreGive(s_child_list_mutex);
            return child;
        }
        child = child->next;
    }

    xSemaphoreGive(s_child_list_mutex);
    return NULL;
}

// Add a child process to the tracking list
static child_process_t *add_child_process(pid_t pid, TaskHandle_t task_handle) {
    init_child_tracking();

    if (xSemaphoreTake(s_child_list_mutex, portMAX_DELAY) != pdTRUE) {
        return NULL;
    }

    child_process_t *child = malloc(sizeof(child_process_t));
    if (!child) {
        xSemaphoreGive(s_child_list_mutex);
        return NULL;
    }

    child->pid = pid;
    child->task_handle = task_handle;
    child->completion_sem = xSemaphoreCreateBinary();
    child->exit_status = -1;
    child->completed = false;
    child->next = s_child_processes;
    s_child_processes = child;

    xSemaphoreGive(s_child_list_mutex);
    return child;
}

// Remove a child process from the tracking list
static void remove_child_process(pid_t pid) {
    if (s_child_list_mutex == NULL) {
        return;
    }

    if (xSemaphoreTake(s_child_list_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }

    child_process_t **prev = &s_child_processes;
    child_process_t *child = s_child_processes;

    while (child != NULL) {
        if (child->pid == pid) {
            *prev = child->next;
            if (child->completion_sem) {
                vSemaphoreDelete(child->completion_sem);
            }
            free(child);
            break;
        }
        prev = &child->next;
        child = child->next;
    }

    xSemaphoreGive(s_child_list_mutex);
}

// Mark a child process as completed
static void mark_child_completed(pid_t pid, int exit_status) {
    child_process_t *child = find_child_process(pid);
    if (child && child->completion_sem) {
        child->exit_status = exit_status;
        child->completed = true;
        xSemaphoreGive(child->completion_sem);
    }
}

/*==============================================================================
 * Exec Family (Spawn Model)
 *============================================================================*/

// Structure to pass parameters to the spawned task
typedef struct {
    char *path;
    char **argv;
    int argc;
    SemaphoreHandle_t started_sem;
    pid_t child_pid;  // PID of the child process
} exec_params_t;

static void exec_task_wrapper(void *pvParameters) {
    exec_params_t *params = (exec_params_t *)pvParameters;

    // Get our task handle and create PID
    TaskHandle_t task_handle = xTaskGetCurrentTaskHandle();
    pid_t child_pid = (pid_t)(uintptr_t)task_handle;

    // Add ourselves to child tracking (parent already did this, but ensure it exists)
    child_process_t *child = find_child_process(child_pid);
    if (!child) {
        child = add_child_process(child_pid, task_handle);
    }

    // Signal that we've started and copied parameters
    char *path = params->path;
    char **argv = params->argv;
    int argc = params->argc;

    xSemaphoreGive(params->started_sem);

    ESP_LOGI(TAG, "Executing: %s (PID: %d)", path, (int)child_pid);

    // Run the ELF
    int ret = load_and_run_elf(path, argc, argv);

    ESP_LOGI(TAG, "ELF returned: %d", ret);

    // Mark this child process as completed before cleanup
    mark_child_completed(child_pid, ret);

    // Free argument copies
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
    free(path);
    free(params);

    // Delete this task
    vTaskDelete(NULL);
}

int shim_execve(const char *path, char *const argv[], char *const envp[]) {
    ESP_LOGI(TAG, "execve('%s')", path);

    // Count arguments
    int argc = 0;
    if (argv) {
        while (argv[argc] != NULL) {
            argc++;
        }
    }

    // Deep copy path and arguments (parent might free them)
    char *path_copy = strdup(path);
    if (!path_copy) {
        errno = ENOMEM;
        return -1;
    }

    char **argv_copy = malloc((argc + 1) * sizeof(char *));
    if (!argv_copy) {
        free(path_copy);
        errno = ENOMEM;
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        argv_copy[i] = strdup(argv[i]);
        if (!argv_copy[i]) {
            for (int j = 0; j < i; j++) free(argv_copy[j]);
            free(argv_copy);
            free(path_copy);
            errno = ENOMEM;
            return -1;
        }
    }
    argv_copy[argc] = NULL;

    // Create synchronization semaphore
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    if (!sem) {
        for (int i = 0; i < argc; i++) free(argv_copy[i]);
        free(argv_copy);
        free(path_copy);
        errno = ENOMEM;
        return -1;
    }

    // Allocate params on heap (task will free it)
    exec_params_t *params = malloc(sizeof(exec_params_t));
    if (!params) {
        vSemaphoreDelete(sem);
        for (int i = 0; i < argc; i++) free(argv_copy[i]);
        free(argv_copy);
        free(path_copy);
        errno = ENOMEM;
        return -1;
    }

    params->path = path_copy;
    params->argv = argv_copy;
    params->argc = argc;
    params->started_sem = sem;
    params->child_pid = 0;  // Will be set by task itself

    // Create the child task
    TaskHandle_t task_handle = NULL;
    BaseType_t ret = xTaskCreate(
        exec_task_wrapper,
        "guest_elf",
        8192,  // Stack size (adjust as needed)
        params,
        5,     // Priority
        &task_handle
    );

    if (ret != pdPASS || task_handle == NULL) {
        free(params);
        vSemaphoreDelete(sem);
        for (int i = 0; i < argc; i++) free(argv_copy[i]);
        free(argv_copy);
        free(path_copy);
        errno = ENOMEM;
        return -1;
    }

    // Create PID from task handle and add to tracking
    pid_t child_pid = (pid_t)(uintptr_t)task_handle;
    child_process_t *child = add_child_process(child_pid, task_handle);
    if (!child) {
        vTaskDelete(task_handle);
        free(params);
        vSemaphoreDelete(sem);
        for (int i = 0; i < argc; i++) free(argv_copy[i]);
        free(argv_copy);
        free(path_copy);
        errno = ENOMEM;
        return -1;
    }

    // Wait for child to start and copy parameters
    xSemaphoreTake(sem, portMAX_DELAY);
    vSemaphoreDelete(sem);

    // In true exec, we would never return. In spawn model, return success.
    // Note: We return 0, but the actual PID is available via waitpid
    return 0;
}

// Simplified versions
int shim_execv(const char *path, char *const argv[]) {
    return shim_execve(path, argv, NULL);
}

int shim_execvp(const char *file, char *const argv[]) {
    // Simple implementation - just try the path directly
    return shim_execve(file, argv, NULL);
}

/*==============================================================================
 * Process Termination
 *============================================================================*/

void shim_exit(int status) {
    ESP_LOGI(TAG, "exit(%d) called", status);

    // TODO: Cleanup any resources allocated by this "process"
    // - Close open file descriptors
    // - Free heap allocations
    // - Cancel pending timers

    // Delete the current FreeRTOS task
    vTaskDelete(NULL);

    // Should never reach here
    while (1) {
        vTaskDelay(portMAX_DELAY);
    }
}

void shim__exit(int status) {
    // Same as exit for our purposes
    shim_exit(status);
}

void shim_abort(void) {
    ESP_LOGE(TAG, "abort() called!");
    shim_exit(-1);
}

/*==============================================================================
 * Signal Handling (Stubbed)
 *============================================================================*/

// Minimal signal support using FreeRTOS task notifications
typedef void (*sighandler_t)(int);
static sighandler_t s_signal_handlers[32] = {0};

sighandler_t shim_signal(int signum, sighandler_t handler) {
    if (signum < 0 || signum >= 32) {
        errno = EINVAL;
        return SIG_ERR;
    }

    sighandler_t old = s_signal_handlers[signum];
    s_signal_handlers[signum] = handler;
    return old;
}

int shim_raise(int sig) {
    if (sig < 0 || sig >= 32) {
        errno = EINVAL;
        return -1;
    }

    sighandler_t handler = s_signal_handlers[sig];
    if (handler && handler != SIG_IGN && handler != SIG_DFL) {
        handler(sig);
    } else if (handler == SIG_DFL) {
        // Default action for most signals is terminate
        if (sig == SIGKILL || sig == SIGTERM || sig == SIGABRT) {
            shim_exit(128 + sig);
        }
    }

    return 0;
}

int shim_kill(pid_t pid, int sig) {
    // Can only signal the current task in our model
    if ((pid_t)(uintptr_t)xTaskGetCurrentTaskHandle() == pid) {
        return shim_raise(sig);
    }

    // TODO: Could use task notifications to signal other tasks
    errno = ESRCH;
    return -1;
}

/*==============================================================================
 * Wait for Child Process (waitpid)
 *============================================================================*/

// Forward declaration
pid_t shim_wait(int *status);

pid_t shim_waitpid(pid_t pid, int *status, int options) {
    ESP_LOGD(TAG, "waitpid(pid=%d, options=0x%x)", (int)pid, options);

    // Handle wait for any child (pid == -1)
    if (pid == -1) {
        return shim_wait(status);
    }

    // Find the child process
    child_process_t *child = find_child_process(pid);
    
    if (!child) {
        // No such child process
        errno = ECHILD;
        return -1;
    }

    // If process already completed, return immediately
    if (child->completed) {
        if (status) {
            *status = child->exit_status;
        }
        pid_t ret_pid = child->pid;
        remove_child_process(pid);
        return ret_pid;
    }

    // Wait for completion semaphore
    if (child->completion_sem) {
        if (xSemaphoreTake(child->completion_sem, portMAX_DELAY) == pdTRUE) {
            if (status) {
                *status = child->exit_status;
            }
            pid_t ret_pid = child->pid;
            remove_child_process(pid);
            return ret_pid;
        }
    }

    // Should not reach here
    errno = ECHILD;
    return -1;
}

// Simplified wait() - waits for any child
__attribute__((used))
pid_t shim_wait(int *status) {
    // Find any child process
    if (s_child_list_mutex == NULL) {
        errno = ECHILD;
        return -1;
    }

    if (xSemaphoreTake(s_child_list_mutex, portMAX_DELAY) != pdTRUE) {
        errno = ECHILD;
        return -1;
    }

    child_process_t *child = s_child_processes;
    pid_t found_pid = -1;

    // Find first child (or any completed child)
    while (child != NULL) {
        if (child->completed) {
            found_pid = child->pid;
            break;
        }
        child = child->next;
    }

    // If no completed child, use first one
    if (found_pid == -1 && s_child_processes != NULL) {
        found_pid = s_child_processes->pid;
    }

    xSemaphoreGive(s_child_list_mutex);

    if (found_pid == -1) {
        errno = ECHILD;
        return -1;
    }

    return shim_waitpid(found_pid, status, 0);
}
