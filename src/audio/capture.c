#include "capture.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static char g_status[96] = "not started";

const char *capture_status(void)
{
    return g_status;
}

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#include <unistd.h>

#define NID_LoadStartModule  "wzvqT4UqKX8"
#define NID_Avcap2Initialize "svzPXluOz8U"
#define NID_Avcap2Terminate  "gojqghDU+1Y"
#define NID_Avcap2OpenAudio  "3nHpE7Dp5SE"
#define NID_Avcap2Close      "tR7gPe1i8hw"
#define NID_Avcap2Start      "8CGNCwBsItI"
#define NID_Avcap2Stop       "Z5dKK0xnQ8g"
#define NID_Avcap2ReadAudio  "WhsHggqdwcg"

/* Status codes of facility 0x8195 */
#define AVCAP2_INVALID 0x81950001u
#define AVCAP2_EMPTY   0x81950002u
#define AVCAP2_OVERRUN 0x81950004u
#define AVCAP2_ENDED   0x81950008u

#define OPEN_PARAM_SIZE 0x38
#define META_SIZE       0x10
#define READ_BYTES      0x2000
#define SAMPLE_RATE     48000

/* Opened first with the payload's own rights, then with these (HearBridge's order) */
#define AUTHID_AVCAP2   0x4900000000000002ULL
#define AUTHID_FALLBACK 0x4801000000000013ULL

typedef int (*fn_load_t)(const char *, size_t, const void *, uint32_t, void *, int *);
typedef int (*fn_void_t)(void);
typedef int (*fn_open_t)(int64_t *, void *, int);
typedef int (*fn_handle_t)(int64_t);
typedef int (*fn_read_t)(int64_t, void *, size_t, void *, int);

static struct {
    fn_void_t   init, term;
    fn_open_t   open;
    fn_handle_t close, start, stop;
    fn_read_t   read;
    int64_t     handle;
    int         auth_mode, read_flag, dead, restarts;
    float       buf[READ_BYTES / 4];
    int         fmt_checked, fmt_s16, nch, rate_done;
    long        rate_t0, last_ok, last_restart;
    uint64_t    rate_bytes, reads_ok, reads_empty, reads_invalid;
} g;

static int libkernel(uint32_t *out)
{
    static const char *const names[] = { "libkernel_web.sprx", "libkernel.sprx", "libkernel_sys.sprx" };
    for (int i = 0; i < 3; i++)
        if (kernel_dynlib_handle(-1, names[i], out) == 0 && *out) return 1;
    return 0;
}

static int load_sprx(fn_load_t load, const char *name)
{
    static const char *const roots[] = {
        "/system/common/lib/", "", "/system/priv/lib/", "/system_ex/common_ex/lib/", "/system_ex/priv_ex/lib/",
    };
    uint32_t handle = 0;
    if (kernel_dynlib_handle(-1, name, &handle) == 0 && handle) return 1;
    if (!load) return 0;
    for (int i = 0; i < 5; i++) {
        char path[256];
        int res = -1;
        snprintf(path, sizeof(path), "%s%s", roots[i], name);
        load(path, 0, 0, 0, 0, &res);
        if (kernel_dynlib_handle(-1, name, &handle) == 0 && handle) {
            log_line("capture: %s loaded from %s", name, path);
            return 1;
        }
    }
    log_line("capture: could not load %s", name);
    return 0;
}

static void set_authid(uint64_t authid)
{
    uint8_t caps[16];
    memset(caps, 0xff, sizeof(caps));
    uint64_t before = kernel_get_ucred_authid(-1);
    kernel_set_ucred_authid(-1, authid);
    kernel_set_ucred_caps(-1, caps);
    log_line("capture: authid 0x%llx -> 0x%llx", (unsigned long long)before,
             (unsigned long long)kernel_get_ucred_authid(-1));
}

static int open_and_start(int auth_mode)
{
    uint8_t param[OPEN_PARAM_SIZE];
    if (auth_mode == 1) set_authid(AUTHID_AVCAP2);
    else if (auth_mode == 2) set_authid(AUTHID_FALLBACK);

    int rv = g.init();
    if (rv != 0) {
        if (g.term) g.term();
        rv = g.init();
        if (rv != 0) {
            snprintf(g_status, sizeof(g_status), "sceAvcap2Initialize failed 0x%08x", (unsigned)rv);
            log_line("capture: %s (auth mode %d)", g_status, auth_mode);
            return 0;
        }
    }
    memset(param, 0, sizeof(param));
    *(uint64_t *)(void *)(param + 0x00) = OPEN_PARAM_SIZE;
    *(uint64_t *)(void *)(param + 0x08) = 0x18;
    *(uint32_t *)(void *)(param + 0x14) = 2;              /* source: the Remote Play mix */
    *(uint32_t *)(void *)(param + 0x1c) = 0xffffffffu;
    int64_t handle = 0;
    rv = g.open(&handle, param, 0);
    if (rv != 0 || !handle) {
        snprintf(g_status, sizeof(g_status), "OpenAudio failed 0x%08x", (unsigned)rv);
        log_line("capture: %s (auth mode %d)", g_status, auth_mode);
        if (g.term) g.term();
        return 0;
    }
    rv = g.start(handle);
    if (rv != 0) {
        snprintf(g_status, sizeof(g_status), "Start failed 0x%08x", (unsigned)rv);
        log_line("capture: %s (auth mode %d)", g_status, auth_mode);
        g.close(handle);
        if (g.term) g.term();
        return 0;
    }
    g.handle = handle;
    return 1;
}

int capture_open(void)
{
    uint32_t kh = 0, ah = 0;
    if (g.handle) return 1;
    if (!libkernel(&kh)) {
        snprintf(g_status, sizeof(g_status), "libkernel not found");
        return 0;
    }
    fn_load_t load = (fn_load_t)kernel_dynlib_resolve(-1, kh, NID_LoadStartModule);
    if (!load_sprx(load, "libSceIpmi.sprx") || !load_sprx(load, "libSceAvcap2.sprx") ||
        kernel_dynlib_handle(-1, "libSceAvcap2.sprx", &ah) != 0 || !ah) {
        snprintf(g_status, sizeof(g_status), "libSceAvcap2 not loaded");
        return 0;
    }
    g.init = (fn_void_t)kernel_dynlib_resolve(-1, ah, NID_Avcap2Initialize);
    g.term = (fn_void_t)kernel_dynlib_resolve(-1, ah, NID_Avcap2Terminate);
    g.open = (fn_open_t)kernel_dynlib_resolve(-1, ah, NID_Avcap2OpenAudio);
    g.close = (fn_handle_t)kernel_dynlib_resolve(-1, ah, NID_Avcap2Close);
    g.start = (fn_handle_t)kernel_dynlib_resolve(-1, ah, NID_Avcap2Start);
    g.stop = (fn_handle_t)kernel_dynlib_resolve(-1, ah, NID_Avcap2Stop);
    g.read = (fn_read_t)kernel_dynlib_resolve(-1, ah, NID_Avcap2ReadAudio);
    if (!g.init || !g.open || !g.close || !g.start || !g.stop || !g.read) {
        snprintf(g_status, sizeof(g_status), "Avcap2 functions missing");
        log_line("capture: %s", g_status);
        return 0;
    }
    uint64_t authid = kernel_get_ucred_authid(-1);
    uint8_t caps[16];
    int have_caps = kernel_get_ucred_caps(-1, caps) == 0;
    for (int mode = 0; mode <= 2; mode++) {
        if (open_and_start(mode)) {
            g.auth_mode = mode;
            g.read_flag = 1;
            g.dead = 0;
            g.restarts = 0;
            g.nch = 2;
            g.fmt_checked = g.rate_done = 0;
            g.rate_t0 = 0;
            g.rate_bytes = 0;
            g.last_ok = g.last_restart = now_ms();
            snprintf(g_status, sizeof(g_status), "capturing (rights %d)", mode);
            log_line("capture: session open (auth mode %d)", mode);
            return 1;
        }
    }
    /* Back to the payload's own rights, so the USB adapter keeps working as before */
    kernel_set_ucred_authid(-1, authid);
    if (have_caps) kernel_set_ucred_caps(-1, caps);
    return 0;
}

void capture_close(void)
{
    if (!g.handle) return;
    g.stop(g.handle);
    g.close(g.handle);
    g.handle = 0;
    if (g.term) g.term();
    snprintf(g_status, sizeof(g_status), "closed");
    log_line("capture: session closed (%llu reads, %llu empty)", (unsigned long long)g.reads_ok,
             (unsigned long long)g.reads_empty);
}

static int restart(void)
{
    log_line("capture: restarting the session");
    g.stop(g.handle);
    g.close(g.handle);
    g.handle = 0;
    if (g.term) g.term();
    return open_and_start(g.auth_mode);
}

/* Float32 in [-1, 1] has nearly every word near exponent 0x3F; int16 pairs read as floats do not */
static void check_format(int got)
{
    const uint32_t *w = (const uint32_t *)(void *)g.buf;
    int nw = got / 4, bad = 0, nz = 0;
    for (int k = 0; k < nw; k++) {
        uint32_t e = (w[k] >> 23) & 0xFF;
        if (!w[k] || w[k] == 0x80000000u) continue;
        nz++;
        if (e == 0xFF || e > 0x7F || e < 0x50) bad++;
    }
    if (nz >= 64) {
        g.fmt_checked = 1;
        g.fmt_s16 = bad * 4 > nz;
        log_line("capture: %d of %d words implausible as float: %s", bad, nz, g.fmt_s16 ? "int16" : "float32");
    }
}

/* Bytes per second give the channel count: 48 kHz x 2 x 4 bytes = 384000 B/s */
static void check_rate(int got, long now)
{
    if (!g.rate_t0) g.rate_t0 = now;
    g.rate_bytes += (uint64_t)got;
    if (now - g.rate_t0 < 2000) return;
    double bps = (double)g.rate_bytes * 1000.0 / (double)(now - g.rate_t0);
    double r = bps / ((double)SAMPLE_RATE * (g.fmt_s16 ? 2.0 : 4.0));
    int ch = (int)(r + 0.5);
    double dev = r - ch;
    g.nch = ((ch == 4 || ch == 6 || ch == 8) && dev < 0.15 && dev > -0.15) ? ch : 2;
    g.rate_done = 1;
    log_line("capture: %.0f B/s = %.2f channels, using the first two of %d", bps, r, g.nch);
}

int capture_read(int16_t *out, int max_frames)
{
    uint8_t meta[META_SIZE];
    if (!g.handle || g.dead) return -1;
    memset(meta, 0, sizeof(meta));
    *(uint32_t *)(void *)meta = META_SIZE;
    int got = g.read(g.handle, g.buf, READ_BYTES, meta, g.read_flag);
    long now = now_ms();
    if (got > 0) {
        if (got > READ_BYTES) got = READ_BYTES;
        if (!g.fmt_checked) check_format(got);
        if (!g.rate_done) check_rate(got, now);
        int bps = g.fmt_s16 ? 2 : 4, nch = g.nch;
        int frames = got / (nch * bps);
        if (frames > max_frames) frames = max_frames;
        const int16_t *si = (const int16_t *)(void *)g.buf;
        for (int f = 0; f < frames; f++) {
            for (int c = 0; c < 2; c++) {
                if (g.fmt_s16) {
                    out[2 * f + c] = si[f * nch + c];
                    continue;
                }
                float v = g.buf[f * nch + c];
                if (!(v == v)) v = 0.f;
                if (v > 1.f) v = 1.f;
                if (v < -1.f) v = -1.f;
                out[2 * f + c] = (int16_t)(v * 32767.f);
            }
        }
        g.last_ok = now;
        g.reads_ok++;
        return frames;
    }
    unsigned code = (unsigned)got;
    if (code == AVCAP2_EMPTY || code == AVCAP2_OVERRUN || got == 0) {
        g.reads_empty++;
        return 0;
    }
    if (code == AVCAP2_ENDED) {
        log_line("capture: the session ended");
        g.dead = 1;
        snprintf(g_status, sizeof(g_status), "session ended");
        return -1;
    }
    if (code == AVCAP2_INVALID) {
        /* Sticky after some system events: a restart clears it (HearBridge) */
        g.reads_invalid++;
        if (now - g.last_ok > 800 && now - g.last_restart > 800 && g.restarts < 3) {
            g.restarts++;
            g.last_restart = now;
            if (g.restarts == 2) g.read_flag = 0;
            if (!restart()) {
                g.dead = 1;
                return -1;
            }
            g.last_ok = now_ms();
        }
        return 0;
    }
    if (g.reads_ok == 0 && g.reads_empty < 3) log_line("capture: ReadAudio returned 0x%08x", code);
    return 0;
}

#else

int capture_open(void)
{
    snprintf(g_status, sizeof(g_status), "no capture on this build");
    return 0;
}

int capture_read(int16_t *out, int max_frames)
{
    (void)out;
    (void)max_frames;
    return -1;
}

void capture_close(void) {}

#endif
