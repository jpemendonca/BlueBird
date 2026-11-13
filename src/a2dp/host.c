#include "host.h"
#include "sdp.h"
#include "store.h"
#include "log.h"
#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HCI opcodes: OGF << 10 | OCF */
#define OP_INQUIRY              0x0401
#define OP_INQUIRY_CANCEL       0x0402
#define OP_CREATE_CONNECTION    0x0405
#define OP_DISCONNECT           0x0406
#define OP_CREATE_CONN_CANCEL   0x0408
#define OP_ACCEPT_CONNECTION    0x0409
#define OP_REJECT_CONNECTION    0x040A
#define OP_LINK_KEY_REPLY       0x040B
#define OP_LINK_KEY_NEG_REPLY   0x040C
#define OP_PIN_CODE_REPLY       0x040D
#define OP_AUTH_REQUESTED       0x0411
#define OP_SET_ENCRYPTION       0x0413
#define OP_REMOTE_NAME          0x0419
#define OP_REMOTE_NAME_CANCEL   0x041A
#define OP_IO_CAP_REPLY         0x042B
#define OP_USER_CONFIRM_REPLY   0x042C
#define OP_USER_PASSKEY_NEG     0x042F
#define OP_WRITE_LINK_POLICY_DEF 0x080F
#define OP_SET_EVENT_MASK       0x0C01
#define OP_WRITE_PAGE_TIMEOUT   0x0C18
#define OP_WRITE_SCAN_ENABLE    0x0C1A
#define OP_WRITE_INQUIRY_MODE   0x0C45
#define OP_WRITE_SSP_MODE       0x0C56
#define OP_READ_BUFFER_SIZE     0x1005

/* L2CAP */
#define CID_SIGNALING   0x0001
#define CID_AV_SIG      0x0040
#define CID_AV_MEDIA    0x0041
#define CID_SDP_SRV     0x0042
#define SIG_REJECT      0x01
#define SIG_CONN_REQ    0x02
#define SIG_CONN_RSP    0x03
#define SIG_CFG_REQ     0x04
#define SIG_CFG_RSP     0x05
#define SIG_DISC_REQ    0x06
#define SIG_DISC_RSP    0x07
#define SIG_ECHO_REQ    0x08
#define SIG_ECHO_RSP    0x09
#define SIG_INFO_REQ    0x0A
#define SIG_INFO_RSP    0x0B
#define OUR_MTU         1024

/* Timing (ms) */
#define T_PAGE          15000   /* calling a speaker */
#define T_SETUP         30000   /* link up to stream open */
#define T_PEER_FIRST     1500   /* a speaker that called us gets this long to start pairing or AVDTP itself */
#define T_REPLY          3000   /* an AVDTP or L2CAP request without an answer */
#define T_START_WAIT     1000   /* the sink opened the media channel: its own START gets this long */
#define T_RESUME         2500   /* after the sink suspended the stream */
#define T_REOPEN         2000   /* AVDTP closed with the link up: set it up again */
#define T_DISC           3000   /* our disconnect without its Disconnection Complete */
#define T_NO_CREDITS     3000   /* no Number Of Completed Packets for this long: take the buffers as free */
#define T_NAME           6000

#define TXQ_LEN          16
#define TX_MAX           (OUR_MTU + 8)
#define FOUND_MAX        24
#define SEP_MAX          8

typedef enum { CH_CLOSED, CH_CONNECTING, CH_CONFIG, CH_OPEN } chan_state;

typedef struct {
    chan_state st;
    unsigned lcid, rcid;
    int  ours_ok, theirs_ok, outgoing;
    int  out_mtu;           /* the most we may send on it: the peer's MTU */
    int  tries;
    long t;
} chan_t;

typedef struct {
    int      used;
    int      state;
    uint8_t  addr[6];
    char     name[64];
    unsigned handle;
    int      connected, incoming, disconnecting;
    long     t_start, t_conn, t_disc, t_enc;
    int      auth_sent, auth_ok, enc_sent, enc_on, auth_retry, key_used, peer_secured;

    chan_t   sig, media, sdp;
    uint8_t  rx[2048];
    int      rx_len, rx_need;

    /* AVDTP: our command waiting for its answer */
    int      label, pending, pending_label, pending_tries;
    uint8_t  pending_msg[64];
    int      pending_len;
    long     t_pending;
    avdtp_sep_t seps[SEP_MAX];
    int      nsep, isep;
    int      peer_spoke;            /* the sink sent an AVDTP command: it leads the set-up */
    long     t_sig_open;
    int      remote_seid, configured, opened, we_opened, start_tries, bitpool_retry, start_pending;
    sbc_choice_t cfg;
    long     start_at, resume_at, reopen_at;
} link_t;

typedef struct {
    uint8_t  addr[6];
    uint8_t  psrm;
    uint16_t clk;
    uint32_t cod;
    int      rssi, named, name_asked;
    char     name[64];
} found_t;

struct host {
    hci_t    hci;
    host_events ev;
    int      fake_csr, dead;
    int      acl_mtu, acl_max, credits;
    long     t_no_credits;
    uint8_t  sig_id;

    int      scanning, inquiry_mode;
    found_t  found[FOUND_MAX];
    int      nfound, heard;
    int      name_busy;
    uint8_t  name_addr[6];
    long     t_name;

    link_t   l;

    uint8_t  txq[TXQ_LEN][TX_MAX];
    int      txlen[TXQ_LEN];
    int      tx_head, tx_n;

    unsigned media_sent, media_dropped;
    unsigned pairings;              /* link keys received: new pairings */
    char     last_error[96];
};

static const char *addr_s(const uint8_t *a)
{
    static char buf[4][18];
    static int i;
    i = (i + 1) & 3;
    store_addr_str(a, buf[i]);
    return buf[i];
}

const char *host_state_name(int state)
{
    switch (state) {
    case HOST_IDLE:        return "idle";
    case HOST_PAGING:      return "calling";
    case HOST_SECURING:    return "pairing";
    case HOST_SIGNALING:   return "opening audio";
    case HOST_CONFIGURING: return "choosing codec";
    case HOST_OPENING:     return "opening stream";
    case HOST_READY:       return "ready";
    case HOST_STREAMING:   return "streaming";
    default:               return "?";
    }
}

static void set_error(host_t *h, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void set_error(host_t *h, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(h->last_error, sizeof(h->last_error), fmt, ap);
    va_end(ap);
    log_line("host: %s", h->last_error);
}

static void set_state(host_t *h, int st)
{
    if (h->l.state == st) return;
    log_line("host: %s -> %s", host_state_name(h->l.state), host_state_name(st));
    h->l.state = st;
    if (st == HOST_STREAMING) h->last_error[0] = 0;
    if (h->ev.on_state) h->ev.on_state(h->ev.ud, st);
}

static int cmd(host_t *h, unsigned op, const void *p, int len)
{
    if (h->dead) return 0;
    if (!h->hci.ops->cmd(h->hci.ctx, op, p, len)) {
        h->dead = 1;
        return 0;
    }
    return 1;
}

/* For set-up only: sends a command and waits for its Command Complete. Other events are dropped. */
static int sync_cmd(host_t *h, unsigned op, const void *p, int len, uint8_t *reply, int max)
{
    if (!cmd(h, op, p, len)) return -1;
    long deadline = now_ms() + 2000;
    uint8_t ev[HCI_PKT_MAX];
    while (now_ms() < deadline) {
        if (h->hci.ops->pump(h->hci.ctx, 20) < 0) {
            h->dead = 1;
            return -1;
        }
        int n;
        while ((n = h->hci.ops->next_event(h->hci.ctx, ev, sizeof(ev))) > 0) {
            if (ev[0] == 0x0E && n >= 6 && le16(ev + 3) == op) {
                int r = n - 5 < max ? n - 5 : max;
                if (reply && r > 0) memcpy(reply, ev + 5, (size_t)r);
                return r;
            }
        }
    }
    log_line("host: no answer to command 0x%04x during set-up", op);
    return -1;
}

/* ---- ACL out ------------------------------------------------------------- */

static int frags_for(const host_t *h, int len)
{
    return (len + h->acl_mtu - 1) / h->acl_mtu;
}

/* Sends one L2CAP frame (basic header included) in ACL fragments; the caller checked credits */
static int acl_write(host_t *h, const uint8_t *frame, int flen)
{
    uint8_t pkt[HCI_PKT_MAX];
    for (int off = 0; off < flen; off += h->acl_mtu) {
        int n = flen - off < h->acl_mtu ? flen - off : h->acl_mtu;
        unsigned hdr = h->l.handle | (off == 0 ? 0x2000u : 0x1000u);
        put16(pkt, hdr);
        put16(pkt + 2, (unsigned)n);
        memcpy(pkt + 4, frame + off, (size_t)n);
        if (!h->hci.ops->acl_send(h->hci.ctx, pkt, 4 + n)) return 0;
        h->credits--;
        if (h->credits == 0) h->t_no_credits = now_ms();
    }
    return 1;
}

static void tx_flush(host_t *h)
{
    while (h->tx_n && h->l.connected) {
        int len = h->txlen[h->tx_head];
        if (h->credits < frags_for(h, len)) return;
        acl_write(h, h->txq[h->tx_head], len);
        h->tx_head = (h->tx_head + 1) % TXQ_LEN;
        h->tx_n--;
    }
}

/* Signaling, AVDTP and SDP frames wait in order for buffers; none is dropped */
static void l2_send(host_t *h, unsigned cid, const uint8_t *d, int len)
{
    if (!h->l.connected || len + 4 > TX_MAX) return;
    if (h->tx_n == TXQ_LEN) {
        log_line("host: send queue full, frame for cid 0x%04x dropped", cid);
        return;
    }
    int i = (h->tx_head + h->tx_n) % TXQ_LEN;
    put16(h->txq[i], (unsigned)len);
    put16(h->txq[i] + 2, cid);
    memcpy(h->txq[i] + 4, d, (size_t)len);
    h->txlen[i] = len + 4;
    h->tx_n++;
    tx_flush(h);
}

static void sig_send(host_t *h, int code, int id, const uint8_t *body, int blen)
{
    uint8_t b[64];
    b[0] = (uint8_t)code;
    b[1] = (uint8_t)id;
    put16(b + 2, (unsigned)blen);
    if (blen) memcpy(b + 4, body, (size_t)blen);
    l2_send(h, CID_SIGNALING, b, 4 + blen);
}

static int next_id(host_t *h)
{
    if (++h->sig_id == 0) h->sig_id = 1;
    return h->sig_id;
}

/* ---- channels -------------------------------------------------------------- */

static chan_t *chan_by_lcid(link_t *l, unsigned lcid)
{
    if (l->sig.st != CH_CLOSED && l->sig.lcid == lcid) return &l->sig;
    if (l->media.st != CH_CLOSED && l->media.lcid == lcid) return &l->media;
    if (l->sdp.st != CH_CLOSED && l->sdp.lcid == lcid) return &l->sdp;
    return NULL;
}

static void chan_reset(chan_t *c, unsigned lcid)
{
    memset(c, 0, sizeof(*c));
    c->lcid = lcid;
    c->out_mtu = 672;
}

static void send_cfg_req(host_t *h, chan_t *c)
{
    uint8_t b[8];
    put16(b, c->rcid);
    put16(b + 2, 0);
    b[4] = 0x01;            /* MTU */
    b[5] = 2;
    put16(b + 6, OUR_MTU);
    sig_send(h, SIG_CFG_REQ, next_id(h), b, 8);
    c->t = now_ms();
    c->tries++;
}

static void chan_connect(host_t *h, chan_t *c, unsigned lcid)
{
    uint8_t b[4];
    chan_reset(c, lcid);
    c->st = CH_CONNECTING;
    c->outgoing = 1;
    c->t = now_ms();
    put16(b, PSM_AVDTP);
    put16(b + 2, lcid);
    sig_send(h, SIG_CONN_REQ, next_id(h), b, 4);
}

static void chan_disconnect(host_t *h, chan_t *c)
{
    if (c->st == CH_CLOSED) return;
    if (c->rcid) {
        uint8_t b[4];
        put16(b, c->rcid);
        put16(b + 2, c->lcid);
        sig_send(h, SIG_DISC_REQ, next_id(h), b, 4);
    }
    c->st = CH_CLOSED;
}

/* ---- AVDTP ------------------------------------------------------------------ */

static void av_send(host_t *h, const uint8_t *msg, int len)
{
    if (h->l.sig.st != CH_OPEN) return;
    l2_send(h, h->l.sig.rcid, msg, len);
}

static void av_command(host_t *h, int signal, const uint8_t *msg, int len)
{
    link_t *l = &h->l;
    l->pending = signal;
    l->pending_label = msg[0] >> 4;
    l->pending_tries = 0;
    l->t_pending = now_ms();
    memcpy(l->pending_msg, msg, (size_t)len);
    l->pending_len = len;
    av_send(h, msg, len);
}

static int take_label(link_t *l)
{
    int lb = l->label;
    l->label = (l->label + 1) & 0x0F;
    return lb;
}

static void av_simple(host_t *h, int signal, int seid)
{
    uint8_t m[8];
    int n = avdtp_build_cmd(m, take_label(&h->l), signal, seid);
    av_command(h, signal, m, n);
}

static void link_disconnect(host_t *h, const char *why);

static void ask_next_sep(host_t *h)
{
    link_t *l = &h->l;
    while (l->isep < l->nsep) {
        const avdtp_sep_t *s = &l->seps[l->isep];
        if (s->tsep == 1 && s->media == 0 && !s->in_use) {
            av_simple(h, AVDTP_GET_CAPABILITIES, s->seid);
            return;
        }
        l->isep++;
    }
    set_error(h, "%s offers no free audio sink to play on", l->name[0] ? l->name : addr_s(l->addr));
    link_disconnect(h, "no usable audio sink");
}

static void start_discover(host_t *h)
{
    link_t *l = &h->l;
    if (l->configured || l->pending) return;
    set_state(h, HOST_CONFIGURING);
    l->nsep = l->isep = 0;
    av_simple(h, AVDTP_DISCOVER, 0);
}

static void send_set_config(host_t *h)
{
    link_t *l = &h->l;
    uint8_t m[32];
    int n = avdtp_build_set_config(m, take_label(l), l->remote_seid, AVDTP_OUR_SEID, &l->cfg);
    av_command(h, AVDTP_SET_CONFIGURATION, m, n);
}

static void log_cfg(const char *who, const sbc_choice_t *c)
{
    static const char *const modes[] = { "", "joint stereo", "stereo", "", "dual", "", "", "", "mono" };
    log_line("host: %s SBC %d Hz, %s, %d blocks, %d subbands, %s, bitpool %u-%u", who, sbc_rate(c),
             c->mode <= 8 ? modes[c->mode] : "?", sbc_blocks(c), sbc_subbands(c),
             c->alloc == 0x02 ? "SNR" : "loudness", c->min_bitpool, c->max_bitpool);
}

static void stream_opened(host_t *h)
{
    link_t *l = &h->l;
    l->start_tries = 0;
    if (l->start_pending) {
        /* The sink sent START before its media channel was open */
        l->start_pending = 0;
        l->start_at = 0;
        set_state(h, HOST_STREAMING);
        return;
    }
    set_state(h, HOST_READY);
    /* The side that opened the stream normally starts it; a sink that opened it gets a moment first */
    l->start_at = now_ms() + (l->we_opened ? 0 : T_START_WAIT);
}

/* An answer to our command */
static void av_on_response(host_t *h, int msg_type, int signal, const uint8_t *d, int len)
{
    link_t *l = &h->l;
    int accepted = msg_type == AVDTP_MSG_ACCEPT;
    l->pending = 0;
    switch (signal) {
    case AVDTP_DISCOVER:
        if (!accepted) {
            set_error(h, "the speaker refused to list its audio end points");
            link_disconnect(h, "discover refused");
            return;
        }
        l->nsep = avdtp_parse_discover(d, len, l->seps, SEP_MAX);
        l->isep = 0;
        for (int i = 0; i < l->nsep; i++)
            log_line("host: sink end point %d: %s %s%s", l->seps[i].seid, l->seps[i].media == 0 ? "audio" : "other",
                     l->seps[i].tsep ? "sink" : "source", l->seps[i].in_use ? " (in use)" : "");
        if (!l->configured) ask_next_sep(h);
        return;
    case AVDTP_GET_CAPABILITIES: {
        uint8_t ie[4];
        if (l->configured) return;
        if (accepted && avdtp_find_sbc(d, len, ie, NULL) && sbc_choose(ie, SBC_BITPOOL_HQ, &l->cfg)) {
            log_line("host: sink %d supports SBC %02x %02x bitpool %u-%u", l->seps[l->isep].seid, ie[0], ie[1], ie[2], ie[3]);
            l->remote_seid = l->seps[l->isep].seid;
            l->bitpool_retry = 0;
            send_set_config(h);
            return;
        }
        l->isep++;
        ask_next_sep(h);
        return;
    }
    case AVDTP_SET_CONFIGURATION:
        if (l->configured) return;      /* the sink configured our end point itself meanwhile */
        if (accepted) {
            l->configured = 1;
            log_cfg("stream configured:", &l->cfg);
            set_state(h, HOST_OPENING);
            av_simple(h, AVDTP_OPEN, l->remote_seid);
            return;
        }
        log_line("host: SET_CONFIGURATION refused (%02x %02x)", len > 0 ? d[0] : 0, len > 1 ? d[1] : 0);
        if (!l->bitpool_retry && l->cfg.max_bitpool > 35) {
            /* Some sinks list a bitpool they then refuse */
            l->bitpool_retry = 1;
            l->cfg.max_bitpool = 35 < l->cfg.min_bitpool ? l->cfg.min_bitpool : 35;
            send_set_config(h);
            return;
        }
        l->isep++;
        ask_next_sep(h);
        return;
    case AVDTP_OPEN:
        if (!accepted) {
            set_error(h, "the speaker refused to open the stream");
            link_disconnect(h, "open refused");
            return;
        }
        l->opened = 1;
        l->we_opened = 1;
        chan_connect(h, &l->media, CID_AV_MEDIA);
        return;
    case AVDTP_START:
        if (accepted) {
            if (l->media.st == CH_OPEN) set_state(h, HOST_STREAMING);
            return;
        }
        log_line("host: START refused (%02x)", len > 0 ? d[0] : 0);
        if (++l->start_tries < 4) l->start_at = now_ms() + 1000;
        return;
    default:
        return;
    }
}

/* A command from the sink: we answer as the acceptor */
static void av_on_command(host_t *h, int label, int signal, const uint8_t *d, int len)
{
    link_t *l = &h->l;
    uint8_t m[64];
    int n;
    int seid = len > 0 ? d[0] >> 2 : 0;
    l->peer_spoke = 1;
    switch (signal) {
    case AVDTP_DISCOVER:
        n = avdtp_build_discover_rsp(m, label, l->configured);
        break;
    case AVDTP_GET_CAPABILITIES:
    case AVDTP_GET_ALL_CAPABILITIES:
        if (seid != AVDTP_OUR_SEID) n = avdtp_build_reject(m, label, signal, 0, 0x12);
        else n = avdtp_build_caps_rsp(m, label, signal);
        break;
    case AVDTP_SET_CONFIGURATION: {
        uint8_t ie[4];
        sbc_choice_t c;
        if (len < 2 || seid != AVDTP_OUR_SEID) {
            n = avdtp_build_reject(m, label, signal, 0, 0x12);
        } else if (l->configured) {
            n = avdtp_build_reject(m, label, signal, 0, 0x13);
        } else if (!avdtp_find_sbc(d + 2, len - 2, ie, NULL) || !sbc_from_ie(ie, &c)) {
            n = avdtp_build_reject(m, label, signal, 0x07, 0x29);
        } else {
            l->cfg = c;
            l->remote_seid = d[1] >> 2;
            l->configured = 1;
            if (l->pending == AVDTP_GET_CAPABILITIES || l->pending == AVDTP_SET_CONFIGURATION) l->pending = 0;
            log_cfg("the speaker configured", &l->cfg);
            set_state(h, HOST_OPENING);
            n = avdtp_build_accept(m, label, signal);
        }
        break;
    }
    case AVDTP_GET_CONFIGURATION:
        n = l->configured ? avdtp_build_config_rsp(m, label, &l->cfg) : avdtp_build_reject(m, label, signal, 0, 0x14);
        break;
    case AVDTP_RECONFIGURE: {
        uint8_t ie[4];
        sbc_choice_t c;
        if (len >= 1 && avdtp_find_sbc(d + 1, len - 1, ie, NULL) && sbc_from_ie(ie, &c)) {
            l->cfg = c;
            log_cfg("the speaker reconfigured", &l->cfg);
            n = avdtp_build_accept(m, label, signal);
        } else {
            n = avdtp_build_reject(m, label, signal, 0x07, 0x29);
        }
        break;
    }
    case AVDTP_OPEN:
        if (!l->configured) {
            n = avdtp_build_reject(m, label, signal, 0, 0x31);
        } else {
            l->opened = 1;
            l->we_opened = 0;
            n = avdtp_build_accept(m, label, signal);
        }
        break;
    case AVDTP_START:
        n = avdtp_build_accept(m, label, signal);
        av_send(h, m, n);
        l->start_at = 0;
        if (l->media.st == CH_OPEN) set_state(h, HOST_STREAMING);
        else l->start_pending = 1;
        return;
    case AVDTP_SUSPEND:
        n = avdtp_build_accept(m, label, signal);
        av_send(h, m, n);
        if (l->state == HOST_STREAMING) {
            log_line("host: the speaker suspended the stream; starting it again in %d ms", T_RESUME);
            set_state(h, HOST_READY);
            l->resume_at = now_ms() + T_RESUME;
        }
        return;
    case AVDTP_CLOSE:
    case AVDTP_ABORT:
        n = avdtp_build_accept(m, label, signal);
        av_send(h, m, n);
        log_line("host: the speaker %s the stream", signal == AVDTP_CLOSE ? "closed" : "aborted");
        l->configured = l->opened = 0;
        l->start_at = l->resume_at = 0;
        set_state(h, HOST_SIGNALING);
        l->reopen_at = now_ms() + T_REOPEN;
        return;
    case AVDTP_DELAYREPORT:
        if (len >= 3) log_line("host: the speaker reports a delay of %u.%u ms", (d[1] << 8 | d[2]) / 10, (d[1] << 8 | d[2]) % 10);
        n = avdtp_build_accept(m, label, signal);
        break;
    case AVDTP_SECURITY_CONTROL:
        n = avdtp_build_reject(m, label, signal, 0, 0x19);
        break;
    default:
        n = avdtp_build_general_reject(m, label, signal);
        break;
    }
    av_send(h, m, n);
}

static void av_on_frame(host_t *h, const uint8_t *d, int len)
{
    int label, type, signal;
    if (!avdtp_header(d, len, &label, &type, &signal)) return;
    if ((d[0] >> 2 & 0x03) != 0) {
        log_line("host: fragmented AVDTP message ignored");
        return;
    }
    if (type == AVDTP_MSG_CMD) {
        av_on_command(h, label, signal, d + 2, len - 2);
        return;
    }
    link_t *l = &h->l;
    if (l->pending && signal == l->pending && label == l->pending_label) {
        av_on_response(h, type, signal, d + 2, len - 2);
    }
}

/* ---- L2CAP signaling ---------------------------------------------------------- */

static void chan_open(host_t *h, chan_t *c)
{
    link_t *l = &h->l;
    c->st = CH_OPEN;
    if (c == &l->sig) {
        log_line("host: AVDTP signaling open (%s, MTU %d)", c->outgoing ? "we opened it" : "the speaker opened it", c->out_mtu);
        l->t_sig_open = now_ms();
        if (l->state < HOST_SIGNALING) set_state(h, HOST_SIGNALING);
        if (c->outgoing && !l->peer_spoke) start_discover(h);
    } else if (c == &l->media) {
        log_line("host: media channel open, the speaker takes packets of up to %d bytes", c->out_mtu);
        stream_opened(h);
    }
}

static void check_open(host_t *h, chan_t *c)
{
    if (c->st == CH_CONFIG && c->ours_ok && c->theirs_ok) chan_open(h, c);
}

static void on_conn_req(host_t *h, int id, const uint8_t *b, int blen)
{
    link_t *l = &h->l;
    if (blen < 4) return;
    unsigned psm = le16(b), scid = le16(b + 2);
    chan_t *c = NULL;
    unsigned lcid = 0;
    if (psm == PSM_AVDTP && l->sig.st == CH_CLOSED) {
        c = &l->sig;
        lcid = CID_AV_SIG;
    } else if (psm == PSM_AVDTP && l->sig.st == CH_OPEN && l->media.st == CH_CLOSED) {
        c = &l->media;
        lcid = CID_AV_MEDIA;
    } else if (psm == PSM_SDP && l->sdp.st == CH_CLOSED) {
        c = &l->sdp;
        lcid = CID_SDP_SRV;
    }
    uint8_t r[8];
    if (!c) {
        log_line("host: the speaker asked for a channel on PSM 0x%04x, refused", psm);
        put16(r, 0);
        put16(r + 2, scid);
        put16(r + 4, psm == PSM_AVDTP ? 0x0004 : 0x0002);    /* no resources / PSM not supported */
        put16(r + 6, 0);
        sig_send(h, SIG_CONN_RSP, id, r, 8);
        return;
    }
    chan_reset(c, lcid);
    c->rcid = scid;
    c->st = CH_CONFIG;
    c->t = now_ms();
    put16(r, lcid);
    put16(r + 2, scid);
    put16(r + 4, 0);
    put16(r + 6, 0);
    sig_send(h, SIG_CONN_RSP, id, r, 8);
    send_cfg_req(h, c);
}

static void on_conn_rsp(host_t *h, const uint8_t *b, int blen)
{
    if (blen < 8) return;
    unsigned dcid = le16(b), scid = le16(b + 2), result = le16(b + 4);
    chan_t *c = chan_by_lcid(&h->l, scid);
    if (!c || c->st != CH_CONNECTING) return;
    if (result == 1) {                      /* pending: the speaker is still deciding */
        c->t = now_ms();
        return;
    }
    if (result != 0) {
        c->st = CH_CLOSED;
        set_error(h, "the speaker refused the audio channel (result %u)", result);
        link_disconnect(h, "channel refused");
        return;
    }
    c->rcid = dcid;
    c->st = CH_CONFIG;
    c->tries = 0;
    send_cfg_req(h, c);
}

static void on_cfg_req(host_t *h, int id, const uint8_t *b, int blen)
{
    if (blen < 4) return;
    unsigned dcid = le16(b), flags = le16(b + 2);
    chan_t *c = chan_by_lcid(&h->l, dcid);
    uint8_t r[64];
    if (!c) {
        uint8_t rej[6];
        put16(rej, 0x0002);                 /* invalid CID */
        put16(rej + 2, dcid);
        put16(rej + 4, 0);
        sig_send(h, SIG_REJECT, id, rej, 6);
        return;
    }
    int result = 0, rlen = 6;
    for (int off = 4; off + 2 <= blen;) {
        int type = b[off], olen = b[off + 1];
        if (off + 2 + olen > blen) break;
        const uint8_t *v = b + off + 2;
        if ((type & 0x7F) == 0x01 && olen >= 2) {
            c->out_mtu = (int)le16(v);
        } else if ((type & 0x7F) == 0x04 && olen >= 1 && v[0] != 0) {
            /* Retransmission or streaming mode asked: only basic mode here */
            result = 1;
            if (rlen + 11 <= (int)sizeof(r)) {
                r[rlen++] = 0x04;
                r[rlen++] = 9;
                memset(r + rlen, 0, 9);
                rlen += 9;
            }
        } else if (!(type & 0x80) && (type & 0x7F) > 0x07) {
            result = 3;                     /* unknown option, not a hint */
            if (rlen + 2 + olen <= (int)sizeof(r)) {
                memcpy(r + rlen, b + off, (size_t)(2 + olen));
                rlen += 2 + olen;
            }
        }
        off += 2 + olen;
    }
    put16(r, c->rcid);
    put16(r + 2, flags & 1);
    put16(r + 4, (unsigned)result);
    sig_send(h, SIG_CFG_RSP, id, r, result ? rlen : 6);
    if (result == 0 && !(flags & 1)) {
        c->theirs_ok = 1;
        check_open(h, c);
    }
}

static void on_cfg_rsp(host_t *h, const uint8_t *b, int blen)
{
    if (blen < 6) return;
    unsigned scid = le16(b), result = le16(b + 4);
    chan_t *c = chan_by_lcid(&h->l, scid);
    if (!c || c->st != CH_CONFIG) return;
    if (result == 0) {
        c->ours_ok = 1;
        check_open(h, c);
        return;
    }
    log_line("host: the speaker turned down our channel settings (result %u)", result);
    if (c->tries >= 3) {
        set_error(h, "the speaker refused the channel settings");
        link_disconnect(h, "configuration refused");
        return;
    }
    /* Again without options: every default is acceptable to any device */
    uint8_t r[4];
    put16(r, c->rcid);
    put16(r + 2, 0);
    sig_send(h, SIG_CFG_REQ, next_id(h), r, 4);
    c->t = now_ms();
    c->tries++;
}

static void channel_closed(host_t *h, chan_t *c)
{
    link_t *l = &h->l;
    if (c == &l->sig) {
        log_line("host: AVDTP signaling closed by the speaker");
        if (l->media.st != CH_CLOSED) chan_disconnect(h, &l->media);
        l->configured = l->opened = 0;
        l->pending = 0;
        l->peer_spoke = 0;
        set_state(h, HOST_SIGNALING);
        l->reopen_at = now_ms() + T_REOPEN;
    } else if (c == &l->media) {
        log_line("host: media channel closed");
        if (l->state >= HOST_READY) set_state(h, HOST_SIGNALING);
        l->opened = 0;
        l->configured = 0;
        l->reopen_at = now_ms() + T_REOPEN;
    }
}

static void on_signaling(host_t *h, const uint8_t *d, int len)
{
    for (int off = 0; off + 4 <= len;) {
        int code = d[off], id = d[off + 1], blen = (int)le16(d + off + 2);
        if (off + 4 + blen > len) break;
        const uint8_t *b = d + off + 4;
        switch (code) {
        case SIG_CONN_REQ: on_conn_req(h, id, b, blen); break;
        case SIG_CONN_RSP: on_conn_rsp(h, b, blen); break;
        case SIG_CFG_REQ:  on_cfg_req(h, id, b, blen); break;
        case SIG_CFG_RSP:  on_cfg_rsp(h, b, blen); break;
        case SIG_DISC_REQ:
            if (blen >= 4) {
                chan_t *c = chan_by_lcid(&h->l, le16(b));
                uint8_t r[4];
                memcpy(r, b, 4);
                sig_send(h, SIG_DISC_RSP, id, r, 4);
                if (c) {
                    c->st = CH_CLOSED;
                    channel_closed(h, c);
                }
            }
            break;
        case SIG_DISC_RSP:
            break;
        case SIG_ECHO_REQ:
            sig_send(h, SIG_ECHO_RSP, id, b, blen > 44 ? 44 : blen);
            break;
        case SIG_INFO_REQ:
            if (blen >= 2) {
                unsigned type = le16(b);
                uint8_t r[12];
                put16(r, type);
                if (type == 0x0002) {          /* extended features: fixed channels only */
                    put16(r + 2, 0);
                    put32(r + 4, 0x00000080);
                    sig_send(h, SIG_INFO_RSP, id, r, 8);
                } else if (type == 0x0003) {   /* fixed channels: signaling */
                    put16(r + 2, 0);
                    memset(r + 4, 0, 8);
                    r[4] = 0x02;
                    sig_send(h, SIG_INFO_RSP, id, r, 12);
                } else {
                    put16(r + 2, 1);           /* not supported */
                    sig_send(h, SIG_INFO_RSP, id, r, 4);
                }
            }
            break;
        case SIG_REJECT:
            log_line("host: the speaker rejected an L2CAP command (reason %u)", blen >= 2 ? le16(b) : 0);
            break;
        default:
            break;
        }
        off += 4 + blen;
    }
}

static void on_l2cap(host_t *h, const uint8_t *d, int len)
{
    if (len < 4) return;
    unsigned l2len = le16(d), cid = le16(d + 2);
    if ((int)l2len > len - 4) return;
    const uint8_t *p = d + 4;
    link_t *l = &h->l;
    if (cid == CID_SIGNALING) {
        on_signaling(h, p, (int)l2len);
    } else if (l->sig.st == CH_OPEN && cid == l->sig.lcid) {
        av_on_frame(h, p, (int)l2len);
    } else if (l->sdp.st == CH_OPEN && cid == l->sdp.lcid) {
        uint8_t out[300];
        int n = sdp_answer(p, (int)l2len, out, (int)sizeof(out) < l->sdp.out_mtu ? (int)sizeof(out) : l->sdp.out_mtu);
        if (n > 0) l2_send(h, l->sdp.rcid, out, n);
    }
}

static void on_acl(host_t *h, const uint8_t *pkt, int len)
{
    link_t *l = &h->l;
    if (len < 4 || !l->connected) return;
    unsigned hdr = le16(pkt), dlen = le16(pkt + 2);
    if ((hdr & 0x0FFF) != l->handle || (int)dlen > len - 4) return;
    int pb = (int)(hdr >> 12) & 3;
    const uint8_t *d = pkt + 4;
    if (pb != 1) {
        if (dlen < 4) return;
        int need = 4 + (int)le16(d);
        if (need > (int)sizeof(l->rx)) {
            log_line("host: L2CAP frame of %d bytes does not fit, dropped", need);
            l->rx_need = 0;
            return;
        }
        memcpy(l->rx, d, dlen);
        l->rx_len = (int)dlen;
        l->rx_need = need;
    } else {
        if (!l->rx_need || l->rx_len + (int)dlen > (int)sizeof(l->rx)) return;
        memcpy(l->rx + l->rx_len, d, dlen);
        l->rx_len += (int)dlen;
    }
    if (l->rx_need && l->rx_len >= l->rx_need) {
        on_l2cap(h, l->rx, l->rx_need);
        l->rx_need = 0;
    }
}

/* ---- the link ---------------------------------------------------------------- */

static void link_reset(host_t *h)
{
    memset(&h->l, 0, sizeof(h->l));
    h->tx_n = 0;
    h->tx_head = 0;
    h->credits = h->acl_max;
}

static void link_down(host_t *h, int reason)
{
    link_t *l = &h->l;
    log_line("host: link to %s down (reason 0x%02x)", addr_s(l->addr), reason);
    if (l->state < HOST_READY && !h->last_error[0]) {
        if (reason == 0x05 || reason == 0x06) set_error(h, "pairing with the speaker failed");
        else if (reason == 0x08) set_error(h, "the speaker went out of range");
        else set_error(h, "the speaker dropped the connection during set-up (0x%02x)", reason);
    }
    link_reset(h);
    if (h->ev.on_state) h->ev.on_state(h->ev.ud, HOST_IDLE);
}

static void link_disconnect(host_t *h, const char *why)
{
    link_t *l = &h->l;
    if (!l->used) return;
    if (!l->connected) {
        if (l->state == HOST_PAGING) {
            cmd(h, OP_CREATE_CONN_CANCEL, l->addr, 6);
            log_line("host: call to %s cancelled (%s)", addr_s(l->addr), why);
        }
        link_reset(h);
        if (h->ev.on_state) h->ev.on_state(h->ev.ud, HOST_IDLE);
        return;
    }
    if (l->disconnecting) return;
    log_line("host: dropping the link to %s (%s)", addr_s(l->addr), why);
    uint8_t p[3];
    put16(p, l->handle);
    p[2] = 0x13;                            /* remote user terminated connection */
    cmd(h, OP_DISCONNECT, p, 3);
    l->disconnecting = 1;
    l->t_disc = now_ms();
}

static void request_auth(host_t *h)
{
    link_t *l = &h->l;
    uint8_t p[2];
    put16(p, l->handle);
    l->auth_sent = 1;
    set_state(h, HOST_SECURING);
    cmd(h, OP_AUTH_REQUESTED, p, 2);
}

static found_t *found_get(host_t *h, const uint8_t addr[6])
{
    for (int i = 0; i < h->nfound; i++)
        if (memcmp(h->found[i].addr, addr, 6) == 0) return &h->found[i];
    return NULL;
}

static void on_conn_complete(host_t *h, const uint8_t *p, int n)
{
    link_t *l = &h->l;
    if (n < 11 || p[9] != 0x01) return;     /* ACL links only */
    if (!l->used || memcmp(l->addr, p + 3, 6) != 0) return;
    if (p[0] != 0) {
        if (p[0] == 0x04) set_error(h, "%s did not answer: switch it on, in range, and not playing from another device",
                                    l->name[0] ? l->name : addr_s(l->addr));
        else set_error(h, "the call to %s failed (0x%02x)", l->name[0] ? l->name : addr_s(l->addr), p[0]);
        link_reset(h);
        if (h->ev.on_state) h->ev.on_state(h->ev.ud, HOST_IDLE);
        return;
    }
    l->handle = le16(p + 1) & 0x0FFF;
    l->connected = 1;
    l->t_conn = now_ms();
    l->enc_on = p[10] != 0;
    chan_reset(&l->sig, CID_AV_SIG);
    chan_reset(&l->media, CID_AV_MEDIA);
    chan_reset(&l->sdp, CID_SDP_SRV);
    log_line("host: link to %s up (handle 0x%03x, %s)", addr_s(l->addr), l->handle, l->incoming ? "it called us" : "we called it");
    set_state(h, HOST_SECURING);
    if (!l->incoming) request_auth(h);
}

static void on_conn_request(host_t *h, const uint8_t *p, int n)
{
    if (n < 10) return;
    const uint8_t *a = p;
    uint32_t cod = (uint32_t)p[6] | (uint32_t)p[7] << 8 | (uint32_t)p[8] << 16;
    link_t *l = &h->l;
    uint8_t r[7];
    memcpy(r, a, 6);
    if (p[9] != 0x01 || l->used || !store_has(a)) {
        log_line("host: call from %s (class 0x%06x) turned down: %s", addr_s(a), cod,
                 p[9] != 0x01 ? "not an ACL link" : l->used ? "busy with another speaker" : "not a paired speaker");
        r[6] = 0x0F;                        /* unacceptable BD_ADDR */
        cmd(h, OP_REJECT_CONNECTION, r, 7);
        return;
    }
    if (h->scanning) {
        cmd(h, OP_INQUIRY_CANCEL, NULL, 0);
        h->scanning = 0;
    }
    link_reset(h);
    l->used = 1;
    l->incoming = 1;
    l->t_start = now_ms();
    memcpy(l->addr, a, 6);
    store_entry_t list[STORE_MAX];
    int k = store_list(list, STORE_MAX);
    for (int i = 0; i < k; i++)
        if (memcmp(list[i].addr, a, 6) == 0) snprintf(l->name, sizeof(l->name), "%s", list[i].name);
    log_line("host: %s (%s) is calling, accepting", l->name, addr_s(a));
    r[6] = 0x01;                            /* stay peripheral: no role switch to fail */
    cmd(h, OP_ACCEPT_CONNECTION, r, 7);
    set_state(h, HOST_PAGING);
}

static void on_auth_complete(host_t *h, const uint8_t *p, int n)
{
    link_t *l = &h->l;
    if (n < 3 || !l->connected || (le16(p + 1) & 0x0FFF) != l->handle) return;
    if (p[0] == 0) {
        l->auth_ok = 1;
        log_line("host: %s authenticated", addr_s(l->addr));
        if (!l->enc_on && !l->enc_sent) {
            uint8_t e[3];
            put16(e, l->handle);
            e[2] = 1;
            l->enc_sent = 1;
            cmd(h, OP_SET_ENCRYPTION, e, 3);
        }
        return;
    }
    log_line("host: authentication of %s failed (0x%02x)%s", addr_s(l->addr), p[0], l->key_used ? " with the stored key" : "");
    if (l->key_used && !l->auth_retry) {
        /* The speaker forgot us (reset, or paired with too many devices): pair again */
        store_drop_key(l->addr);
        l->auth_retry = 1;
        l->key_used = 0;
        request_auth(h);
        return;
    }
    set_error(h, "pairing with %s failed: put it in pairing mode and try again", l->name[0] ? l->name : addr_s(l->addr));
    link_disconnect(h, "authentication failed");
}

static void on_encryption(host_t *h, const uint8_t *p, int n)
{
    link_t *l = &h->l;
    if (n < 4 || !l->connected || (le16(p + 1) & 0x0FFF) != l->handle) return;
    if (p[0] != 0 || !p[3]) {
        log_line("host: encryption with %s not on (status 0x%02x)", addr_s(l->addr), p[0]);
        if (p[0] != 0) {
            set_error(h, "the speaker would not encrypt the link (0x%02x)", p[0]);
            link_disconnect(h, "encryption failed");
        }
        return;
    }
    l->enc_on = 1;
    l->t_enc = now_ms();
    log_line("host: link to %s encrypted", addr_s(l->addr));
    /* A speaker that called us may open AVDTP itself now: link_tick gives it a moment */
    if (l->sig.st == CH_CLOSED && !l->incoming) {
        set_state(h, HOST_SIGNALING);
        chan_connect(h, &l->sig, CID_AV_SIG);
    }
}

static int is_audio(uint32_t cod)
{
    int major = (int)(cod >> 8) & 0x1F;
    return major == 0x04 || (cod & 0x200000) != 0 || ((cod & 0x040000) && major != 0x01 && major != 0x02);
}

static void found_report(host_t *h, found_t *f)
{
    if (h->ev.on_found) h->ev.on_found(h->ev.ud, f->addr, f->name, f->cod, f->rssi, is_audio(f->cod));
}

static void eir_name(const uint8_t *eir, int len, char *out, int max)
{
    for (int off = 0; off + 1 < len;) {
        int flen = eir[off];
        if (flen == 0 || off + 1 + flen > len) return;
        int type = eir[off + 1];
        if (type == 0x09 || (type == 0x08 && !out[0])) {
            int n = flen - 1 < max - 1 ? flen - 1 : max - 1;
            memcpy(out, eir + off + 2, (size_t)n);
            out[n] = 0;
        }
        off += 1 + flen;
    }
}

static void on_inquiry_result(host_t *h, int code, const uint8_t *p, int n)
{
    if (n < 1) return;
    int num = p[0];
    for (int i = 0; i < num; i++) {
        const uint8_t *a;
        uint8_t psrm;
        uint32_t cod;
        uint16_t clk;
        int rssi = 0;
        const uint8_t *eir = NULL;
        int eir_len = 0;
        if (code == 0x02) {
            if (n < 1 + num * 14) return;
            a = p + 1 + 6 * i;
            psrm = p[1 + 6 * num + i];
            const uint8_t *c = p + 1 + 9 * num + 3 * i;
            cod = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16;
            clk = (uint16_t)le16(p + 1 + 12 * num + 2 * i);
        } else if (code == 0x22) {
            if (n < 1 + num * 14) return;
            a = p + 1 + 6 * i;
            psrm = p[1 + 6 * num + i];
            const uint8_t *c = p + 1 + 8 * num + 3 * i;
            cod = (uint32_t)c[0] | (uint32_t)c[1] << 8 | (uint32_t)c[2] << 16;
            clk = (uint16_t)le16(p + 1 + 11 * num + 2 * i);
            rssi = (int8_t)p[1 + 13 * num + i];
        } else {
            if (i > 0 || n < 15) return;
            a = p + 1;
            psrm = p[7];
            cod = (uint32_t)p[9] | (uint32_t)p[10] << 8 | (uint32_t)p[11] << 16;
            clk = (uint16_t)le16(p + 12);
            rssi = (int8_t)p[14];
            eir = p + 15;
            eir_len = n - 15;
        }
        h->heard++;
        found_t *f = found_get(h, a);
        int fresh = !f;
        if (!f) {
            if (h->nfound == FOUND_MAX) continue;
            f = &h->found[h->nfound++];
            memset(f, 0, sizeof(*f));
            memcpy(f->addr, a, 6);
        }
        f->psrm = psrm;
        f->clk = clk;
        f->cod = cod;
        if (rssi) f->rssi = rssi;
        char name[64] = "";
        if (eir) eir_name(eir, eir_len, name, (int)sizeof(name));
        int named = name[0] && strcmp(name, f->name) != 0;
        if (named) {
            snprintf(f->name, sizeof(f->name), "%s", name);
            f->named = 1;
        }
        if (fresh) log_line("host: found %s class 0x%06x rssi %d \"%s\"%s", addr_s(a), cod, rssi, f->name,
                            is_audio(cod) ? " (audio)" : "");
        if (fresh || named) found_report(h, f);
    }
}

static void ask_next_name(host_t *h)
{
    if (h->name_busy || h->scanning || h->l.used) return;
    for (int i = 0; i < h->nfound; i++) {
        found_t *f = &h->found[i];
        if (f->named || f->name_asked || !is_audio(f->cod)) continue;
        f->name_asked = 1;
        uint8_t p[10];
        memcpy(p, f->addr, 6);
        p[6] = f->psrm;
        p[7] = 0;
        put16(p + 8, f->clk | 0x8000);
        h->name_busy = 1;
        memcpy(h->name_addr, f->addr, 6);
        h->t_name = now_ms();
        cmd(h, OP_REMOTE_NAME, p, 10);
        return;
    }
}

static void on_name(host_t *h, const uint8_t *p, int n)
{
    if (n < 7) return;
    if (memcmp(p + 1, h->name_addr, 6) == 0) h->name_busy = 0;
    found_t *f = found_get(h, p + 1);
    if (p[0] == 0 && f && n > 7) {
        int len = n - 7 < 63 ? n - 7 : 63;
        memcpy(f->name, p + 7, (size_t)len);
        f->name[len] = 0;
        f->named = 1;
        log_line("host: %s is \"%s\"", addr_s(f->addr), f->name);
        found_report(h, f);
    }
    ask_next_name(h);
}

static void on_completed_packets(host_t *h, const uint8_t *p, int n)
{
    if (n < 1) return;
    int num = p[0];
    for (int i = 0; i < num && 1 + 4 * i + 4 <= n; i++) {
        h->credits += (int)le16(p + 1 + 4 * i + 2);
    }
    if (h->credits > h->acl_max) h->credits = h->acl_max;
    tx_flush(h);
}

static void on_event(host_t *h, const uint8_t *ev, int len)
{
    if (len < 2) return;
    int code = ev[0];
    const uint8_t *p = ev + 2;
    int n = len - 2;
    link_t *l = &h->l;
    uint8_t r[32];
    switch (code) {
    case 0x01:                              /* Inquiry Complete */
        if (h->scanning) {
            h->scanning = 0;
            log_line("host: search done, %d device(s) heard, %d listed", h->heard, h->nfound);
            if (h->heard == 0 && h->fake_csr && h->inquiry_mode == 2) {
                /* A CSR clone in HearBridge answered extended inquiries with nothing (PS5) */
                uint8_t m = 1;
                h->inquiry_mode = 1;
                cmd(h, OP_WRITE_INQUIRY_MODE, &m, 1);
                log_line("host: no answers with extended inquiry results; the next search uses RSSI results");
            }
            if (h->ev.on_scan_done) h->ev.on_scan_done(h->ev.ud);
            ask_next_name(h);
        }
        break;
    case 0x02: case 0x22: case 0x2F:
        if (h->scanning) on_inquiry_result(h, code, p, n);
        break;
    case 0x03: on_conn_complete(h, p, n); break;
    case 0x04: on_conn_request(h, p, n); break;
    case 0x05:                              /* Disconnection Complete */
        if (n >= 4 && l->connected && (le16(p + 1) & 0x0FFF) == l->handle && p[0] == 0) link_down(h, p[3]);
        break;
    case 0x06: on_auth_complete(h, p, n); break;
    case 0x07: on_name(h, p, n); break;
    case 0x08: on_encryption(h, p, n); break;
    case 0x0F:                              /* Command Status */
        if (n >= 4 && p[0] != 0) {
            unsigned op = le16(p + 2);
            log_line("host: command 0x%04x refused (0x%02x)", op, p[0]);
            if (op == OP_CREATE_CONNECTION && l->used && !l->connected) {
                set_error(h, "the adapter could not call the speaker (0x%02x)", p[0]);
                link_reset(h);
                if (h->ev.on_state) h->ev.on_state(h->ev.ud, HOST_IDLE);
            } else if (op == OP_INQUIRY) {
                h->scanning = 0;
                if (h->ev.on_scan_done) h->ev.on_scan_done(h->ev.ud);
            } else if (op == OP_REMOTE_NAME) {
                h->name_busy = 0;
            } else if (op == OP_AUTH_REQUESTED && l->connected) {
                set_error(h, "the adapter could not pair (0x%02x)", p[0]);
                link_disconnect(h, "authentication refused");
            }
        }
        break;
    case 0x13: on_completed_packets(h, p, n); break;
    case 0x16:                              /* PIN Code Request: legacy speakers use 0000 */
        if (n >= 6) {
            memset(r, 0, 23);
            memcpy(r, p, 6);
            r[6] = 4;
            memcpy(r + 7, "0000", 4);
            log_line("host: %s asks for a PIN, answering 0000", addr_s(p));
            cmd(h, OP_PIN_CODE_REPLY, r, 23);
        }
        break;
    case 0x17:                              /* Link Key Request */
        if (n >= 6) {
            memcpy(r, p, 6);
            int ours = l->used && memcmp(l->addr, p, 6) == 0;
            if (ours && !l->auth_sent) l->peer_secured = 1;
            if (store_get_key(p, r + 6)) {
                if (ours) l->key_used = 1;
                cmd(h, OP_LINK_KEY_REPLY, r, 22);
            } else {
                cmd(h, OP_LINK_KEY_NEG_REPLY, r, 6);
            }
        }
        break;
    case 0x18:                              /* Link Key Notification */
        if (n >= 23) {
            store_put(p, l->used && memcmp(l->addr, p, 6) == 0 ? l->name : NULL, p + 6);
            h->pairings++;
            log_line("host: paired with %s (key type %u), key stored", addr_s(p), p[22]);
        }
        break;
    case 0x31:                              /* IO Capability Request: no display, no keyboard */
        if (n >= 6) {
            memcpy(r, p, 6);
            r[6] = 0x03;
            r[7] = 0x00;
            r[8] = 0x04;                    /* general bonding, no MITM protection */
            if (l->used && memcmp(l->addr, p, 6) == 0 && !l->auth_sent) l->peer_secured = 1;
            cmd(h, OP_IO_CAP_REPLY, r, 9);
        }
        break;
    case 0x33:                              /* User Confirmation Request: Just Works */
        if (n >= 6) cmd(h, OP_USER_CONFIRM_REPLY, p, 6);
        break;
    case 0x34:                              /* User Passkey Request: we have no keyboard */
        if (n >= 6) cmd(h, OP_USER_PASSKEY_NEG, p, 6);
        break;
    case 0x36:
        if (n >= 7) log_line("host: Simple Pairing with %s %s (0x%02x)", addr_s(p + 1), p[0] ? "failed" : "done", p[0]);
        break;
    case 0x14:
        if (n >= 6) log_line("host: link mode %s, interval %u", p[3] == 0 ? "active" : p[3] == 2 ? "sniff" : "hold", le16(p + 4));
        break;
    case 0x10:
        log_line("host: the adapter reports a hardware error (0x%02x)", n > 0 ? p[0] : 0);
        break;
    default:
        break;
    }
}

/* ---- timers ---------------------------------------------------------------- */

static void link_tick(host_t *h, long now)
{
    link_t *l = &h->l;
    if (!l->used) return;
    if (l->disconnecting) {
        if (now - l->t_disc > T_DISC) {
            log_line("host: no Disconnection Complete from the adapter, taking the link as down");
            link_down(h, 0x16);
        }
        return;
    }
    if (l->state == HOST_PAGING && !l->connected && now - l->t_start > T_PAGE) {
        set_error(h, "%s did not answer the call", l->name[0] ? l->name : addr_s(l->addr));
        link_disconnect(h, "page timed out");
        return;
    }
    if (!l->connected) return;
    if (l->state < HOST_READY && now - l->t_start > T_SETUP && l->reopen_at == 0) {
        set_error(h, "setting up the stream with %s took too long", l->name[0] ? l->name : addr_s(l->addr));
        link_disconnect(h, "set-up timed out");
        return;
    }
    /* A speaker that called us leads for a moment: if it neither pairs nor encrypts, we do it; once
     * the link is encrypted and it has not opened AVDTP, we open it */
    if (l->incoming && l->sig.st == CH_CLOSED && l->reopen_at == 0) {
        if (!l->enc_on && !l->auth_sent && !l->peer_secured && now - l->t_conn > T_PEER_FIRST) {
            request_auth(h);
        } else if (l->enc_on && now - l->t_enc > T_PEER_FIRST) {
            set_state(h, HOST_SIGNALING);
            chan_connect(h, &l->sig, CID_AV_SIG);
        }
    }
    /* The sink opened AVDTP and said nothing: we lead the set-up */
    if (l->sig.st == CH_OPEN && !l->peer_spoke && !l->configured && !l->pending && l->state < HOST_CONFIGURING &&
        now - l->t_sig_open > T_PEER_FIRST) {
        start_discover(h);
    }
    /* Channels and AVDTP commands without an answer */
    chan_t *cs[3] = { &l->sig, &l->media, &l->sdp };
    for (int i = 0; i < 3; i++) {
        chan_t *c = cs[i];
        if ((c->st == CH_CONNECTING || c->st == CH_CONFIG) && now - c->t > T_REPLY) {
            if (c->st == CH_CONFIG && !c->ours_ok && c->tries < 3) {
                send_cfg_req(h, c);
                continue;
            }
            if (c == &l->sdp) {
                c->st = CH_CLOSED;
                continue;
            }
            set_error(h, "the speaker did not finish opening the audio channel");
            link_disconnect(h, "channel timed out");
            return;
        }
    }
    if (l->pending && now - l->t_pending > T_REPLY) {
        if (l->pending_tries++ < 1) {
            log_line("host: no answer to AVDTP signal 0x%02x, asking again", l->pending);
            l->t_pending = now;
            av_send(h, l->pending_msg, l->pending_len);
        } else {
            set_error(h, "the speaker stopped answering during the stream set-up");
            link_disconnect(h, "AVDTP timed out");
            return;
        }
    }
    if (l->state == HOST_READY && l->start_at && now >= l->start_at && !l->pending) {
        l->start_at = 0;
        av_simple(h, AVDTP_START, l->remote_seid);
    }
    if (l->state == HOST_READY && l->resume_at && now >= l->resume_at && !l->pending) {
        l->resume_at = 0;
        av_simple(h, AVDTP_START, l->remote_seid);
    }
    if (l->reopen_at && now >= l->reopen_at) {
        l->reopen_at = 0;
        l->t_start = now;
        if (l->sig.st == CH_CLOSED) chan_connect(h, &l->sig, CID_AV_SIG);
        else if (l->sig.st == CH_OPEN && !l->configured && !l->pending) start_discover(h);
    }
}

static void host_tick(host_t *h, long now)
{
    link_tick(h, now);
    if (h->name_busy && now - h->t_name > T_NAME) {
        cmd(h, OP_REMOTE_NAME_CANCEL, h->name_addr, 6);
        h->name_busy = 0;
        ask_next_name(h);
    }
    if (h->credits <= 0 && h->l.connected && now - h->t_no_credits > T_NO_CREDITS) {
        log_line("host: no completed-packet report for %d ms, taking the adapter's %d buffers as free",
                 T_NO_CREDITS, h->acl_max);
        h->credits = h->acl_max;
        tx_flush(h);
    }
}

/* ---- public ------------------------------------------------------------------- */

host_t *host_open(hci_t hci, const host_events *ev, int fake_csr)
{
    host_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->hci = hci;
    if (ev) h->ev = *ev;
    h->fake_csr = fake_csr;

    uint8_t r[32];
    if (sync_cmd(h, OP_READ_BUFFER_SIZE, NULL, 0, r, sizeof(r)) < 8 || r[0] != 0) {
        log_line("host: Read Buffer Size failed");
        free(h);
        return NULL;
    }
    h->acl_mtu = (int)le16(r + 1);
    h->acl_max = (int)le16(r + 4);
    if (h->acl_mtu > HCI_PKT_MAX - 4) h->acl_mtu = HCI_PKT_MAX - 4;
    if (h->acl_mtu < 27) h->acl_mtu = 27;
    if (h->acl_max < 1) h->acl_max = 1;
    h->credits = h->acl_max;
    log_line("host: the adapter has %d ACL buffers of %d bytes", h->acl_max, h->acl_mtu);

    /* Every Classic event, with Connection Request and Extended Inquiry Result; no LE */
    static const uint8_t mask[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1F };
    static const uint8_t one = 1, mode = 2, page_scan = 0x02;
    static const uint8_t page_timeout[2] = { 0x00, 0x20 };      /* 0x2000 slots: 5.12 s */
    static const uint8_t policy[2] = { 0x01, 0x00 };            /* role switch; no sniff while streaming */
    if (sync_cmd(h, OP_SET_EVENT_MASK, mask, 8, r, sizeof(r)) < 1 ||
        sync_cmd(h, OP_WRITE_SSP_MODE, &one, 1, r, sizeof(r)) < 1) {
        log_line("host: the adapter refused the event mask or Simple Pairing");
        free(h);
        return NULL;
    }
    sync_cmd(h, OP_WRITE_INQUIRY_MODE, &mode, 1, r, sizeof(r));
    h->inquiry_mode = 2;
    sync_cmd(h, OP_WRITE_PAGE_TIMEOUT, page_timeout, 2, r, sizeof(r));
    sync_cmd(h, OP_WRITE_LINK_POLICY_DEF, policy, 2, r, sizeof(r));
    /* Page scan: a paired speaker switched on can call us by itself */
    if (sync_cmd(h, OP_WRITE_SCAN_ENABLE, &page_scan, 1, r, sizeof(r)) < 1) {
        log_line("host: page scan not set");
    }
    link_reset(h);
    return h->dead ? (free(h), NULL) : h;
}

void host_close(host_t *h)
{
    if (!h) return;
    if (h->l.connected && !h->dead) {
        uint8_t p[3];
        put16(p, h->l.handle);
        p[2] = 0x15;                        /* power off */
        cmd(h, OP_DISCONNECT, p, 3);
        h->hci.ops->pump(h->hci.ctx, 100);
    }
    free(h);
}

int host_poll(host_t *h, int timeout_ms)
{
    if (h->dead) return -1;
    int r = h->hci.ops->pump(h->hci.ctx, timeout_ms);
    if (r < 0) {
        h->dead = 1;
        return -1;
    }
    uint8_t buf[HCI_PKT_MAX];
    int n, budget = 64;
    while (budget-- > 0) {
        int any = 0;
        if ((n = h->hci.ops->next_event(h->hci.ctx, buf, sizeof(buf))) > 0) {
            on_event(h, buf, n);
            any = 1;
        }
        if ((n = h->hci.ops->next_acl(h->hci.ctx, buf, sizeof(buf))) > 0) {
            on_acl(h, buf, n);
            any = 1;
        }
        if (!any) break;
    }
    host_tick(h, now_ms());
    return h->dead ? -1 : 0;
}

int host_scan(host_t *h, int seconds)
{
    if (h->l.used && h->l.state != HOST_STREAMING && h->l.state != HOST_READY) return 0;
    if (h->fake_csr && h->l.used) return 0;
    if (h->scanning) return 1;
    if (h->name_busy) {
        cmd(h, OP_REMOTE_NAME_CANCEL, h->name_addr, 6);
        h->name_busy = 0;
    }
    int units = (int)(seconds / 1.28 + 0.5);
    if (units < 1) units = 1;
    if (units > 0x30) units = 0x30;
    uint8_t p[5] = { 0x33, 0x8B, 0x9E, (uint8_t)units, 0 };    /* general inquiry access code */
    h->nfound = 0;
    h->heard = 0;
    h->scanning = 1;
    log_line("host: searching for speakers for %.1f s", units * 1.28);
    return cmd(h, OP_INQUIRY, p, 5);
}

int host_scanning(const host_t *h)
{
    return h->scanning || h->name_busy;
}

int host_connect(host_t *h, const uint8_t addr[6], const char *name)
{
    link_t *l = &h->l;
    if (l->used) return 0;
    if (h->scanning) {
        cmd(h, OP_INQUIRY_CANCEL, NULL, 0);
        h->scanning = 0;
    }
    if (h->name_busy) {
        cmd(h, OP_REMOTE_NAME_CANCEL, h->name_addr, 6);
        h->name_busy = 0;
    }
    link_reset(h);
    l->used = 1;
    l->t_start = now_ms();
    memcpy(l->addr, addr, 6);
    found_t *f = found_get(h, addr);
    if (name && name[0]) snprintf(l->name, sizeof(l->name), "%s", name);
    else if (f && f->name[0]) snprintf(l->name, sizeof(l->name), "%s", f->name);
    h->last_error[0] = 0;

    uint8_t p[13];
    memcpy(p, addr, 6);
    put16(p + 6, 0xCC18);                   /* DM1, DH1, DM3, DH3, DM5, DH5; EDR allowed */
    p[8] = f ? f->psrm : 0x01;
    p[9] = 0;
    put16(p + 10, f ? (unsigned)(f->clk | 0x8000) : 0);
    p[12] = 0x01;                           /* allow role switch */
    log_line("host: calling %s \"%s\"", addr_s(addr), l->name);
    set_state(h, HOST_PAGING);
    return cmd(h, OP_CREATE_CONNECTION, p, 13);
}

void host_disconnect(host_t *h)
{
    h->last_error[0] = 0;
    link_disconnect(h, "asked from the dashboard");
}

int host_send_media(host_t *h, const uint8_t *pkt, int len)
{
    link_t *l = &h->l;
    if (l->state != HOST_STREAMING || l->media.st != CH_OPEN || l->disconnecting) return -1;
    if (len > l->media.out_mtu) len = l->media.out_mtu;
    tx_flush(h);
    uint8_t frame[TX_MAX + 1100];
    if (len + 4 > (int)sizeof(frame)) return 0;
    if (h->tx_n || h->credits < frags_for(h, len + 4)) {
        h->media_dropped++;
        return 0;
    }
    put16(frame, (unsigned)len);
    put16(frame + 2, l->media.rcid);
    memcpy(frame + 4, pkt, (size_t)len);
    if (!acl_write(h, frame, len + 4)) {
        h->media_dropped++;
        return 0;
    }
    h->media_sent++;
    return 1;
}

void host_resume(host_t *h)
{
    link_t *l = &h->l;
    if (l->state == HOST_READY && !l->pending) av_simple(h, AVDTP_START, l->remote_seid);
}

void host_status(const host_t *h, host_status_t *out)
{
    memset(out, 0, sizeof(*out));
    const link_t *l = &h->l;
    out->state = l->state;
    memcpy(out->addr, l->addr, 6);
    snprintf(out->name, sizeof(out->name), "%s", l->name);
    out->incoming = l->incoming;
    out->cfg = l->cfg;
    out->mtu = l->media.out_mtu;
    out->acl_mtu = h->acl_mtu;
    out->acl_bufs = h->acl_max;
    out->acl_free = h->credits;
    out->media_sent = h->media_sent;
    out->media_dropped = h->media_dropped;
    out->pairings = h->pairings;
    snprintf(out->last_error, sizeof(out->last_error), "%s", h->last_error);
}
