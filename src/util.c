#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#define _BSD_SOURCE

#include "util.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <ctype.h>


long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

uint64_t uptime_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

void sleep_ms(int ms)
{
    if (ms <= 0) return;
    usleep((useconds_t)ms * 1000);
}

uint32_t crc32_le(uint32_t crc, const uint8_t *buf, size_t len)
{
    crc = ~crc;
    while (len--) {
        crc ^= *buf++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
    return ~crc;
}

uint64_t hex_to_u64(const char *s)
{
    if (!s) return 0;
    while (*s == ' ' || *s == ':' || *s == '=') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint64_t val = 0;
    while (isxdigit((unsigned char)*s)) {
        char c = *s++;
        val = (val << 4) | (uint64_t)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
    }
    return val;
}

int hex_dump(const void *data, size_t len, char *out, size_t out_len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t written = 0;
    for (size_t i = 0; i < len && written + 3 < out_len; i++) {
        written += snprintf(out + written, out_len - written, "%02x ", p[i]);
    }
    if (written < out_len) out[written] = '\0';
    return (int)written;
}

#define THREAD_STACK (512 * 1024)

typedef struct {
    void *(*fn)(void *);
    void *arg;
    const char *name;
} thread_boot_t;

/* Names the thread for the log and crash reports, then runs it */
static void *thread_boot(void *p)
{
    thread_boot_t b = *(thread_boot_t *)p;
    free(p);
    log_thread_name(b.name);
    return b.fn(b.arg);
}

int thread_start(pthread_t *t, void *(*fn)(void *), void *arg, const char *name)
{
    thread_boot_t *b = malloc(sizeof(*b));
    if (!b) return 0;
    b->fn = fn;
    b->arg = arg;
    b->name = name;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    int set = pthread_attr_setstacksize(&attr, THREAD_STACK);
    int err = pthread_create(t, &attr, thread_boot, b);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        free(b);
        log_line("thread: %s not started (error %d)", name, err);
        return 0;
    }
    log_line("thread: %s started (stack %s)", name, set == 0 ? "512 KB" : "default");
    return 1;
}
