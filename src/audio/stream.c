#include "stream.h"
#include "capture.h"
#include "../a2dp/media.h"
#include "log.h"
#include "util.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define RING_FRAMES     24000       /* 0.5 s at 48 kHz */
#define PREBUFFER       1920        /* 40 ms gathered before packing starts, and after a dry ring */
#define RING_HIGH       12000       /* 250 ms: the capture ran ahead, drop back ... */
#define RING_KEEP       2880        /* ... to 60 ms */
#define LATE_RESYNC_US  200000      /* the worker stalled this long: restart the clock */
#define MAX_PER_TICK    4
#define BITPOOL_FLOOR   29
#define CAPTURE_IDLE_MS 30000
#define CAPTURE_RETRY_MS 5000

static struct {
    pthread_mutex_t lock;
    int16_t  ring[RING_FRAMES * 2];
    int      head, count;
    int      priming;
    unsigned underruns, skips;

    int      volume;            /* 0..100 */
    int      gain_q12;
    long     tone_until;
    double   tone_phase;

    int      active;
    media_t  m;
    long     next_us;
    unsigned sent, dropped, late;
    long     window_at;
    unsigned window_drops;
    long     clean_since;

    pthread_t thread;
    int      thread_on;
    volatile int want;
    long     unwanted_since;
    int      capturing;
} g_s = { .lock = PTHREAD_MUTEX_INITIALIZER, .volume = 100, .gain_q12 = 4096, .priming = 1 };

static long now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000L + tv.tv_usec;
}

void stream_feed(const int16_t *pcm, int frames)
{
    pthread_mutex_lock(&g_s.lock);
    for (int i = 0; i < frames; i++) {
        if (g_s.count == RING_FRAMES) {
            g_s.head = (g_s.head + 1) % RING_FRAMES;
            g_s.count--;
        }
        int at = (g_s.head + g_s.count) % RING_FRAMES;
        g_s.ring[2 * at] = pcm[2 * i];
        g_s.ring[2 * at + 1] = pcm[2 * i + 1];
        g_s.count++;
    }
    if (g_s.count > RING_HIGH) {
        int drop = g_s.count - RING_KEEP;
        g_s.head = (g_s.head + drop) % RING_FRAMES;
        g_s.count -= drop;
        g_s.skips++;
    }
    pthread_mutex_unlock(&g_s.lock);
}

static void tone(int16_t *pcm, int frames)
{
    for (int i = 0; i < frames; i++) {
        int16_t v = (int16_t)(8000.0 * sin(g_s.tone_phase));
        g_s.tone_phase += 2.0 * 3.14159265358979 * 440.0 / 48000.0;
        if (g_s.tone_phase > 6.28318530717959) g_s.tone_phase -= 6.28318530717959;
        pcm[2 * i] = pcm[2 * i + 1] = v;
    }
}

/* media_pull_fn: always fills `frames`, with silence while the ring gathers its prebuffer */
static void pull(void *ud, int16_t *pcm, int frames)
{
    (void)ud;
    pthread_mutex_lock(&g_s.lock);
    int got = 0;
    if (g_s.priming && g_s.count >= PREBUFFER) g_s.priming = 0;
    if (!g_s.priming) {
        got = frames < g_s.count ? frames : g_s.count;
        for (int i = 0; i < got; i++) {
            int at = (g_s.head + i) % RING_FRAMES;
            pcm[2 * i] = g_s.ring[2 * at];
            pcm[2 * i + 1] = g_s.ring[2 * at + 1];
        }
        g_s.head = (g_s.head + got) % RING_FRAMES;
        g_s.count -= got;
        if (got < frames) {
            g_s.underruns++;
            g_s.priming = 1;
        }
    }
    int gain = g_s.gain_q12;
    int tone_on = now_ms() < g_s.tone_until;
    pthread_mutex_unlock(&g_s.lock);

    memset(pcm + 2 * got, 0, (size_t)(frames - got) * 4);
    if (tone_on) tone(pcm, frames);
    if (gain != 4096) {
        for (int i = 0; i < 2 * frames; i++) pcm[i] = (int16_t)((pcm[i] * gain) >> 12);
    }
}

void stream_init(void)
{
    g_s.window_at = now_ms();
}

void stream_start(const sbc_choice_t *cfg, int mtu)
{
    if (!media_setup(&g_s.m, cfg, mtu)) {
        log_line("stream: the encoder refused the configuration");
        return;
    }
    g_s.active = 1;
    g_s.sent = g_s.dropped = g_s.late = 0;
    g_s.next_us = now_us();
    g_s.window_at = g_s.clean_since = now_ms();
    g_s.window_drops = 0;
    pthread_mutex_lock(&g_s.lock);
    g_s.priming = 1;
    g_s.underruns = g_s.skips = 0;
    pthread_mutex_unlock(&g_s.lock);
    log_line("stream: %d Hz, %d frames of %d bytes per packet (MTU %d), %ld us per packet, bitpool %d",
             g_s.m.rate, g_s.m.per_pkt, g_s.m.frame_len, mtu, media_packet_us(&g_s.m), g_s.m.bitpool);
}

void stream_stop(void)
{
    if (!g_s.active) return;
    g_s.active = 0;
    log_line("stream: stopped after %u packets (%u dropped, %u underruns, %u skips)",
             g_s.sent, g_s.dropped, g_s.underruns, g_s.skips);
}

/* A weak link fills the adapter's buffers: smaller packets get through, larger ones come back */
static void adapt(long now)
{
    if (now - g_s.window_at < 1000) return;
    if (g_s.window_drops) {
        int floor = BITPOOL_FLOOR > g_s.m.cfg.min_bitpool ? BITPOOL_FLOOR : g_s.m.cfg.min_bitpool;
        int bp = g_s.m.bitpool - 6 < floor ? floor : g_s.m.bitpool - 6;
        if (bp != g_s.m.bitpool) {
            media_set_bitpool(&g_s.m, bp);
            log_line("stream: %u packets dropped in 1 s, bitpool down to %d", g_s.window_drops, g_s.m.bitpool);
        }
        g_s.clean_since = now;
    } else if (g_s.m.bitpool < g_s.m.cfg.max_bitpool && now - g_s.clean_since > 10000) {
        media_set_bitpool(&g_s.m, g_s.m.bitpool + 2);
        log_line("stream: 10 s without drops, bitpool up to %d", g_s.m.bitpool);
        g_s.clean_since = now;
    }
    g_s.window_drops = 0;
    g_s.window_at = now;
}

void stream_tick(stream_send_fn send, void *ctx)
{
    if (!g_s.active) return;
    long t = now_us();
    if (t - g_s.next_us > LATE_RESYNC_US) {
        g_s.late++;
        g_s.next_us = t;
    }
    for (int k = 0; k < MAX_PER_TICK && t >= g_s.next_us; k++) {
        uint8_t pkt[1100];
        int len = media_build(&g_s.m, pull, NULL, pkt, (int)sizeof(pkt));
        if (len < 0) {
            log_line("stream: packet does not fit, stopping");
            g_s.active = 0;
            return;
        }
        g_s.next_us += media_packet_us(&g_s.m);
        int r = send(ctx, pkt, len);
        if (r < 0) {
            g_s.active = 0;
            return;
        }
        if (r == 0) {
            g_s.dropped++;
            g_s.window_drops++;
        } else {
            g_s.sent++;
        }
    }
    adapt(now_ms());
}

int stream_wait_ms(int cap)
{
    if (!g_s.active) return cap;
    long left = (g_s.next_us - now_us()) / 1000;
    if (left < 0) return 0;
    return left < cap ? (int)left : cap;
}

void stream_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    pthread_mutex_lock(&g_s.lock);
    g_s.volume = pct;
    /* Squared, so the slider feels even to the ear */
    g_s.gain_q12 = pct * pct * 4096 / 10000;
    pthread_mutex_unlock(&g_s.lock);
}

int stream_volume(void)
{
    return g_s.volume;
}

void stream_test_tone(int ms)
{
    pthread_mutex_lock(&g_s.lock);
    g_s.tone_until = now_ms() + ms;
    pthread_mutex_unlock(&g_s.lock);
}

/* The capture thread: opens the session while the stream wants it, closes it after a while without */
static void *capture_worker(void *arg)
{
    (void)arg;
    int16_t pcm[2 * 2048];
    long next_try = 0;
    for (;;) {
        long now = now_ms();
        if (!g_s.want) {
            if (!g_s.unwanted_since) g_s.unwanted_since = now;
            if (g_s.capturing && now - g_s.unwanted_since > CAPTURE_IDLE_MS) {
                capture_close();
                g_s.capturing = 0;
            }
            if (!g_s.capturing) {
                sleep_ms(100);
                continue;
            }
        } else {
            g_s.unwanted_since = 0;
        }
        if (!g_s.capturing) {
            if (now < next_try) {
                sleep_ms(100);
                continue;
            }
            g_s.capturing = capture_open();
            if (!g_s.capturing) {
                log_line("stream: audio capture did not open (%s), trying again in %d s", capture_status(),
                         CAPTURE_RETRY_MS / 1000);
                next_try = now + CAPTURE_RETRY_MS;
                continue;
            }
        }
        int n = capture_read(pcm, 2048);
        if (n > 0) {
            stream_feed(pcm, n);
        } else if (n < 0) {
            capture_close();
            g_s.capturing = 0;
            next_try = now + 1000;
        } else {
            sleep_ms(2);
        }
    }
    return NULL;
}

void stream_want_capture(int on)
{
    g_s.want = on;
    if (on && !g_s.thread_on) {
        if (pthread_create(&g_s.thread, NULL, capture_worker, NULL) == 0) {
            pthread_detach(g_s.thread);
            g_s.thread_on = 1;
        } else {
            log_line("stream: capture thread not started");
        }
    }
}

int stream_status_json(char *out, int n)
{
    pthread_mutex_lock(&g_s.lock);
    int buffered_ms = g_s.count * 1000 / MEDIA_IN_RATE;
    unsigned underruns = g_s.underruns, skips = g_s.skips;
    pthread_mutex_unlock(&g_s.lock);
    int len = snprintf(out, (size_t)n,
        "{\"active\":%s,\"rate\":%d,\"bitpool\":%d,\"bitpool_max\":%d,\"frames_per_packet\":%d,"
        "\"packet_ms\":%.1f,\"sent\":%u,\"dropped\":%u,\"late\":%u,\"underruns\":%u,\"skips\":%u,"
        "\"buffered_ms\":%d,\"volume\":%d,\"capture\":\"%s\",\"capturing\":%s}",
        g_s.active ? "true" : "false", g_s.active ? g_s.m.rate : 0, g_s.active ? g_s.m.bitpool : 0,
        g_s.active ? g_s.m.cfg.max_bitpool : 0, g_s.active ? g_s.m.per_pkt : 0,
        g_s.active ? media_packet_us(&g_s.m) / 1000.0 : 0.0, g_s.sent, g_s.dropped, g_s.late,
        underruns, skips, buffered_ms, g_s.volume, capture_status(), g_s.capturing ? "true" : "false");
    return len < n ? len : n - 1;
}
