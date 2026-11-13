/* The A2DP host (src/a2dp/host.c) against a simulated adapter and speaker: search, pairing,
 * L2CAP, AVDTP in both roles, and media packets through small ACL buffers. The simulator follows
 * the Bluetooth Core and AVDTP specifications; it is not a recording of a real speaker. */
#include "check.h"
#include "host.h"
#include "store.h"
#include "../src/audio/stream.h"
#include "util.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define QLEN 512
#define HANDLE 0x0042
#define SPK_SIG 0x0070
#define SPK_MEDIA 0x0071

typedef struct { uint8_t d[1200]; int n; } pkt_t;

static struct {
    /* the adapter */
    int acl_mtu, acl_bufs, inflight, frag_over, buf_over, max_frag;
    pkt_t ev[QLEN]; int ev_h, ev_n;
    pkt_t acl[QLEN]; int acl_h, acl_n;
    uint8_t rx[4096]; int rx_len, rx_need;
    int connected;

    /* the speaker */
    uint8_t addr[6];
    int off;                    /* does not answer calls */
    uint8_t key[16]; int has_key;
    int leads;                  /* it called us: it leads AVDTP as initiator */
    int suspend_after;          /* suspends the stream after this many media packets, 0 never */
    int suspended_once;
    unsigned host_sig, host_media;   /* the host's CIDs */
    int sig_open, media_open, sig_cfg_in, sig_cfg_out, media_cfg_in, media_cfg_out;
    int av_step, av_label;
    int configured, started, starts;
    unsigned media_pkts, media_bad, frames;
    int last_seq;
    int sdp_asked;
} S;

static void q_push(pkt_t *q, int *h, int *n, const uint8_t *d, int len)
{
    if (*n == QLEN) return;
    pkt_t *p = &q[(*h + *n) % QLEN];
    memcpy(p->d, d, (size_t)len);
    p->n = len;
    (*n)++;
}

static int q_pop(pkt_t *q, int *h, int *n, uint8_t *out, int max)
{
    if (!*n) return 0;
    pkt_t *p = &q[*h];
    int len = p->n < max ? p->n : max;
    memcpy(out, p->d, (size_t)len);
    *h = (*h + 1) % QLEN;
    (*n)--;
    return len;
}

static void event(int code, const uint8_t *p, int len)
{
    uint8_t b[300];
    b[0] = (uint8_t)code;
    b[1] = (uint8_t)len;
    memcpy(b + 2, p, (size_t)len);
    q_push(S.ev, &S.ev_h, &S.ev_n, b, len + 2);
}

static void cc(unsigned op, const uint8_t *ret, int len)
{
    uint8_t p[64] = { 1, (uint8_t)op, (uint8_t)(op >> 8) };
    memcpy(p + 3, ret, (size_t)len);
    event(0x0E, p, 3 + len);
}

static void cs(unsigned op, int status)
{
    uint8_t p[4] = { (uint8_t)status, 1, (uint8_t)op, (uint8_t)(op >> 8) };
    event(0x0F, p, 4);
}

static void conn_complete(int status)
{
    uint8_t p[11] = { (uint8_t)status, HANDLE & 0xFF, HANDLE >> 8 };
    memcpy(p + 3, S.addr, 6);
    p[9] = 1;
    p[10] = 0;
    S.connected = status == 0;
    event(0x03, p, 11);
}

static void auth_complete(int status)
{
    uint8_t p[3] = { (uint8_t)status, HANDLE & 0xFF, HANDLE >> 8 };
    event(0x06, p, 3);
}

/* The speaker sends an L2CAP frame to the host (one ACL packet: the simulated adapter's buffer is large) */
static void spk_send(unsigned cid, const uint8_t *d, int len)
{
    uint8_t b[1200];
    put16(b, HANDLE | 0x2000);
    put16(b + 2, (unsigned)(len + 4));
    put16(b + 4, (unsigned)len);
    put16(b + 6, cid);
    memcpy(b + 8, d, (size_t)len);
    q_push(S.acl, &S.acl_h, &S.acl_n, b, len + 8);
}

static uint8_t g_sig_id = 0x80;

static void spk_sig(int code, int id, const uint8_t *body, int blen)
{
    uint8_t b[64] = { (uint8_t)code, (uint8_t)id, (uint8_t)blen, 0 };
    memcpy(b + 4, body, (size_t)blen);
    spk_send(0x0001, b, 4 + blen);
}

static void spk_cfg_req(unsigned host_cid)
{
    uint8_t b[8];
    put16(b, host_cid);
    put16(b + 2, 0);
    b[4] = 0x01; b[5] = 2;
    put16(b + 6, 895);
    spk_sig(0x04, g_sig_id++, b, 8);
}

static void spk_av(const uint8_t *m, int len)
{
    spk_send(S.host_sig, m, len);
}

static void spk_av_cmd(int signal, const uint8_t *body, int blen)
{
    uint8_t m[32] = { (uint8_t)(S.av_label << 4), (uint8_t)signal };
    memcpy(m + 2, body, (size_t)blen);
    spk_av(m, 2 + blen);
}

/* The speaker as initiator, one step per answer */
static void spk_lead(void)
{
    static const uint8_t setcfg[] = { 0x04, 0x0C, 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x11, 0x15, 2, 53 };
    const uint8_t seid = 0x04;
    S.av_label = (S.av_label + 1) & 15;
    switch (S.av_step) {
    case 0: spk_av_cmd(0x01, NULL, 0); break;
    case 1: spk_av_cmd(0x02, &seid, 1); break;
    case 2: spk_av_cmd(0x03, setcfg, sizeof(setcfg)); break;
    case 3: spk_av_cmd(0x06, &seid, 1); break;
    case 4: {
        uint8_t b[4];
        put16(b, 0x0019);
        put16(b + 2, SPK_MEDIA);
        spk_sig(0x02, g_sig_id++, b, 4);
        break;
    }
    case 5: spk_av_cmd(0x07, &seid, 1); break;
    default: break;
    }
}

static void spk_channel_ready(unsigned scid)
{
    if (scid == SPK_SIG && S.sig_cfg_in && S.sig_cfg_out && !S.sig_open) {
        S.sig_open = 1;
        if (S.leads) spk_lead();
    }
    if (scid == SPK_MEDIA && S.media_cfg_in && S.media_cfg_out && !S.media_open) {
        S.media_open = 1;
        if (S.leads) {
            S.av_step = 5;
            spk_lead();
        }
    }
}

static void spk_on_signaling(const uint8_t *d, int len)
{
    int code = d[0], id = d[1];
    const uint8_t *b = d + 4;
    (void)len;
    uint8_t r[16];
    switch (code) {
    case 0x02: {                                /* the host opens a channel */
        unsigned psm = le16(b), scid = le16(b + 2);
        unsigned mine = psm == 0x0001 ? 0x0072 : !S.host_sig ? SPK_SIG : SPK_MEDIA;
        if (psm == 0x0019 && mine == SPK_SIG) S.host_sig = scid;
        else if (psm == 0x0019) S.host_media = scid;
        put16(r, mine); put16(r + 2, scid); put16(r + 4, 0); put16(r + 6, 0);
        spk_sig(0x03, id, r, 8);
        if (psm == 0x0019) spk_cfg_req(scid);
        break;
    }
    case 0x03: {                                /* the host accepted our channel */
        unsigned dcid = le16(b), scid = le16(b + 2);
        if (scid == SPK_SIG) S.host_sig = dcid;
        else S.host_media = dcid;
        spk_cfg_req(dcid);
        break;
    }
    case 0x04: {                                /* the host's configuration: accepted */
        unsigned dcid = le16(b);
        unsigned host = dcid == SPK_SIG ? S.host_sig : S.host_media;
        put16(r, host); put16(r + 2, 0); put16(r + 4, 0);
        spk_sig(0x05, id, r, 6);
        if (dcid == SPK_SIG) S.sig_cfg_in = 1;
        if (dcid == SPK_MEDIA) S.media_cfg_in = 1;
        spk_channel_ready(dcid);
        break;
    }
    case 0x05: {                                /* the host accepted our configuration */
        unsigned scid = le16(b);
        if (scid == SPK_SIG) S.sig_cfg_out = 1;
        if (scid == SPK_MEDIA) S.media_cfg_out = 1;
        spk_channel_ready(scid);
        break;
    }
    default:
        break;
    }
}

static void spk_on_avdtp(const uint8_t *d, int len)
{
    int label = d[0] >> 4, type = d[0] & 3, signal = d[1] & 0x3F;
    uint8_t m[32] = { (uint8_t)(label << 4 | 2), (uint8_t)signal };
    if (type != 0) {                            /* an answer to the speaker's own command */
        if (type != 2) {
            S.media_bad++;
            return;
        }
        if (signal == 0x03) S.configured = 1;
        if (signal == 0x07) S.started = 1;
        S.av_step++;
        if (S.av_step <= 4) spk_lead();
        return;
    }
    switch (signal) {
    case 0x01:                                  /* DISCOVER: an audio sink, seid 1 */
        m[2] = 0x04; m[3] = 0x08;
        spk_av(m, 4);
        break;
    case 0x02: case 0x0C: {                     /* capabilities: SBC, everything */
        static const uint8_t caps[] = { 0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0xFF, 0xFF, 0x02, 0x35 };
        memcpy(m + 2, caps, sizeof(caps));
        spk_av(m, 2 + (int)sizeof(caps));
        break;
    }
    case 0x03:
        S.configured = 1;
        spk_av(m, 2);
        break;
    case 0x07:
        S.started = 1;
        S.starts++;
        spk_av(m, 2);
        break;
    default:
        spk_av(m, 2);
        break;
    }
    (void)len;
}

static void spk_on_media(const uint8_t *d, int len)
{
    S.media_pkts++;
    if (len < 14 || d[0] != 0x80 || (d[1] & 0x7F) != 96) {
        S.media_bad++;
        return;
    }
    int seq = d[2] << 8 | d[3];
    if (S.last_seq >= 0 && seq != ((S.last_seq + 1) & 0xFFFF)) S.media_bad++;
    S.last_seq = seq;
    int nf = d[12] & 0x0F;
    if (d[13] != 0x9C) S.media_bad++;
    S.frames += (unsigned)nf;
    if (S.suspend_after && (int)S.media_pkts == S.suspend_after && !S.suspended_once) {
        S.suspended_once = 1;
        S.started = 0;
        uint8_t seid = 0x04;
        S.av_label = (S.av_label + 1) & 15;
        spk_av_cmd(0x09, &seid, 1);
    }
}

static void spk_on_frame(const uint8_t *f, int len)
{
    unsigned l2len = le16(f), cid = le16(f + 2);
    if ((int)l2len != len - 4) {
        S.media_bad++;
        return;
    }
    if (cid == 0x0001) spk_on_signaling(f + 4, (int)l2len);
    else if (cid == SPK_SIG) spk_on_avdtp(f + 4, (int)l2len);
    else if (cid == SPK_MEDIA) spk_on_media(f + 4, (int)l2len);
    else if (cid == 0x0072) S.sdp_asked++;
}

/* ---- the simulated adapter: hci_ops ---- */

static int op_cmd(void *ctx, unsigned op, const void *params, int plen)
{
    (void)ctx;
    (void)plen;
    const uint8_t *p = params;
    uint8_t r[64] = { 0 };
    switch (op) {
    case 0x1005:
        r[0] = 0;
        put16(r + 1, (unsigned)S.acl_mtu);
        r[3] = 64;
        put16(r + 4, (unsigned)S.acl_bufs);
        put16(r + 6, 8);
        cc(op, r, 8);
        break;
    case 0x0401: {                              /* inquiry: one speaker with its name in EIR */
        cs(op, 0);
        uint8_t e[255] = { 1 };
        memcpy(e + 1, S.addr, 6);
        e[7] = 1; e[8] = 0;
        e[9] = 0x14; e[10] = 0x04; e[11] = 0x24;     /* audio service, loudspeaker */
        e[12] = 0x34; e[13] = 0x12;
        e[14] = (uint8_t)-50;
        const char *name = "Test Speaker";
        e[15] = (uint8_t)(strlen(name) + 1);
        e[16] = 0x09;
        memcpy(e + 17, name, strlen(name));
        event(0x2F, e, 255);
        uint8_t done = 0;
        event(0x01, &done, 1);
        break;
    }
    case 0x0405:                                /* call the speaker */
        cs(op, 0);
        if (memcmp(p, S.addr, 6) != 0 || S.off) conn_complete(0x04);
        else conn_complete(0);
        break;
    case 0x0409:
        cs(op, 0);
        conn_complete(0);
        break;
    case 0x0411:                                /* authentication: the controller asks for the key */
        cs(op, 0);
        event(0x17, S.addr, 6);
        break;
    case 0x040B:                                /* a stored key */
        cc(op, p, 6);
        if (S.has_key && memcmp(p + 6, S.key, 16) == 0) {
            auth_complete(0);
        } else {
            auth_complete(0x06);
        }
        break;
    case 0x040C:                                /* no key: Secure Simple Pairing */
        cc(op, p, 6);
        event(0x31, S.addr, 6);
        break;
    case 0x042B: {
        cc(op, p, 6);
        uint8_t e[10];
        memcpy(e, S.addr, 6);
        e[6] = 3; e[7] = 0; e[8] = 0;
        event(0x32, e, 9);
        memcpy(e, S.addr, 6);
        put32(e + 6, 123456);
        event(0x33, e, 10);
        break;
    }
    case 0x042C: {
        cc(op, p, 6);
        uint8_t e[23] = { 0 };
        memcpy(e + 1, S.addr, 6);
        event(0x36, e, 7);
        for (int i = 0; i < 16; i++) S.key[i] = (uint8_t)(0xA0 + i);
        S.has_key = 1;
        memcpy(e, S.addr, 6);
        memcpy(e + 6, S.key, 16);
        e[22] = 4;
        event(0x18, e, 23);
        auth_complete(0);
        break;
    }
    case 0x0413: {
        cs(op, 0);
        uint8_t e[4] = { 0, HANDLE & 0xFF, HANDLE >> 8, 1 };
        event(0x08, e, 4);
        break;
    }
    case 0x0406: {
        cs(op, 0);
        uint8_t e[4] = { 0, HANDLE & 0xFF, HANDLE >> 8, 0x16 };
        S.connected = 0;
        event(0x05, e, 4);
        break;
    }
    case 0x0419: {
        cs(op, 0);
        uint8_t e[255] = { 0 };
        memcpy(e + 1, p, 6);
        strcpy((char *)e + 7, "Named Later");
        event(0x07, e, 255);
        break;
    }
    default:
        cc(op, r, 1);
        break;
    }
    return 1;
}

static int op_acl_send(void *ctx, const unsigned char *pkt, int len)
{
    (void)ctx;
    unsigned hdr = le16(pkt), dlen = le16(pkt + 2);
    if ((int)dlen != len - 4) S.media_bad++;
    if ((int)dlen > S.acl_mtu) S.frag_over++;
    if ((int)dlen > S.max_frag) S.max_frag = (int)dlen;
    if (++S.inflight > S.acl_bufs) S.buf_over++;
    /* The packet leaves the adapter at once: its buffer comes back */
    uint8_t e[5] = { 1, HANDLE & 0xFF, HANDLE >> 8, 1, 0 };
    S.inflight--;
    event(0x13, e, 5);
    int pb = (int)(hdr >> 12) & 3;
    if (pb == 2) {
        S.rx_need = 4 + (int)le16(pkt + 4);
        S.rx_len = 0;
    }
    if (S.rx_len + (int)dlen > (int)sizeof(S.rx)) return 1;
    memcpy(S.rx + S.rx_len, pkt + 4, dlen);
    S.rx_len += (int)dlen;
    if (S.rx_need && S.rx_len == S.rx_need) {
        spk_on_frame(S.rx, S.rx_len);
        S.rx_need = 0;
    }
    return 1;
}

static int op_pump(void *ctx, int timeout_ms)
{
    (void)ctx;
    if (!S.ev_n && !S.acl_n && timeout_ms > 0) usleep(1000);
    return S.ev_n || S.acl_n;
}

static int op_next_event(void *ctx, unsigned char *out, int max)
{
    (void)ctx;
    return q_pop(S.ev, &S.ev_h, &S.ev_n, out, max);
}

static int op_next_acl(void *ctx, unsigned char *out, int max)
{
    (void)ctx;
    return q_pop(S.acl, &S.acl_h, &S.acl_n, out, max);
}

static const hci_ops k_sim = { op_cmd, op_acl_send, op_pump, op_next_event, op_next_acl };

static void sim_reset(int acl_mtu, int acl_bufs)
{
    memset(&S, 0, sizeof(S));
    S.acl_mtu = acl_mtu;
    S.acl_bufs = acl_bufs;
    S.last_seq = -1;
    store_addr_parse("00:11:22:33:44:55", S.addr);
}

static int g_found;
static char g_found_name[64];

static void on_found(void *ud, const uint8_t addr[6], const char *name, uint32_t cod, int rssi, int audio)
{
    (void)ud; (void)addr; (void)cod; (void)rssi;
    if (audio) g_found++;
    snprintf(g_found_name, sizeof(g_found_name), "%s", name);
}

static host_t *open_host(void)
{
    hci_t t = { &k_sim, NULL };
    host_events ev = { on_found, NULL, NULL, NULL };
    return host_open(t, &ev, 0);
}

/* Polls until the host reaches `state` or ms pass. Returns 1 if it did. */
static int run_until(host_t *h, int state, int ms)
{
    long end = now_ms() + ms;
    host_status_t s;
    while (now_ms() < end) {
        host_poll(h, 1);
        host_status(h, &s);
        if (s.state == state) return 1;
    }
    return 0;
}

static int send_media(void *ctx, const uint8_t *pkt, int len)
{
    return host_send_media((host_t *)ctx, pkt, len);
}

static void stream_for(host_t *h, int ms)
{
    host_status_t s;
    host_status(h, &s);
    stream_start(&s.cfg, s.mtu);
    int16_t pcm[2 * 480];
    double ph = 0;
    long end = now_ms() + ms, fed = now_ms();
    while (now_ms() < end) {
        while (now_ms() - fed >= 10) {
            for (int i = 0; i < 480; i++) {
                pcm[2 * i] = pcm[2 * i + 1] = (int16_t)(9000 * sin(ph));
                ph += 2 * 3.14159265 * 440 / 48000;
            }
            stream_feed(pcm, 480);
            fed += 10;
        }
        host_poll(h, stream_wait_ms(2));
        stream_tick(send_media, h);
    }
    stream_stop();
}

static void test_host_connect_freshSpeaker_pairsAndStreams(void)
{
    /* arrange: a Realtek-like adapter (8 buffers of 1021 bytes) and a speaker never paired */
    unlink("build/host/sim_store.txt");
    store_init("build/host/sim_store.txt");
    sim_reset(1021, 8);
    host_t *h = open_host();
    CHECK(h != NULL, "the host starts on the simulated adapter");
    if (!h) return;

    /* act */
    host_scan(h, 3);
    host_poll(h, 1);
    host_poll(h, 1);
    host_connect(h, S.addr, NULL);
    int streaming = run_until(h, HOST_STREAMING, 3000);

    /* assert */
    host_status_t s;
    host_status(h, &s);
    CHECK(g_found == 1 && strcmp(g_found_name, "Test Speaker") == 0, "the search reports the speaker by its EIR name");
    CHECK(streaming, "the stream reaches Streaming, state %s, error \"%s\"", host_state_name(s.state), s.last_error);
    CHECK(strcmp(s.name, "Test Speaker") == 0, "the link carries the name found in the search");
    uint8_t key[16];
    CHECK(store_get_key(S.addr, key) && memcmp(key, S.key, 16) == 0, "the link key from pairing is stored");
    CHECK(sbc_rate(&s.cfg) == 48000 && s.cfg.max_bitpool == 53, "48 kHz, bitpool 53");
    CHECK(s.mtu == 895, "the speaker's media MTU is used, got %d", s.mtu);

    stream_for(h, 400);
    CHECK(S.media_pkts >= 15 && S.media_pkts <= 25, "about 400 ms / 18.7 ms packets reach the speaker, got %u", S.media_pkts);
    CHECK(S.media_bad == 0, "every packet is well formed and in sequence (%u bad)", S.media_bad);
    CHECK(S.frag_over == 0 && S.buf_over == 0, "fragments fit the adapter's buffers");
    host_close(h);
}

static void test_host_media_csrBuffers_fragmented(void)
{
    /* arrange: a CSR8510-like adapter, 10 buffers of 310 bytes, speaker already paired */
    sim_reset(310, 10);
    for (int i = 0; i < 16; i++) S.key[i] = (uint8_t)(0xA0 + i);
    S.has_key = 1;
    host_t *h = open_host();
    if (!h) return;
    host_connect(h, S.addr, "Paired");
    int streaming = run_until(h, HOST_STREAMING, 3000);
    CHECK(streaming, "a paired speaker streams with its stored key");

    /* act */
    stream_for(h, 300);

    /* assert */
    CHECK(S.media_pkts >= 10, "packets reach the speaker, got %u", S.media_pkts);
    CHECK(S.max_frag <= 310 && S.frag_over == 0, "no ACL packet is bigger than 310 bytes, max %d", S.max_frag);
    CHECK(S.buf_over == 0, "never more packets in flight than the adapter's buffers");
    CHECK(S.media_bad == 0, "the fragments join into whole packets (%u bad)", S.media_bad);
    host_close(h);
}

static void test_host_incoming_speakerLeads_streams(void)
{
    /* arrange: the stored speaker is switched on and calls us, then leads AVDTP itself */
    sim_reset(1021, 8);
    S.leads = 1;
    for (int i = 0; i < 16; i++) S.key[i] = (uint8_t)(0xA0 + i);
    S.has_key = 1;
    host_t *h = open_host();
    if (!h) return;
    uint8_t req[10];
    memcpy(req, S.addr, 6);
    req[6] = 0x14; req[7] = 0x04; req[8] = 0x24; req[9] = 1;

    /* act: it calls, authenticates and encrypts, then opens AVDTP */
    event(0x04, req, 10);
    run_until(h, HOST_SECURING, 500);
    event(0x17, S.addr, 6);
    uint8_t enc[4] = { 0, HANDLE & 0xFF, HANDLE >> 8, 1 };
    event(0x08, enc, 4);
    uint8_t b[4];
    put16(b, 0x0019);
    put16(b + 2, SPK_SIG);
    spk_sig(0x02, g_sig_id++, b, 4);
    int streaming = run_until(h, HOST_STREAMING, 3000);

    /* assert */
    host_status_t s;
    host_status(h, &s);
    CHECK(streaming, "the stream reaches Streaming with the speaker leading, state %s, error \"%s\"",
          host_state_name(s.state), s.last_error);
    CHECK(s.incoming, "the link is marked as the speaker's call");
    CHECK(S.configured && S.started, "the speaker configured and started our end point");
    CHECK(strcmp(s.name, "Test Speaker") == 0, "the stored name is used, got \"%s\"", s.name);
    host_close(h);
}

static void test_host_connect_speakerForgotKey_pairsAgain(void)
{
    /* arrange: we hold a key the speaker no longer has */
    sim_reset(1021, 8);
    S.has_key = 0;
    host_t *h = open_host();
    if (!h) return;

    /* act */
    host_connect(h, S.addr, "Paired");
    int streaming = run_until(h, HOST_STREAMING, 3000);

    /* assert */
    uint8_t key[16];
    CHECK(streaming, "after the stored key fails, pairing again reaches Streaming");
    CHECK(store_get_key(S.addr, key) && memcmp(key, S.key, 16) == 0, "the new key replaces the old one");
    host_close(h);
}

static void test_host_connect_speakerOff_errorAndIdle(void)
{
    /* arrange */
    sim_reset(1021, 8);
    S.off = 1;
    host_t *h = open_host();
    if (!h) return;

    /* act */
    host_connect(h, S.addr, "Off Speaker");
    run_until(h, -1, 200);

    /* assert */
    host_status_t s;
    host_status(h, &s);
    CHECK(s.state == HOST_IDLE, "the host is idle again");
    CHECK(strstr(s.last_error, "did not answer") != NULL, "the error says the speaker did not answer: \"%s\"", s.last_error);
    host_close(h);
}

static void test_host_suspendBySpeaker_resumes(void)
{
    /* arrange: the speaker suspends after 5 packets */
    sim_reset(1021, 8);
    for (int i = 0; i < 16; i++) S.key[i] = (uint8_t)(0xA0 + i);
    S.has_key = 1;
    S.suspend_after = 5;
    host_t *h = open_host();
    if (!h) return;
    host_connect(h, S.addr, "Paired");
    run_until(h, HOST_STREAMING, 3000);

    /* act */
    stream_for(h, 300);
    int ready = run_until(h, HOST_READY, 200);
    int back = run_until(h, HOST_STREAMING, 4000);

    /* assert */
    CHECK(S.suspended_once, "the speaker suspended the stream");
    CHECK(ready || back, "the host took the suspension");
    CHECK(back && S.starts >= 2, "the host starts the stream again, starts %d", S.starts);
    host_close(h);
}

int main(void)
{
    test_host_connect_freshSpeaker_pairsAndStreams();
    test_host_media_csrBuffers_fragmented();
    test_host_incoming_speakerLeads_streams();
    test_host_connect_speakerForgotKey_pairsAgain();
    test_host_connect_speakerOff_errorAndIdle();
    test_host_suspendBySpeaker_resumes();
    return REPORT("test_host_sim");
}
