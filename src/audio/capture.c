#include "capture.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static char g_status[96] = "not started";
static uint64_t g_payload_authid;

/* The source probe: what each value of the open parameter's source field gives */
#define PROBE_SOURCES 8
typedef struct {
    int    tried, opened;
    char   result[48];
    double bytes_per_s;
    int    level;           /* peak, 0..100, read as the format the data looks like */
    int    nonzero;         /* share of non-zero words, 0..100 */
    int    s16;             /* the data looks like int16 rather than float32 */
} probe_t;
static probe_t g_probe[PROBE_SOURCES];
static volatile int g_probe_state;  /* 0 never run, 1 running, 2 done */

int capture_probe_json(char *out, int n)
{
    int len = snprintf(out, (size_t)n, "{\"state\":%d,\"sources\":[", g_probe_state);
    int first = 1;
    for (int i = 0; i < PROBE_SOURCES && len < n; i++) {
        const probe_t *r = &g_probe[i];
        if (!r->tried) continue;
        len += snprintf(out + len, (size_t)(n - len),
                        "%s{\"source\":%d,\"opened\":%s,\"result\":\"%s\",\"bytes_per_s\":%.0f,\"level\":%d,\"nonzero\":%d,\"format\":\"%s\"}",
                        first ? "" : ",", i, r->opened ? "true" : "false", r->result, r->bytes_per_s, r->level,
                        r->nonzero, r->s16 ? "int16" : "float32");
        first = 0;
    }
    if (len < n) len += snprintf(out + len, (size_t)(n - len), "]}");
    return len < n ? len : n - 1;
}

void capture_set_payload_authid(uint64_t authid)
{
    g_payload_authid = authid;
}

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
    long        rate_t0, last_ok, last_restart, stats_at;
    uint64_t    rate_bytes, reads_ok, reads_empty, reads_invalid;
    uint64_t    win_ok, win_empty, win_frames;
    int         win_peak;
    uint32_t    source;             /* the open parameter's source field: 2, the Remote Play mix */
} g = { .source = 2 };

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

/* Rights tried in turn: the ones we run with, Avcap2's own, a fallback, the payload's at start */
static int open_and_start(int auth_mode)
{
    uint8_t param[OPEN_PARAM_SIZE];
    if (auth_mode == 1) set_authid(AUTHID_AVCAP2);
    else if (auth_mode == 2) set_authid(AUTHID_FALLBACK);
    else if (auth_mode == 3) {
        if (!g_payload_authid) return 0;
        set_authid(g_payload_authid);
    }
    log_line("capture: opening with rights %d (authid 0x%016llx)", auth_mode,
             (unsigned long long)kernel_get_ucred_authid(-1));

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
    *(uint32_t *)(void *)(param + 0x14) = g.source;
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

static int load_libs(void)
{
    uint32_t kh = 0, ah = 0;
    if (g.read) return 1;
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
        g.read = NULL;
        return 0;
    }
    return 1;
}

int capture_open(void)
{
    if (g.handle) return 1;
    if (!load_libs()) return 0;
    uint64_t authid = kernel_get_ucred_authid(-1);
    uint8_t caps[16];
    int have_caps = kernel_get_ucred_caps(-1, caps) == 0;
    for (int mode = 0; mode <= 3; mode++) {
        if (open_and_start(mode)) {
            g.auth_mode = mode;
            g.read_flag = 1;
            g.dead = 0;
            g.restarts = 0;
            g.nch = 2;
            g.fmt_checked = g.rate_done = 0;
            g.rate_t0 = 0;
            g.rate_bytes = 0;
            g.last_ok = g.last_restart = g.stats_at = now_ms();
            g.win_ok = g.win_empty = g.win_frames = 0;
            g.win_peak = 0;
            snprintf(g_status, sizeof(g_status), "capturing (rights %d)", mode);
            log_line("capture: session open (auth mode %d)", mode);
            return 1;
        }
    }
    log_line("capture: no rights opened the session (%s)", g_status);
    /* Back to the rights we started with, so the USB adapter keeps working as before */
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

/* Opens the capture with each source value for 1.5 s and measures what comes. The controller's
 * speaker is a separate stream from the Remote Play mix (PS5, 13 Nov); this looks for a source
 * that carries it. */
int capture_probe(void)
{
    if (!load_libs()) {
        log_line("capture: probe not possible (%s)", g_status);
        return 0;
    }
    g_probe_state = 1;
    int mode = g.auth_mode;
    capture_close();
    memset(g_probe, 0, sizeof(g_probe));
    log_line("capture: probe of %d sources, 1.5 s each, with rights %d", PROBE_SOURCES, mode);
    for (int src = 0; src < PROBE_SOURCES; src++) {
        probe_t *r = &g_probe[src];
        static char step[40];
        snprintf(step, sizeof(step), "probing capture source %d", src);
        log_step(step);
        r->tried = 1;
        g.source = (uint32_t)src;
        if (!open_and_start(mode)) {
            snprintf(r->result, sizeof(r->result), "%s", g_status);
            log_line("capture: source %d: %s", src, g_status);
            continue;
        }
        r->opened = 1;
        long t0 = now_ms();
        uint64_t bytes = 0, words = 0, nz = 0, bad = 0;
        float fpeak = 0.f;
        int speak = 0;
        uint8_t meta[META_SIZE];
        while (now_ms() - t0 < 1500) {
            memset(meta, 0, sizeof(meta));
            *(uint32_t *)(void *)meta = META_SIZE;
            int got = g.read(g.handle, g.buf, READ_BYTES, meta, 1);
            if (got <= 0) {
                usleep(2000);
                continue;
            }
            if (got > READ_BYTES) got = READ_BYTES;
            bytes += (uint64_t)got;
            const uint32_t *w = (const uint32_t *)(void *)g.buf;
            const int16_t *si = (const int16_t *)(void *)g.buf;
            for (int k = 0; k < got / 4; k++) {
                words++;
                if (!w[k] || w[k] == 0x80000000u) continue;
                nz++;
                uint32_t e = (w[k] >> 23) & 0xFF;
                if (e == 0xFF || e > 0x7F || e < 0x50) bad++;
                else {
                    float v = g.buf[k] < 0 ? -g.buf[k] : g.buf[k];
                    if (v > fpeak) fpeak = v;
                }
                int a = si[2 * k] < 0 ? -si[2 * k] : si[2 * k];
                if (a > speak) speak = a;
            }
        }
        long ms = now_ms() - t0;
        g.stop(g.handle);
        g.close(g.handle);
        g.handle = 0;
        if (g.term) g.term();
        r->bytes_per_s = ms > 0 ? (double)bytes * 1000.0 / (double)ms : 0;
        r->nonzero = words ? (int)(nz * 100 / words) : 0;
        r->s16 = nz >= 64 && bad * 4 > nz;
        r->level = r->s16 ? speak * 100 / 32767 : (int)(fpeak > 1.f ? 100 : fpeak * 100.f);
        snprintf(r->result, sizeof(r->result), "opened");
        log_line("capture: source %d: %.0f B/s, %d%% non-zero words, %s, level %d%%", src, r->bytes_per_s,
                 r->nonzero, r->s16 ? "int16" : "float32", r->level);
    }
    g.source = 2;
    g_probe_state = 2;
    snprintf(g_status, sizeof(g_status), "probe done");
    log_line("capture: probe done");
    return 1;
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
        g.win_ok++;
        g.win_frames += (uint64_t)frames;
        for (int i = 0; i < 2 * frames; i += 16) {
            int v = out[i] < 0 ? -out[i] : out[i];
            if (v > g.win_peak) g.win_peak = v;
        }
        if (g.reads_ok == 1) log_line("capture: first audio read, %d frames (%s, %d channels)", frames,
                                      g.fmt_s16 ? "int16" : "float32", g.nch);
        if (now - g.stats_at >= 10000) {
            log_line("capture: last %ld s: %llu reads with audio, %llu empty, %llu frames (%.0f per s), peak %d%%",
                     (now - g.stats_at) / 1000, (unsigned long long)g.win_ok, (unsigned long long)g.win_empty,
                     (unsigned long long)g.win_frames, g.win_frames * 1000.0 / (double)(now - g.stats_at),
                     g.win_peak * 100 / 32767);
            g.stats_at = now;
            g.win_ok = g.win_empty = g.win_frames = 0;
            g.win_peak = 0;
        }
        return frames;
    }
    unsigned code = (unsigned)got;
    if (code == AVCAP2_EMPTY || code == AVCAP2_OVERRUN || got == 0) {
        g.reads_empty++;
        g.win_empty++;
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
        if (g.reads_invalid <= 5) log_line("capture: invalid status from ReadAudio (%llu)", (unsigned long long)g.reads_invalid);
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

int capture_probe(void)
{
    g_probe_state = 2;
    return 0;
}

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
