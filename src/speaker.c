#include "speaker.h"
#include "a2dp/host.h"
#include "a2dp/store.h"
#include "audio/stream.h"
#include "bt/usb_hci.h"
#include "log.h"
#include "ui_text.h"
#include "util.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOCAL_NAME        "BlueBird (PS5)"
#define FIND_EVERY_MS     2000
#define NO_ADAPTER_TELL_MS 20000      /* nothing plugged this long after start: tell the user once */
#define FIRST_CALL_MS     2000        /* after the host is up, the last speaker is called */
#define RECALL_MIN_MS     10000
#define RECALL_MAX_MS     60000
#define SCAN_S            12
#define USB_RESETS        2
#define FOUND_MAX         24
#define HEARTBEAT_MS      30000
#define STREAM_STATS_MS   10000

typedef struct {
    char     addr[18];
    char     name[64];
    int      rssi, audio;
    uint32_t cod;
} found_t;

static struct {
    pthread_t       thread;
    volatile int    stop;
    int             running;
    pthread_mutex_t lock;
    char            settings_path[256];

    /* Requests from the dashboard */
    int             req_scan, req_disconnect, req_restart;
    char            req_connect[18];

    /* Settings */
    int             auto_on;
    char            prefer[16];

    /* What the dashboard shows */
    const char     *bt_state;
    usb_hci_info_t  info;
    int             have_adapter;
    host_status_t   hs;
    int             scanning;
    found_t         found[FOUND_MAX];
    int             nfound;
    long            next_call_at;
    int             user_dropped;
} g_sp = { .lock = PTHREAD_MUTEX_INITIALIZER, .auto_on = 1, .bt_state = "looking for an adapter" };

static void json_str(char *out, size_t cap, const char *s)
{
    size_t o = 0;
    for (; s && *s && o + 7 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static void save_settings(void)
{
    FILE *f = fopen(g_sp.settings_path, "w");
    if (!f) return;
    fprintf(f, "volume %d\nauto %d\nadapter %s\n", stream_volume(), g_sp.auto_on, g_sp.prefer[0] ? g_sp.prefer : "-");
    fclose(f);
}

static void load_settings(void)
{
    FILE *f = fopen(g_sp.settings_path, "r");
    if (!f) return;
    char key[32], val[32];
    while (fscanf(f, "%31s %31s", key, val) == 2) {
        if (!strcmp(key, "volume")) stream_set_volume(atoi(val));
        else if (!strcmp(key, "auto")) g_sp.auto_on = atoi(val) != 0;
        else if (!strcmp(key, "adapter") && strcmp(val, "-")) snprintf(g_sp.prefer, sizeof(g_sp.prefer), "%s", val);
    }
    fclose(f);
}

/* ---- host events, on the worker thread --------------------------------------- */

static void on_found(void *ud, const uint8_t addr[6], const char *name, uint32_t cod, int rssi, int audio)
{
    (void)ud;
    char a[18];
    store_addr_str(addr, a);
    pthread_mutex_lock(&g_sp.lock);
    int i;
    for (i = 0; i < g_sp.nfound; i++)
        if (!strcmp(g_sp.found[i].addr, a)) break;
    if (i == g_sp.nfound && g_sp.nfound < FOUND_MAX) g_sp.nfound++;
    if (i < g_sp.nfound) {
        found_t *f = &g_sp.found[i];
        snprintf(f->addr, sizeof(f->addr), "%s", a);
        if (name && name[0]) snprintf(f->name, sizeof(f->name), "%s", name);
        f->cod = cod;
        f->rssi = rssi;
        f->audio = audio;
    }
    pthread_mutex_unlock(&g_sp.lock);
}

static int send_media(void *ctx, const uint8_t *pkt, int len)
{
    return host_send_media((host_t *)ctx, pkt, len);
}

/* ---- the worker ------------------------------------------------------------- */

static void publish(host_t *h)
{
    host_status_t s;
    host_status(h, &s);
    pthread_mutex_lock(&g_sp.lock);
    g_sp.hs = s;
    g_sp.scanning = host_scanning(h);
    pthread_mutex_unlock(&g_sp.lock);
}

/* Calls the last speaker while nothing is linked. The first call after the adapter starts is
 * told to the user; the retries are only logged. */
static void call_last(host_t *h, long now, long *backoff, int *told)
{
    store_entry_t last;
    if (!g_sp.auto_on || g_sp.user_dropped || host_scanning(h) || !store_last(&last)) return;
    if (now < g_sp.next_call_at) return;
    host_status_t s;
    host_status(h, &s);
    if (s.state != HOST_IDLE) return;
    const char *name = last.name[0] ? last.name : "?";
    log_line("speaker: calling the last speaker, %s (next try in %ld s)", name, *backoff / 1000);
    if (!*told) {
        *told = 1;
        notify_ps5(ui_text(UI_CONNECTING), name);
    }
    host_connect(h, last.addr, last.name);
    g_sp.next_call_at = now + *backoff;
    *backoff = *backoff * 2 > RECALL_MAX_MS ? RECALL_MAX_MS : *backoff * 2;
}

/* One line now and then with everything that matters, so a log shows the state at any moment */
static void heartbeat(host_t *h, const usb_hci_info_t *info)
{
    host_status_t s;
    host_status(h, &s);
    char st[512];
    stream_status_json(st, sizeof(st));
    store_entry_t saved[STORE_MAX];
    int nsaved = store_list(saved, STORE_MAX);
    log_line("speaker: heartbeat: adapter %s %04x:%04x, buffers %d/%d free, link %s \"%s\", sent %u dropped %u, "
             "%d saved speaker(s), auto %s; stream %s",
             info->chip, info->vid, info->pid, s.acl_free, s.acl_bufs, host_state_name(s.state), s.name,
             s.media_sent, s.media_dropped, nsaved, g_sp.auto_on ? "on" : "off", st);
}

static void run_host(usb_hci_info_t *info)
{
    log_step("opening the Bluetooth host");
    host_events ev = { on_found, NULL, NULL, NULL };
    host_t *h = host_open(usb_hci_transport(), &ev, info->fake_csr);
    if (!h) {
        log_line("speaker: the Bluetooth host did not start on this adapter");
        return;
    }
    pthread_mutex_lock(&g_sp.lock);
    g_sp.bt_state = "ready";
    pthread_mutex_unlock(&g_sp.lock);
    log_line("speaker: ready on %s (%s, %04x:%04x, address %s)", info->chip, info->path, info->vid, info->pid,
             info->address);
    notify_ps5(ui_text(UI_ADAPTER_READY), info->chip);
    log_step("running the Bluetooth host");

    long now = now_ms(), backoff = RECALL_MIN_MS, last_pub = 0, last_beat = now, last_stats = now;
    g_sp.next_call_at = now + FIRST_CALL_MS;
    int prev = HOST_IDLE, played = 0, told_call = 0, asked = 0;
    unsigned pairings = 0;
    char played_name[64] = "", asked_name[64] = "";
    while (!g_sp.stop) {
        int scan, drop, restart;
        char connect[18];
        pthread_mutex_lock(&g_sp.lock);
        scan = g_sp.req_scan;
        drop = g_sp.req_disconnect;
        restart = g_sp.req_restart;
        snprintf(connect, sizeof(connect), "%s", g_sp.req_connect);
        g_sp.req_scan = g_sp.req_disconnect = 0;
        g_sp.req_connect[0] = 0;
        if (scan) g_sp.nfound = 0;
        pthread_mutex_unlock(&g_sp.lock);
        if (restart) {
            log_line("speaker: restarting Bluetooth for another adapter choice");
            break;
        }
        if (drop) {
            log_line("speaker: disconnect asked from the dashboard; no reconnecting until Connect is pressed");
            g_sp.user_dropped = 1;
            host_disconnect(h);
        }
        if (scan) {
            if (host_scan(h, SCAN_S)) log_line("speaker: search asked from the dashboard");
            else log_line("speaker: search not possible while a speaker is being set up");
        }
        if (connect[0]) {
            uint8_t a[6];
            char name[64] = "";
            pthread_mutex_lock(&g_sp.lock);
            for (int i = 0; i < g_sp.nfound; i++)
                if (!strcmp(g_sp.found[i].addr, connect)) snprintf(name, sizeof(name), "%s", g_sp.found[i].name);
            pthread_mutex_unlock(&g_sp.lock);
            if (!name[0]) {
                store_entry_t list[STORE_MAX];
                int n = store_list(list, STORE_MAX);
                for (int i = 0; i < n; i++) {
                    char s[18];
                    store_addr_str(list[i].addr, s);
                    if (!strcmp(s, connect)) snprintf(name, sizeof(name), "%s", list[i].name);
                }
            }
            if (store_addr_parse(connect, a)) {
                host_status_t s;
                host_status(h, &s);
                if (s.state != HOST_IDLE && memcmp(s.addr, a, 6) != 0) {
                    /* Another speaker is on: drop it first, call this one when the link is down */
                    log_line("speaker: %s asked while %s is linked: dropping that one first", connect, s.name);
                    host_disconnect(h);
                    pthread_mutex_lock(&g_sp.lock);
                    snprintf(g_sp.req_connect, sizeof(g_sp.req_connect), "%s", connect);
                    pthread_mutex_unlock(&g_sp.lock);
                } else if (s.state == HOST_IDLE) {
                    log_line("speaker: connect to %s \"%s\" asked from the dashboard", connect, name);
                    g_sp.user_dropped = 0;
                    backoff = RECALL_MIN_MS;
                    g_sp.next_call_at = now_ms() + RECALL_MIN_MS;
                    asked = 1;
                    snprintf(asked_name, sizeof(asked_name), "%s", name[0] ? name : connect);
                    notify_ps5(ui_text(UI_CONNECTING), asked_name);
                    host_connect(h, a, name);
                }
            }
        }

        if (host_poll(h, stream_wait_ms(10)) < 0) {
            log_line("speaker: the adapter stopped answering");
            notify_ps5("%s", ui_text(UI_ADAPTER_GONE));
            break;
        }

        host_status_t s;
        host_status(h, &s);
        if (s.pairings != pairings) {
            pairings = s.pairings;
            notify_ps5(ui_text(UI_PAIRED), s.name[0] ? s.name : "?");
        }
        if (s.state != prev) {
            if (s.state == HOST_STREAMING) {
                stream_start(&s.cfg, s.mtu);
                store_put(s.addr, s.name, NULL);
                store_touch(s.addr);
                backoff = RECALL_MIN_MS;
                if (!played || strcmp(played_name, s.name)) notify_ps5(ui_text(UI_SPEAKER_PLAYING), s.name[0] ? s.name : "?");
                played = 1;
                asked = 0;
                snprintf(played_name, sizeof(played_name), "%s", s.name);
            } else if (prev == HOST_STREAMING) {
                stream_stop();
            }
            if (s.state == HOST_IDLE && prev >= HOST_READY) {
                notify_ps5(ui_text(UI_SPEAKER_LOST), played_name[0] ? played_name : "?");
                played = 0;
                told_call = 0;
                g_sp.next_call_at = now_ms() + RECALL_MIN_MS;
            } else if (s.state == HOST_IDLE && asked) {
                /* A connection the user asked for failed: say so; the reason is on the dashboard */
                log_line("speaker: the connection to %s failed: %s", asked_name, s.last_error);
                notify_ps5(ui_text(UI_CONNECT_FAILED), asked_name);
                asked = 0;
            }
            stream_want_capture(s.state >= HOST_READY);
            prev = s.state;
        }
        stream_tick(send_media, h);

        now = now_ms();
        call_last(h, now, &backoff, &told_call);
        if (now - last_pub > 200) {
            last_pub = now;
            publish(h);
        }
        if (now - last_beat >= HEARTBEAT_MS) {
            last_beat = now;
            heartbeat(h, info);
        }
        if (s.state == HOST_STREAMING && now - last_stats >= STREAM_STATS_MS) {
            last_stats = now;
            char st[512];
            stream_status_json(st, sizeof(st));
            log_line("speaker: stream %s, adapter buffers %d/%d free", st, s.acl_free, s.acl_bufs);
        }
    }
    stream_stop();
    stream_want_capture(0);
    if (prev >= HOST_READY) notify_ps5(ui_text(UI_SPEAKER_LOST), played_name[0] ? played_name : "?");
    host_close(h);
}

static void set_bt_state(const char *st, int have)
{
    pthread_mutex_lock(&g_sp.lock);
    g_sp.bt_state = st;
    g_sp.have_adapter = have;
    if (!have) memset(&g_sp.hs, 0, sizeof(g_sp.hs));
    pthread_mutex_unlock(&g_sp.lock);
}

static void *worker(void *arg)
{
    (void)arg;
    long started = now_ms(), last_none_log = 0;
    int told_none = 0, resets = 0;
    usb_hci_set_stop_flag(&g_sp.stop);
    log_line("speaker: looking for a USB Bluetooth adapter every %d s", FIND_EVERY_MS / 1000);
    while (!g_sp.stop) {
        usb_hci_info_t info;
        char prefer[16];
        pthread_mutex_lock(&g_sp.lock);
        snprintf(prefer, sizeof(prefer), "%s", g_sp.prefer);
        g_sp.req_restart = 0;
        pthread_mutex_unlock(&g_sp.lock);
        log_step("looking for an adapter");
        if (!usb_hci_find(prefer, &info)) {
            set_bt_state("looking for an adapter", 0);
            long now = now_ms();
            if (now - last_none_log >= 60000) {
                last_none_log = now;
                log_line("speaker: no usable Bluetooth adapter yet (%ld s since start)", (now - started) / 1000);
            }
            if (!told_none && now - started > NO_ADAPTER_TELL_MS) {
                told_none = 1;
                notify_ps5("%s", ui_text(UI_NO_ADAPTER));
            }
            sleep_ms(FIND_EVERY_MS);
            continue;
        }
        told_none = 1;
        pthread_mutex_lock(&g_sp.lock);
        g_sp.info = info;
        g_sp.have_adapter = 1;
        g_sp.bt_state = "starting";
        pthread_mutex_unlock(&g_sp.lock);
        char ids[16];
        snprintf(ids, sizeof(ids), "%04x:%04x", info.vid, info.pid);
        notify_ps5(ui_text(UI_ADAPTER_FOUND), ids);

        log_step("starting the adapter");
        long t0 = now_ms();
        int started_ok = usb_hci_start(LOCAL_NAME, &info);
        log_line("speaker: adapter start %s in %ld ms (%s, firmware %d)", started_ok ? "done" : "failed",
                 now_ms() - t0, info.chip[0] ? info.chip : "chip unknown", info.firmware);
        pthread_mutex_lock(&g_sp.lock);
        g_sp.info = info;
        pthread_mutex_unlock(&g_sp.lock);
        if (started_ok) {
            resets = 0;
            run_host(&info);
            usb_hci_diag();
        }
        usb_hci_close();
        set_bt_state(g_sp.stop ? "stopped" : "adapter lost", 0);
        if (g_sp.stop) break;
        if (!started_ok) {
            /* A locked-up adapter is re-enumerated, as unplugging it would, before asking for a replug */
            if (resets < USB_RESETS && usb_hci_still_there(&info) && usb_hci_reset(info.path)) {
                resets++;
                sleep_ms(3000);
                continue;
            }
            usb_hci_mark_failed(&info);
            notify_ps5("%s", ui_text(UI_ADAPTER_FAILED));
        }
        sleep_ms(FIND_EVERY_MS);
    }
    log_line("speaker: worker stopped");
    return NULL;
}

int speaker_init(const char *settings_dir)
{
    char path[256];
    snprintf(g_sp.settings_path, sizeof(g_sp.settings_path), "%s/settings.txt", settings_dir);
    snprintf(path, sizeof(path), "%s/speakers.txt", settings_dir);
    store_init(path);
    stream_init();
    load_settings();
    log_line("speaker: settings: volume %d, reconnect %s, adapter %s", stream_volume(), g_sp.auto_on ? "on" : "off",
             g_sp.prefer[0] ? g_sp.prefer : "any");
    if (!thread_start(&g_sp.thread, worker, NULL, "bluetooth worker")) return 0;
    g_sp.running = 1;
    return 1;
}

void speaker_cleanup(void)
{
    if (!g_sp.running) return;
    g_sp.stop = 1;
    pthread_join(g_sp.thread, NULL);
    g_sp.running = 0;
}

static int ready(void)
{
    return !strcmp(g_sp.bt_state, "ready");
}

int speaker_scan(void)
{
    pthread_mutex_lock(&g_sp.lock);
    int ok = ready();
    if (ok) g_sp.req_scan = 1;
    pthread_mutex_unlock(&g_sp.lock);
    return ok;
}

int speaker_connect(const char *addr)
{
    uint8_t a[6];
    if (!store_addr_parse(addr, a)) return 0;
    pthread_mutex_lock(&g_sp.lock);
    int ok = ready();
    if (ok) store_addr_str(a, g_sp.req_connect);
    pthread_mutex_unlock(&g_sp.lock);
    return ok;
}

int speaker_disconnect(void)
{
    pthread_mutex_lock(&g_sp.lock);
    int ok = ready();
    if (ok) g_sp.req_disconnect = 1;
    pthread_mutex_unlock(&g_sp.lock);
    return ok;
}

int speaker_forget(const char *addr)
{
    uint8_t a[6];
    if (!store_addr_parse(addr, a)) return 0;
    pthread_mutex_lock(&g_sp.lock);
    int linked = g_sp.hs.state != HOST_IDLE && memcmp(g_sp.hs.addr, a, 6) == 0;
    if (linked) g_sp.req_disconnect = 1;
    pthread_mutex_unlock(&g_sp.lock);
    return store_forget(a);
}

void speaker_set_auto(int on)
{
    g_sp.auto_on = on != 0;
    save_settings();
}

void speaker_set_volume(int pct)
{
    stream_set_volume(pct);
    save_settings();
}

void speaker_test_tone(void)
{
    stream_test_tone(3000);
}

void speaker_set_adapter(const char *prefer)
{
    unsigned v, p;
    pthread_mutex_lock(&g_sp.lock);
    if (prefer && sscanf(prefer, "%4x:%4x", &v, &p) == 2) snprintf(g_sp.prefer, sizeof(g_sp.prefer), "%04x:%04x", v, p);
    else g_sp.prefer[0] = 0;
    g_sp.req_restart = 1;
    pthread_mutex_unlock(&g_sp.lock);
    save_settings();
}

int speaker_status_json(char *out, int n)
{
    char stream[512], adapters[512], name[140], err[200], chip[100];
    stream_status_json(stream, sizeof(stream));
    usb_hci_list_json(adapters, sizeof(adapters));
    store_entry_t saved[STORE_MAX];
    int nsaved = store_list(saved, STORE_MAX);

    pthread_mutex_lock(&g_sp.lock);
    host_status_t s = g_sp.hs;
    char a[18];
    store_addr_str(s.addr, a);
    json_str(name, sizeof(name), s.name);
    json_str(err, sizeof(err), s.last_error);
    json_str(chip, sizeof(chip), g_sp.info.chip);
    int len = snprintf(out, (size_t)n,
        "{\"bt\":\"%s\",\"adapter\":{\"present\":%s,\"path\":\"%s\",\"id\":\"%04x:%04x\",\"chip\":\"%s\","
        "\"address\":\"%s\",\"firmware\":%d,\"clone\":%s,\"acl_mtu\":%d,\"acl_bufs\":%d,\"prefer\":\"%s\"},"
        "\"adapters\":%s,"
        "\"link\":{\"state\":\"%s\",\"code\":%d,\"addr\":\"%s\",\"name\":\"%s\",\"incoming\":%s,\"error\":\"%s\","
        "\"mtu\":%d,\"media_sent\":%u,\"media_dropped\":%u},"
        "\"stream\":%s,\"auto\":%s,\"scanning\":%s,\"found\":[",
        g_sp.bt_state, g_sp.have_adapter ? "true" : "false", g_sp.info.path, g_sp.info.vid, g_sp.info.pid, chip,
        g_sp.info.address, g_sp.info.firmware, g_sp.info.fake_csr ? "true" : "false", s.acl_mtu, s.acl_bufs, g_sp.prefer,
        adapters,
        host_state_name(s.state), s.state, s.state == HOST_IDLE ? "" : a, name, s.incoming ? "true" : "false", err,
        s.mtu, s.media_sent, s.media_dropped,
        stream, g_sp.auto_on ? "true" : "false", g_sp.scanning ? "true" : "false");
    for (int i = 0; i < g_sp.nfound && len < n; i++) {
        char fname[140];
        json_str(fname, sizeof(fname), g_sp.found[i].name);
        len += snprintf(out + len, (size_t)(n - len), "%s{\"addr\":\"%s\",\"name\":\"%s\",\"rssi\":%d,\"audio\":%s,\"cod\":\"%06x\"}",
                        i ? "," : "", g_sp.found[i].addr, fname, g_sp.found[i].rssi, g_sp.found[i].audio ? "true" : "false",
                        (unsigned)g_sp.found[i].cod);
    }
    pthread_mutex_unlock(&g_sp.lock);
    if (len < n) len += snprintf(out + len, (size_t)(n - len), "],\"saved\":[");
    for (int i = 0; i < nsaved && len < n; i++) {
        char sa[18], sname[140];
        store_addr_str(saved[i].addr, sa);
        json_str(sname, sizeof(sname), saved[i].name);
        len += snprintf(out + len, (size_t)(n - len), "%s{\"addr\":\"%s\",\"name\":\"%s\",\"paired\":%s}",
                        i ? "," : "", sa, sname, saved[i].has_key ? "true" : "false");
    }
    if (len < n) len += snprintf(out + len, (size_t)(n - len), "]}");
    return len < n ? len : n - 1;
}
