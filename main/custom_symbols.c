/*
 * Custom symbol table for ELF loader - Linux Compatibility Layer
 * This file provides symbols exported to guest ELF applications
 */

#include <stddef.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "private/elf_symbol.h"

/*==============================================================================
 * Extern declarations - File I/O shims
 *============================================================================*/
extern void shim_open(void);
extern void shim_read(void);
extern void shim_write(void);
extern void shim_close(void);
extern void shim_lseek(void);
extern void shim_ioctl(void);
extern void shim_fopen(void);
extern void shim_stat(void);
extern void shim_fstat(void);
extern void shim_access(void);
extern void shim_unlink(void);
extern void shim_rename(void);
extern void shim_mkdir(void);
extern void shim_rmdir(void);
extern void shim_opendir(void);
extern void shim_readdir(void);
extern void shim_closedir(void);
extern void shim_getcwd(void);
extern void shim_chdir(void);
extern void shim_printf(void);
extern void shim_puts(void);
extern void shim_dup(void);
extern void shim_dup2(void);
extern void shim_dup3(void);
extern void shim_clock_gettime(void);
extern void shim_nanosleep(void);
extern void shim_getuid(void);
extern void shim_getgid(void);
extern void shim_geteuid(void);
extern void shim_getegid(void);
extern void shim_ftruncate(void);
extern void shim_pipe(void);
extern void shim_gettimeofday(void);

/*==============================================================================
 * Extern declarations - Socket shims
 *============================================================================*/
extern void shim_socket(void);
extern void shim_bind(void);
extern void shim_listen(void);
extern void shim_accept(void);
extern void shim_connect(void);
extern void shim_send(void);
extern void shim_sendto(void);
extern void shim_recv(void);
extern void shim_recvfrom(void);
extern void shim_shutdown(void);
extern void shim_setsockopt(void);
extern void shim_getsockopt(void);
extern void shim_select(void);
extern void shim_poll(void);
extern void shim_gethostbyname(void);
extern void shim_getaddrinfo(void);
extern void shim_freeaddrinfo(void);

/*==============================================================================
 * Extern declarations - Process shims
 *============================================================================*/
extern void shim_fork(void);
extern void shim_execve(void);
extern void shim_waitpid(void);
extern void shim_wait(void);
extern void shim_exit(void);
extern void shim__exit(void);
extern void shim_abort(void);
extern void shim_getpid(void);
extern void shim_getppid(void);
extern void shim_signal(void);
extern void shim_raise(void);
extern void shim_kill(void);

/*==============================================================================
 * Extern declarations - Standard C library
 *============================================================================*/
extern void fprintf(void);
extern void fclose(void);
extern void fread(void);
extern void fwrite(void);
extern void fflush(void);
extern void fseek(void);
extern void ftell(void);
extern void sprintf(void);
extern void snprintf(void);
extern void sscanf(void);
extern void atoi(void);
extern void atof(void);
extern void strtol(void);
extern void strtod(void);

/*==============================================================================
 * Extern declarations - libgcc soft-float helpers
 *============================================================================*/
extern void __floatsidf(void);    // int to double
extern void __extendsfdf2(void);  // float to double
extern void __adddf3(void);       // double addition
extern void __divdf3(void);       // double division
extern void __truncdfsf2(void);   // double to float
extern void __muldf3(void);       // double multiplication
extern void __subdf3(void);       // double subtraction
extern void __fixdfsi(void);      // double to int
extern void __floatunsidf(void);  // unsigned int to double
extern void __gedf2(void);        // double compare >=
extern void __ledf2(void);        // double compare <=
extern void __ltdf2(void);        // double compare <
extern void __gtdf2(void);        // double compare >
extern void __eqdf2(void);        // double compare ==
extern void __nedf2(void);        // double compare !=

/*==============================================================================
 * Extern declarations - FreeRTOS
 *============================================================================*/
extern void vTaskDelay(void);
extern void vTaskDelete(void);
extern void xTaskGetCurrentTaskHandle(void);
extern void xQueueReceive(void);
extern void pvPortMalloc(void);
extern void vPortFree(void);

/*==============================================================================
 * Extern declarations - ESP-IDF
 *============================================================================*/
extern void esp_log_write(void);
extern void esp_log_timestamp(void);

/*==============================================================================
 * Custom symbol table for ELF loader
 *
 * This table maps function names to addresses for dynamic linking.
 * Customer symbols (g_customer_elfsyms) are checked FIRST, allowing us
 * to override libc functions with our shims.
 *============================================================================*/
const struct esp_elfsym g_customer_elfsyms[] = {
    /*--------------------------------------------------------------------------
     * File I/O - Mapped to shims with path translation
     *------------------------------------------------------------------------*/
    { "open", &shim_open },
    { "read", &shim_read },
    { "write", &shim_write },
    { "close", &shim_close },
    { "lseek", &shim_lseek },
    { "ioctl", &shim_ioctl },
    { "fopen", &shim_fopen },
    { "stat", &shim_stat },
    { "fstat", &shim_fstat },
    { "access", &shim_access },
    { "unlink", &shim_unlink },
    { "rename", &shim_rename },
    { "mkdir", &shim_mkdir },
    { "rmdir", &shim_rmdir },
    { "opendir", &shim_opendir },
    { "readdir", &shim_readdir },
    { "closedir", &shim_closedir },
    { "getcwd", &shim_getcwd },
    { "chdir", &shim_chdir },
    { "clock_gettime", &shim_clock_gettime },
    { "nanosleep", &shim_nanosleep },
    { "getuid", &shim_getuid },
    { "getgid", &shim_getgid },
    { "geteuid", &shim_geteuid },
    { "getegid", &shim_getegid },
    { "ftruncate", &shim_ftruncate },
    { "pipe", &shim_pipe },
    { "gettimeofday", &shim_gettimeofday },

    /*--------------------------------------------------------------------------
     * Standard I/O with C2 redirection support
     *------------------------------------------------------------------------*/
    { "printf", &shim_printf },
    { "puts", &shim_puts },
    { "dup", &shim_dup },
    { "dup2", &shim_dup2 },
    { "dup3", &shim_dup3 },

    /*--------------------------------------------------------------------------
     * File streams - Direct libc (no path translation needed)
     *------------------------------------------------------------------------*/
    ESP_ELFSYM_EXPORT(fprintf),
    ESP_ELFSYM_EXPORT(fclose),
    ESP_ELFSYM_EXPORT(fread),
    ESP_ELFSYM_EXPORT(fwrite),
    ESP_ELFSYM_EXPORT(fflush),
    ESP_ELFSYM_EXPORT(fseek),
    ESP_ELFSYM_EXPORT(ftell),
    ESP_ELFSYM_EXPORT(sprintf),
    ESP_ELFSYM_EXPORT(snprintf),
    ESP_ELFSYM_EXPORT(sscanf),

    /*--------------------------------------------------------------------------
     * Socket API - Mapped to shims with sockaddr translation
     *------------------------------------------------------------------------*/
    { "socket", &shim_socket },
    { "bind", &shim_bind },
    { "listen", &shim_listen },
    { "accept", &shim_accept },
    { "connect", &shim_connect },
    { "send", &shim_send },
    { "sendto", &shim_sendto },
    { "recv", &shim_recv },
    { "recvfrom", &shim_recvfrom },
    { "shutdown", &shim_shutdown },
    { "setsockopt", &shim_setsockopt },
    { "getsockopt", &shim_getsockopt },
    { "select", &shim_select },
    { "poll", &shim_poll },
    { "gethostbyname", &shim_gethostbyname },
    { "getaddrinfo", &shim_getaddrinfo },
    { "freeaddrinfo", &shim_freeaddrinfo },

    /*--------------------------------------------------------------------------
     * Process Control - Spawn model emulation
     *------------------------------------------------------------------------*/
    { "fork", &shim_fork },
    { "execve", &shim_execve },
    { "waitpid", &shim_waitpid },
    { "wait", &shim_wait },
    { "exit", &shim_exit },
    { "_exit", &shim__exit },
    { "abort", &shim_abort },
    { "getpid", &shim_getpid },
    { "getppid", &shim_getppid },
    { "signal", &shim_signal },
    { "raise", &shim_raise },
    { "kill", &shim_kill },

    /*--------------------------------------------------------------------------
     * String/number conversion
     *------------------------------------------------------------------------*/
    ESP_ELFSYM_EXPORT(atoi),
    ESP_ELFSYM_EXPORT(atof),
    ESP_ELFSYM_EXPORT(strtol),
    ESP_ELFSYM_EXPORT(strtod),

    /*--------------------------------------------------------------------------
     * String functions
     *------------------------------------------------------------------------*/
    ESP_ELFSYM_EXPORT(memcpy),
    ESP_ELFSYM_EXPORT(memset),
    ESP_ELFSYM_EXPORT(memmove),
    ESP_ELFSYM_EXPORT(memcmp),
    ESP_ELFSYM_EXPORT(strlen),
    ESP_ELFSYM_EXPORT(strcpy),
    ESP_ELFSYM_EXPORT(strncpy),
    ESP_ELFSYM_EXPORT(strcat),
    ESP_ELFSYM_EXPORT(strncat),
    ESP_ELFSYM_EXPORT(strcmp),
    ESP_ELFSYM_EXPORT(strncmp),
    ESP_ELFSYM_EXPORT(strchr),
    ESP_ELFSYM_EXPORT(strrchr),
    ESP_ELFSYM_EXPORT(strstr),

    /*--------------------------------------------------------------------------
     * Memory allocation
     *------------------------------------------------------------------------*/
    ESP_ELFSYM_EXPORT(malloc),
    ESP_ELFSYM_EXPORT(free),
    ESP_ELFSYM_EXPORT(calloc),
    ESP_ELFSYM_EXPORT(realloc),

    /*--------------------------------------------------------------------------
     * Math functions - Single precision (float)
     *------------------------------------------------------------------------*/
    ESP_ELFSYM_EXPORT(sinf),
    ESP_ELFSYM_EXPORT(cosf),
    ESP_ELFSYM_EXPORT(tanf),
    ESP_ELFSYM_EXPORT(asinf),
    ESP_ELFSYM_EXPORT(acosf),
    ESP_ELFSYM_EXPORT(atanf),
    ESP_ELFSYM_EXPORT(atan2f),
    ESP_ELFSYM_EXPORT(sqrtf),
    ESP_ELFSYM_EXPORT(powf),
    ESP_ELFSYM_EXPORT(expf),
    ESP_ELFSYM_EXPORT(logf),
    ESP_ELFSYM_EXPORT(log10f),
    ESP_ELFSYM_EXPORT(fabsf),
    ESP_ELFSYM_EXPORT(floorf),
    ESP_ELFSYM_EXPORT(ceilf),
    ESP_ELFSYM_EXPORT(fmodf),
    ESP_ELFSYM_EXPORT(roundf),

    /*--------------------------------------------------------------------------
     * Math functions - Double precision
     *------------------------------------------------------------------------*/
    ESP_ELFSYM_EXPORT(sin),
    ESP_ELFSYM_EXPORT(cos),
    ESP_ELFSYM_EXPORT(tan),
    ESP_ELFSYM_EXPORT(asin),
    ESP_ELFSYM_EXPORT(acos),
    ESP_ELFSYM_EXPORT(atan),
    ESP_ELFSYM_EXPORT(atan2),
    ESP_ELFSYM_EXPORT(sqrt),
    ESP_ELFSYM_EXPORT(pow),
    ESP_ELFSYM_EXPORT(exp),
    ESP_ELFSYM_EXPORT(log),
    ESP_ELFSYM_EXPORT(log10),
    ESP_ELFSYM_EXPORT(fabs),
    ESP_ELFSYM_EXPORT(floor),
    ESP_ELFSYM_EXPORT(ceil),
    ESP_ELFSYM_EXPORT(fmod),
    ESP_ELFSYM_EXPORT(round),

    /*--------------------------------------------------------------------------
     * libgcc soft-float helpers (needed for double precision operations)
     *------------------------------------------------------------------------*/
    ESP_ELFSYM_EXPORT(__floatsidf),
    ESP_ELFSYM_EXPORT(__extendsfdf2),
    ESP_ELFSYM_EXPORT(__adddf3),
    ESP_ELFSYM_EXPORT(__divdf3),
    ESP_ELFSYM_EXPORT(__truncdfsf2),
    ESP_ELFSYM_EXPORT(__muldf3),
    ESP_ELFSYM_EXPORT(__subdf3),
    ESP_ELFSYM_EXPORT(__fixdfsi),
    ESP_ELFSYM_EXPORT(__floatunsidf),
    ESP_ELFSYM_EXPORT(__gedf2),
    ESP_ELFSYM_EXPORT(__ledf2),
    ESP_ELFSYM_EXPORT(__ltdf2),
    ESP_ELFSYM_EXPORT(__gtdf2),
    ESP_ELFSYM_EXPORT(__eqdf2),
    ESP_ELFSYM_EXPORT(__nedf2),

    /*--------------------------------------------------------------------------
     * FreeRTOS APIs
     *------------------------------------------------------------------------*/
    ESP_ELFSYM_EXPORT(vTaskDelay),
    ESP_ELFSYM_EXPORT(vTaskDelete),
    ESP_ELFSYM_EXPORT(xTaskGetCurrentTaskHandle),
    ESP_ELFSYM_EXPORT(xQueueReceive),
    ESP_ELFSYM_EXPORT(pvPortMalloc),
    ESP_ELFSYM_EXPORT(vPortFree),

    /*--------------------------------------------------------------------------
     * ESP-IDF Logging
     *------------------------------------------------------------------------*/
    ESP_ELFSYM_EXPORT(esp_log_write),
    ESP_ELFSYM_EXPORT(esp_log_timestamp),

    ESP_ELFSYM_END
};
