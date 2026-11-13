#include "store.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static store_entry_t g_e[STORE_MAX];
static int g_n;
static char g_path[256];

void store_addr_str(const uint8_t addr[6], char *out)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

int store_addr_parse(const char *s, uint8_t addr[6])
{
    unsigned b[6];
    if (!s || sscanf(s, "%2x:%2x:%2x:%2x:%2x:%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return 0;
    for (int i = 0; i < 6; i++) addr[5 - i] = (uint8_t)b[i];
    return 1;
}

static int find(const uint8_t addr[6])
{
    for (int i = 0; i < g_n; i++)
        if (memcmp(g_e[i].addr, addr, 6) == 0) return i;
    return -1;
}

/* Written to a temporary file and renamed, so a crash never leaves half a store */
static void save_locked(void)
{
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        log_line("store: cannot write %s", tmp);
        return;
    }
    for (int i = 0; i < g_n; i++) {
        char a[18], k[33];
        store_addr_str(g_e[i].addr, a);
        if (g_e[i].has_key) {
            for (int j = 0; j < 16; j++) snprintf(k + 2 * j, 3, "%02x", g_e[i].key[j]);
        } else {
            snprintf(k, sizeof(k), "-");
        }
        fprintf(f, "%s %s %ld %s\n", a, k, g_e[i].last_used, g_e[i].name);
    }
    int ok = fclose(f) == 0;
    if (!ok || rename(tmp, g_path) != 0) log_line("store: cannot save %s", g_path);
}

void store_init(const char *path)
{
    pthread_mutex_lock(&g_lock);
    snprintf(g_path, sizeof(g_path), "%s", path);
    g_n = 0;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (g_n < STORE_MAX && fgets(line, sizeof(line), f)) {
            char a[32], k[40];
            long used = 0;
            int off = 0;
            if (sscanf(line, "%31s %39s %ld %n", a, k, &used, &off) < 3) continue;
            store_entry_t *e = &g_e[g_n];
            memset(e, 0, sizeof(*e));
            if (!store_addr_parse(a, e->addr)) continue;
            if (strlen(k) == 32) {
                e->has_key = 1;
                for (int j = 0; j < 16; j++) {
                    unsigned v;
                    if (sscanf(k + 2 * j, "%2x", &v) != 1) e->has_key = 0;
                    e->key[j] = (uint8_t)v;
                }
            }
            e->last_used = used;
            snprintf(e->name, sizeof(e->name), "%s", line + off);
            e->name[strcspn(e->name, "\r\n")] = 0;
            g_n++;
        }
        fclose(f);
    }
    int n = g_n;
    pthread_mutex_unlock(&g_lock);
    log_line("store: %d paired speaker(s)", n);
}

int store_get_key(const uint8_t addr[6], uint8_t key[16])
{
    pthread_mutex_lock(&g_lock);
    int i = find(addr);
    int ok = i >= 0 && g_e[i].has_key;
    if (ok) memcpy(key, g_e[i].key, 16);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

void store_put(const uint8_t addr[6], const char *name, const uint8_t *key)
{
    pthread_mutex_lock(&g_lock);
    int i = find(addr);
    if (i < 0) {
        if (g_n == STORE_MAX) {
            /* Full: the speaker used least recently makes room */
            int old = 0;
            for (int j = 1; j < g_n; j++)
                if (g_e[j].last_used < g_e[old].last_used) old = j;
            g_e[old] = g_e[--g_n];
        }
        i = g_n++;
        memset(&g_e[i], 0, sizeof(g_e[i]));
        memcpy(g_e[i].addr, addr, 6);
    }
    if (name && name[0]) snprintf(g_e[i].name, sizeof(g_e[i].name), "%s", name);
    if (key) {
        memcpy(g_e[i].key, key, 16);
        g_e[i].has_key = 1;
    }
    save_locked();
    pthread_mutex_unlock(&g_lock);
}

void store_drop_key(const uint8_t addr[6])
{
    pthread_mutex_lock(&g_lock);
    int i = find(addr);
    if (i >= 0 && g_e[i].has_key) {
        g_e[i].has_key = 0;
        memset(g_e[i].key, 0, 16);
        save_locked();
    }
    pthread_mutex_unlock(&g_lock);
}

void store_touch(const uint8_t addr[6])
{
    pthread_mutex_lock(&g_lock);
    int i = find(addr);
    if (i >= 0) {
        g_e[i].last_used = (long)time(NULL);
        save_locked();
    }
    pthread_mutex_unlock(&g_lock);
}

int store_forget(const uint8_t addr[6])
{
    pthread_mutex_lock(&g_lock);
    int i = find(addr);
    if (i >= 0) {
        g_e[i] = g_e[--g_n];
        save_locked();
    }
    pthread_mutex_unlock(&g_lock);
    return i >= 0;
}

int store_has(const uint8_t addr[6])
{
    pthread_mutex_lock(&g_lock);
    int i = find(addr);
    pthread_mutex_unlock(&g_lock);
    return i >= 0;
}

static int by_recent(const void *a, const void *b)
{
    long x = ((const store_entry_t *)a)->last_used, y = ((const store_entry_t *)b)->last_used;
    return x < y ? 1 : x > y ? -1 : 0;
}

int store_list(store_entry_t *out, int max)
{
    pthread_mutex_lock(&g_lock);
    int n = g_n < max ? g_n : max;
    store_entry_t all[STORE_MAX];
    memcpy(all, g_e, sizeof(store_entry_t) * (size_t)g_n);
    qsort(all, (size_t)g_n, sizeof(all[0]), by_recent);
    memcpy(out, all, sizeof(store_entry_t) * (size_t)n);
    pthread_mutex_unlock(&g_lock);
    return n;
}

int store_last(store_entry_t *out)
{
    return store_list(out, 1);
}
