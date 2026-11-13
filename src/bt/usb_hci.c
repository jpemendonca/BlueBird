#include "usb_hci.h"
#include "bt_rtl.h"
#include "cmdq.h"
#include "log.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define EVQ_LEN   48
#define ACLQ_LEN  48
#define ACL_RX    2048
#define BUS_MAX   6
#define DEV_MAX   21

typedef struct {
    uint8_t data[HCI_PKT_MAX];
    int     len;
} pkt_t;

typedef struct {
    pkt_t    pkt[ACLQ_LEN];
    int      head, count, cap;
    unsigned dropped;
} queue_t;

enum { EP_EVT = 0, EP_ACL_IN = 1, EP_ACL_OUT = 2, EP_COUNT = 3 };

/* What the bus scan saw, for the dashboard: an adapter per node, 0 when none */
static struct {
    pthread_mutex_t lock;
    uint32_t ids[BUS_MAX][DEV_MAX];
    uint32_t logged[BUS_MAX][DEV_MAX];
    int      open_err[BUS_MAX][DEV_MAX];   /* the last open error logged for the node */
    char     failed[32];
    uint32_t failed_ids;
    char     current[32];
} g_bus = { .lock = PTHREAD_MUTEX_INITIALIZER };

static volatile int *g_stop;

void usb_hci_set_stop_flag(volatile int *flag)
{
    g_stop = flag;
}

#ifdef __PROSPERO__
#include <dev/usb/usb.h>
#include <dev/usb/usb_ioctl.h>
#include <dev/usb/usb_endian.h>
#include <dev/usb/usbdi.h>
#include "rtl8761bu_fw.h"
#include "rtl8761bu_config.h"

static int stopping(void)
{
    return g_stop && *g_stop;
}

static const char *manufacturer_name(uint16_t m)
{
    switch (m) {
    case 0x0002: return "Intel";
    case 0x000A: return "CSR";
    case 0x000F: return "Broadcom";
    case 0x001D: return "Qualcomm";
    case 0x0046: return "MediaTek";
    case 0x0048: return "Marvell";
    case 0x005D: return "Realtek";
    case 0x0131: return "Cypress";
    default:     return NULL;
    }
}

static void queue_push(queue_t *q, const uint8_t *data, int len)
{
    if (len > HCI_PKT_MAX) len = HCI_PKT_MAX;
    if (q->count == q->cap) {
        q->head = (q->head + 1) % q->cap;
        q->count--;
        q->dropped++;
    }
    pkt_t *p = &q->pkt[(q->head + q->count) % q->cap];
    memcpy(p->data, data, (size_t)len);
    p->len = len;
    q->count++;
}

static int queue_pop(queue_t *q, uint8_t *out, int max)
{
    if (q->count == 0) return 0;
    pkt_t *p = &q->pkt[q->head];
    int len = p->len < max ? p->len : max;
    memcpy(out, p->data, (size_t)len);
    q->head = (q->head + 1) % q->cap;
    q->count--;
    return len;
}

static void fmt_addr(const uint8_t *a, char *out)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
}


typedef struct {
    int      fd;
    usb_hci_info_t *info;
    uint8_t  ep_addr[EP_COUNT];
    uint16_t ep_mps[EP_COUNT];
    struct usb_fs_endpoint eps[EP_COUNT];
    void    *frame[EP_COUNT];
    uint32_t frame_len[EP_COUNT];
    int      pending[EP_COUNT];
    uint8_t  rx_evt[64];
    uint8_t  rx_acl[ACL_RX];
    uint8_t  tx_acl[HCI_PKT_MAX];
    int      out_done, out_status;
    uint8_t  evt_acc[512];      /* events span several interrupt packets */
    int      evt_acc_len;
    uint8_t  acl_acc[2 * ACL_RX];
    int      acl_acc_len;
    queue_t  evq, aclq;
    unsigned events_in, acl_in, acl_out;
    long     last_event_at, last_acl_at;
    int      gone;
    bt_cmdq_t cmdq;
} dev_t_;

static dev_t_ g_dev = { .fd = -1 };

/* The console's own radio: MediaTek (0e8d:3603 on ugen0.2), or Marvell on some consoles. OmniPad
 * saw a 1286:2059 taken as an adapter, and a user lost the DualSense (4 Nov). External adapters
 * of these vendors are rare, so the whole vendor is left alone. */
static int is_internal_radio(uint16_t vid)
{
    return vid == 0x0e8d || vid == 0x1286;
}

/* The HCI interface (class E0/01/01): its event, ACL in and ACL out endpoints */
static int find_hci_interface(int fd, uint8_t ep_addr[EP_COUNT], uint16_t ep_mps[EP_COUNT])
{
    uint8_t raw[1024];
    struct usb_gen_descriptor desc;
    memset(&desc, 0, sizeof(desc));
    desc.ugd_data = raw;
    desc.ugd_maxlen = sizeof(raw);
    if (ioctl(fd, USB_GET_FULL_DESC, &desc) != 0) return 0;
    int len = desc.ugd_actlen;
    if (len <= 0 || len > (int)sizeof(raw)) return 0;

    int in_hci = 0;
    memset(ep_addr, 0, EP_COUNT);
    for (int off = 0; off + 2 <= len;) {
        int dl = raw[off];
        if (dl < 2 || off + dl > len) break;
        if (raw[off + 1] == 0x04 && dl >= 9) {
            in_hci = raw[off + 2] == 0 && raw[off + 3] == 0 &&
                     raw[off + 5] == 0xE0 && raw[off + 6] == 0x01 && raw[off + 7] == 0x01;
        } else if (raw[off + 1] == 0x05 && dl >= 7 && in_hci) {
            uint8_t addr = raw[off + 2], type = raw[off + 3] & 0x03;
            uint16_t mps = (uint16_t)(raw[off + 4] | (raw[off + 5] & 0x07) << 8);
            int k = -1;
            if (type == 0x03 && (addr & 0x80)) k = EP_EVT;
            else if (type == 0x02 && (addr & 0x80)) k = EP_ACL_IN;
            else if (type == 0x02) k = EP_ACL_OUT;
            if (k >= 0 && !ep_addr[k]) {
                ep_addr[k] = addr;
                ep_mps[k] = mps;
            }
        }
        off += dl;
    }
    return ep_addr[EP_EVT] && ep_addr[EP_ACL_IN] && ep_addr[EP_ACL_OUT];
}

static int parse_prefer(const char *prefer, uint32_t *ids)
{
    unsigned v, p;
    if (!prefer || sscanf(prefer, "%4x:%4x", &v, &p) != 2) return 0;
    *ids = v << 16 | p;
    return 1;
}

int usb_hci_find(const char *prefer, usb_hci_info_t *info)
{
    uint32_t want = 0;
    int has_want = parse_prefer(prefer, &want);
    int found_bus = -1, found_dev = -1, found_fd = -1;
    uint32_t found_ids = 0;
    /* Only the endpoints here: the whole device state is 115 KB, and on the stack it brought the
     * payload down as the worker thread started (PS5, 13 Nov) */
    uint8_t ep_addr[EP_COUNT];
    uint16_t ep_mps[EP_COUNT];

    /* The failed adapter is forgotten once it is unplugged, so plugging it in again retries it */
    if (g_bus.failed[0]) {
        int fd = open(g_bus.failed, O_RDWR | O_NONBLOCK);
        struct usb_device_descriptor dd;
        int same = fd >= 0 && ioctl(fd, USB_GET_DEVICE_DESC, &dd) == 0 &&
                   ((uint32_t)UGETW(dd.idVendor) << 16 | UGETW(dd.idProduct)) == g_bus.failed_ids;
        if (fd >= 0) close(fd);
        if (!same) {
            log_line("bt: the adapter that did not start was unplugged");
            g_bus.failed[0] = 0;
        }
    }

    for (int bus = 0; bus < BUS_MAX; bus++) {
        for (int dev = 1; dev < DEV_MAX; dev++) {
            char path[32];
            snprintf(path, sizeof(path), "/dev/ugen%d.%d", bus, dev);
            int fd = open(path, O_RDWR | O_NONBLOCK);
            if (fd < 0 && errno != ENOENT && errno != ENXIO && g_bus.open_err[bus][dev] != errno) {
                g_bus.open_err[bus][dev] = errno;
                log_line("bt: cannot open %s (%s)%s", path, strerror(errno),
                         errno == EBUSY ? ": another app holds it, OmniPad for one" : "");
            }
            if (fd >= 0) g_bus.open_err[bus][dev] = 0;
            if (fd < 0) {
                /* Held by another app (OmniPad, for one): keep what was last seen there */
                if (errno != EBUSY) {
                    pthread_mutex_lock(&g_bus.lock);
                    g_bus.ids[bus][dev] = 0;
                    pthread_mutex_unlock(&g_bus.lock);
                }
                continue;
            }
            struct usb_device_descriptor dd;
            if (ioctl(fd, USB_GET_DEVICE_DESC, &dd) != 0) {
                close(fd);
                continue;
            }
            uint16_t vid = UGETW(dd.idVendor), pid = UGETW(dd.idProduct);
            uint32_t ids = (uint32_t)vid << 16 | pid;
            if (is_internal_radio(vid)) {
                /* Checked before any further request reaches it */
                if (g_bus.logged[bus][dev] != ids) {
                    g_bus.logged[bus][dev] = ids;
                    log_line("bt: %04x:%04x on %s is the console's own radio, left alone", vid, pid, path);
                }
                close(fd);
                continue;
            }
            if (!find_hci_interface(fd, ep_addr, ep_mps)) {
                if (g_bus.logged[bus][dev] != ids) {
                    g_bus.logged[bus][dev] = ids;
                    log_line("bt: %04x:%04x on %s (class %02x) is not a Bluetooth adapter", vid, pid, path, dd.bDeviceClass);
                }
                close(fd);
                continue;
            }
            pthread_mutex_lock(&g_bus.lock);
            g_bus.ids[bus][dev] = ids;
            pthread_mutex_unlock(&g_bus.lock);
            if (g_bus.logged[bus][dev] != ids) {
                g_bus.logged[bus][dev] = ids;
                log_line("bt: Bluetooth adapter %04x:%04x on %s", vid, pid, path);
            }
            int skip = (strcmp(g_bus.failed, path) == 0 && g_bus.failed_ids == ids) ||
                       found_fd >= 0 || (has_want && ids != want);
            if (skip) {
                close(fd);
                continue;
            }
            found_fd = fd;
            found_bus = bus;
            found_dev = dev;
            found_ids = ids;
        }
    }
    /* The preferred model is not plugged: take any other */
    if (found_fd < 0 && has_want) return usb_hci_find("", info);
    if (found_fd < 0) return 0;

    memset(info, 0, sizeof(*info));
    snprintf(info->path, sizeof(info->path), "/dev/ugen%d.%d", found_bus, found_dev);
    info->vid = (uint16_t)(found_ids >> 16);
    info->pid = (uint16_t)found_ids;
    info->firmware = -1;
    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.fd = found_fd;
    g_dev.info = info;
    find_hci_interface(found_fd, g_dev.ep_addr, g_dev.ep_mps);
    pthread_mutex_lock(&g_bus.lock);
    snprintf(g_bus.current, sizeof(g_bus.current), "%s", info->path);
    pthread_mutex_unlock(&g_bus.lock);
    return 1;
}

static int dev_open(dev_t_ *d)
{
    for (int itf = 0; itf < 2; itf++) {
        int n = itf;
        if (ioctl(d->fd, USB_IFACE_DRIVER_ACTIVE, &n) == 0) {
            log_line("bt: a system driver holds interface %d of %s, detaching it", itf, d->info->path);
            n = itf;
            if (ioctl(d->fd, USB_IFACE_DRIVER_DETACH, &n) != 0)
                log_line("bt: detach failed (%s)", strerror(errno));
        }
    }
    int config = 0;
    if (ioctl(d->fd, USB_GET_CONFIG, &config) == 0 && (config & 0xff) == 0xff) {
        config = 0;
        ioctl(d->fd, USB_SET_CONFIG, &config);
    }

    struct usb_fs_init init;
    memset(d->eps, 0, sizeof(d->eps));
    memset(&init, 0, sizeof(init));
    init.pEndpoints = d->eps;
    init.ep_index_max = EP_COUNT;
    if (ioctl(d->fd, USB_FS_INIT, &init) != 0) {
        log_line("bt: USB_FS_INIT failed on %s (%s)", d->info->path, strerror(errno));
        return 0;
    }
    static const uint32_t bufsize[EP_COUNT] = { 64, ACL_RX, HCI_PKT_MAX };
    d->frame[EP_EVT] = d->rx_evt;
    d->frame[EP_ACL_IN] = d->rx_acl;
    d->frame[EP_ACL_OUT] = d->tx_acl;
    for (int k = 0; k < EP_COUNT; k++) {
        struct usb_fs_open op;
        memset(&op, 0, sizeof(op));
        op.max_bufsize = bufsize[k];
        op.max_frames = 1;
        op.ep_index = (uint8_t)k;
        op.ep_no = d->ep_addr[k];
        if (ioctl(d->fd, USB_FS_OPEN, &op) != 0) {
            log_line("bt: cannot open endpoint 0x%02x on %s (%s)", d->ep_addr[k], d->info->path, strerror(errno));
            struct usb_fs_uninit un = {0};
            ioctl(d->fd, USB_FS_UNINIT, &un);
            return 0;
        }
        d->eps[k].ppBuffer = &d->frame[k];
        d->eps[k].pLength = &d->frame_len[k];
        d->eps[k].nFrames = 1;
        d->eps[k].flags = USB_FS_FLAG_SINGLE_SHORT_OK;
        d->eps[k].timeout = k == EP_ACL_OUT ? 1000 : USB_FS_TIMEOUT_NONE;
    }
    if (d->ep_mps[EP_EVT] == 0 || d->ep_mps[EP_EVT] > sizeof(d->rx_evt)) d->ep_mps[EP_EVT] = sizeof(d->rx_evt);
    d->evq.cap = EVQ_LEN;
    d->aclq.cap = ACLQ_LEN;
    bt_cmdq_reset(&d->cmdq);
    return 1;
}

static int arm_read(dev_t_ *d, int k)
{
    if (d->pending[k]) return 1;
    /* One interrupt packet per event read; ACL reads end on the short last packet */
    d->frame_len[k] = k == EP_EVT ? d->ep_mps[EP_EVT] : ACL_RX;
    struct usb_fs_start start = { .ep_index = (uint8_t)k };
    if (ioctl(d->fd, USB_FS_START, &start) != 0 && errno != EBUSY) {
        d->gone = 1;
        return 0;
    }
    d->pending[k] = 1;
    return 1;
}

/* Connection events as the adapter sends them, before the host sees them. Link keys are blanked. */
static void log_link_event(const uint8_t *ev, int len)
{
    static int budget = 400;
    switch (ev[0]) {
    case 0x01: case 0x02: case 0x22: case 0x2F: case 0x07:   /* inquiry and names: the host logs them */
    case 0x0E: case 0x0F: case 0x13: case 0x3E:              /* replies, completed packets, LE */
        return;
    default: break;
    }
    if (budget <= 0) return;
    budget--;
    char hex[3 * 24 + 1];
    int k;
    for (k = 0; k < len && k < 24; k++) {
        int blank = ev[0] == 0x18 && k >= 8 && k < 24;
        snprintf(hex + 3 * k, 4, blank ? "-- " : "%02x ", ev[k]);
    }
    hex[3 * k] = 0;
    log_line("bt: adapter event %s(len %d)", hex, len);
}

static void take_events(dev_t_ *d)
{
    while (d->evt_acc_len >= 2 && d->evt_acc_len >= 2 + d->evt_acc[1]) {
        int total = 2 + d->evt_acc[1];
        log_link_event(d->evt_acc, total);
        bt_cmdq_on_event(&d->cmdq, d->evt_acc, total);
        queue_push(&d->evq, d->evt_acc, total);
        d->events_in++;
        d->last_event_at = now_ms();
        d->evt_acc_len -= total;
        memmove(d->evt_acc, d->evt_acc + total, (size_t)d->evt_acc_len);
    }
}

static void take_acl(dev_t_ *d)
{
    while (d->acl_acc_len >= 4) {
        int total = 4 + (int)le16(d->acl_acc + 2);
        if (total > HCI_PKT_MAX) {
            log_line("bt: ACL packet of %d bytes does not fit, stream reset", total);
            d->acl_acc_len = 0;
            return;
        }
        if (d->acl_acc_len < total) return;
        queue_push(&d->aclq, d->acl_acc, total);
        d->acl_in++;
        d->last_acl_at = now_ms();
        d->acl_acc_len -= total;
        memmove(d->acl_acc, d->acl_acc + total, (size_t)d->acl_acc_len);
    }
}

static int hci_send(dev_t_ *d, uint16_t opcode, const uint8_t *params, int len)
{
    uint8_t buf[3 + 255];
    if (len < 0 || len > 255) return -1;
    buf[0] = (uint8_t)opcode;
    buf[1] = (uint8_t)(opcode >> 8);
    buf[2] = (uint8_t)len;
    if (len) memcpy(buf + 3, params, (size_t)len);

    struct usb_ctl_request req;
    memset(&req, 0, sizeof(req));
    req.ucr_request.bmRequestType = 0x20;   /* class request to the device: HCI command */
    req.ucr_request.bRequest = 0;
    USETW(req.ucr_request.wValue, 0);
    USETW(req.ucr_request.wIndex, 0);
    USETW(req.ucr_request.wLength, 3 + len);
    req.ucr_data = buf;
    long t0 = now_ms();
    if (ioctl(d->fd, USB_DO_REQUEST, &req) != 0) {
        log_line("bt: command 0x%04x not sent after %ld ms (%s)", opcode, now_ms() - t0, strerror(errno));
        d->gone = 1;
        return -1;
    }
    return 0;
}

static int send_queued(void *ctx, uint16_t opcode, const uint8_t *params, int len)
{
    dev_t_ *d = ctx;
    hci_send(d, opcode, params, len);
    return !d->gone;
}

static void flush_cmds(dev_t_ *d)
{
    if (!d->gone) bt_cmdq_flush(&d->cmdq, send_queued, d);
}

/* Collects finished transfers, waiting up to timeout_ms for the first, and keeps one read queued
 * on each IN endpoint. Returns -1 once the adapter is gone. */
static int service(dev_t_ *d, int timeout_ms)
{
    if (d->gone) return -1;
    if (!arm_read(d, EP_EVT) || !arm_read(d, EP_ACL_IN)) return -1;
    flush_cmds(d);

    long deadline = now_ms() + timeout_ms;
    int got = 0;
    for (;;) {
        struct usb_fs_complete c;
        memset(&c, 0, sizeof(c));
        if (ioctl(d->fd, USB_FS_COMPLETE, &c) == 0) {
            int k = c.ep_index;
            if (k < 0 || k >= EP_COUNT) continue;
            d->pending[k] = 0;
            int st = d->eps[k].status;
            int ok = st == USB_ERR_NORMAL_COMPLETION || st == USB_ERR_SHORT_XFER;
            got++;
            if (k == EP_ACL_OUT) {
                d->out_done = 1;
                d->out_status = ok ? 0 : st;
                continue;
            }
            if (!ok) {
                if (st == USB_ERR_CANCELLED) continue;
                log_line("bt: %s endpoint error %d", k == EP_EVT ? "event" : "ACL", st);
                d->gone = 1;
                return -1;
            }
            int n = (int)d->frame_len[k];
            if (k == EP_EVT) {
                if (n > (int)sizeof(d->evt_acc) - d->evt_acc_len) d->evt_acc_len = 0;
                memcpy(d->evt_acc + d->evt_acc_len, d->rx_evt, (size_t)n);
                d->evt_acc_len += n;
                take_events(d);
                flush_cmds(d);
            } else {
                if (n > (int)sizeof(d->acl_acc) - d->acl_acc_len) d->acl_acc_len = 0;
                memcpy(d->acl_acc + d->acl_acc_len, d->rx_acl, (size_t)n);
                d->acl_acc_len += n;
                take_acl(d);
            }
            if (!arm_read(d, k)) return -1;
            continue;
        }
        if (errno != EBUSY && errno != EAGAIN) {
            d->gone = 1;
            return -1;
        }
        long left = deadline - now_ms();
        if (got || left <= 0 || stopping()) return got;

        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        FD_SET(d->fd, &rset);
        FD_SET(d->fd, &wset);
        struct timeval tv = { 0, (left < 10 ? left : 10) * 1000 };
        select(d->fd + 1, &rset, &wset, NULL, &tv);
    }
}

static int acl_send(dev_t_ *d, const uint8_t *pkt, int len)
{
    if (d->gone || len <= 0 || len > HCI_PKT_MAX) return 0;
    memcpy(d->tx_acl, pkt, (size_t)len);
    d->frame_len[EP_ACL_OUT] = (uint32_t)len;
    d->out_done = 0;
    struct usb_fs_start start = { .ep_index = EP_ACL_OUT };
    if (ioctl(d->fd, USB_FS_START, &start) != 0) {
        log_line("bt: ACL send failed (%s)", strerror(errno));
        d->gone = 1;
        return 0;
    }
    d->pending[EP_ACL_OUT] = 1;
    long deadline = now_ms() + 1500;
    while (!d->out_done && now_ms() < deadline) {
        if (service(d, 5) < 0) return 0;
    }
    if (!d->out_done) {
        log_line("bt: ACL send timed out");
        return 0;
    }
    d->acl_out++;
    return d->out_status == 0;
}

static int op_cmd(void *ctx, unsigned opcode, const void *params, int plen)
{
    dev_t_ *d = ctx;
    if (d->gone || plen < 0 || plen > 255) return 0;
    if (!bt_cmdq_push(&d->cmdq, opcode, params, plen)) {
        log_line("bt: command queue full, command 0x%04x dropped", opcode);
        return 0;
    }
    flush_cmds(d);
    return !d->gone;
}

static int op_acl_send(void *ctx, const unsigned char *pkt, int len)
{
    return acl_send(ctx, pkt, len);
}

static int op_pump(void *ctx, int timeout_ms)
{
    dev_t_ *d = ctx;
    if (d->evq.count || d->aclq.count) {
        service(d, 0);
        return d->gone ? -1 : 1;
    }
    if (service(d, timeout_ms) < 0) return -1;
    return d->evq.count || d->aclq.count;
}

static int op_next_event(void *ctx, unsigned char *out, int max)
{
    return queue_pop(&((dev_t_ *)ctx)->evq, out, max);
}

static int op_next_acl(void *ctx, unsigned char *out, int max)
{
    return queue_pop(&((dev_t_ *)ctx)->aclq, out, max);
}

static const hci_ops k_usb_ops = {
    .cmd = op_cmd,
    .acl_send = op_acl_send,
    .pump = op_pump,
    .next_event = op_next_event,
    .next_acl = op_next_acl,
};

hci_t usb_hci_transport(void)
{
    hci_t t = { &k_usb_ops, &g_dev };
    return t;
}

void usb_hci_diag(void)
{
    dev_t_ *d = &g_dev;
    long now = now_ms();
    log_line("bt: transport %s: %u events, %u ACL in, %u ACL out, dropped %u events / %u ACL; "
             "last event %ld ms ago, last ACL %ld ms ago",
             d->info ? d->info->path : "?", d->events_in, d->acl_in, d->acl_out, d->evq.dropped,
             d->aclq.dropped, d->last_event_at ? now - d->last_event_at : -1L,
             d->last_acl_at ? now - d->last_acl_at : -1L);
}

/* Sends a command and waits for its Command Complete (return parameters, status first) or
 * Command Status (one status byte). Returns the reply length, or -1. */
static int hci_cmd(dev_t_ *d, uint16_t opcode, const uint8_t *params, int len,
                   uint8_t *reply, int max, int timeout_ms)
{
    if (hci_send(d, opcode, params, len) != 0) return -1;
    long deadline = now_ms() + timeout_ms;
    uint8_t ev[HCI_PKT_MAX];
    while (now_ms() < deadline) {
        int n;
        while ((n = queue_pop(&d->evq, ev, sizeof(ev))) > 0) {
            const uint8_t *p = ev + 2;
            int plen = n - 2;
            if (ev[0] == 0x0E && plen >= 3 && (p[1] | p[2] << 8) == opcode) {
                int r = plen - 3 < max ? plen - 3 : max;
                memcpy(reply, p + 3, (size_t)r);
                return r;
            }
            if (ev[0] == 0x0F && plen >= 4 && (p[2] | p[3] << 8) == opcode) {
                if (max > 0) reply[0] = p[0];
                return 1;
            }
        }
        if (service(d, 20) < 0) break;
    }
    if (!d->gone) log_line("bt: no reply to command 0x%04x", opcode);
    return -1;
}

typedef struct {
    uint8_t  hci_ver, lmp_ver;
    uint16_t hci_rev, manufacturer, lmp_sub;
} version_t;

static int read_version(dev_t_ *d, version_t *v)
{
    uint8_t r[16];
    if (hci_cmd(d, 0x1001, NULL, 0, r, sizeof(r), 1000) < 9 || r[0] != 0) return 0;
    v->hci_ver = r[1];
    v->hci_rev = (uint16_t)(r[2] | r[3] << 8);
    v->lmp_ver = r[4];
    v->manufacturer = (uint16_t)(r[5] | r[6] << 8);
    v->lmp_sub = (uint16_t)(r[7] | r[8] << 8);
    return 1;
}

/* Chosen by the chip's manufacturer, not the USB vendor: the TP-Link UB500 is 2357:0604, and a
 * check on Realtek's 0bda left it on its ROM firmware. */
static int realtek_firmware(dev_t_ *d, version_t *v)
{
    if (v->lmp_sub != 0x8761) {
        log_line("bt: Realtek chip already running patched firmware (subversion 0x%04x)", v->lmp_sub);
        return 1;
    }
    if (RTL8761BU_FW_LEN == 0) {
        log_line("bt: this build carries no Realtek firmware, staying on ROM firmware");
        return 0;
    }
    int want = rtl_expected_project(v->lmp_sub, v->hci_rev);
    uint8_t r[8];
    if (hci_cmd(d, 0xFC6D, NULL, 0, r, sizeof(r), 1000) < 2 || r[0] != 0) {
        log_line("bt: Realtek ROM version not readable, staying on ROM firmware");
        return 0;
    }
    uint8_t rom = r[1];

    static uint8_t data[64 * 1024];
    rtl_fw_info_t info;
    int total = rtl_fw_build((const uint8_t *)RTL8761BU_FW, RTL8761BU_FW_LEN,
                             (const uint8_t *)RTL8761BU_CONFIG, RTL8761BU_CONFIG_LEN,
                             rom, data, sizeof(data), &info);
    if (want < 0 || info.project_id != want) {
        log_line("bt: no firmware for this Realtek chip (rev 0x%04x, ROM %u); the embedded one is for RTL8761B",
                 v->hci_rev, rom);
        return 0;
    }
    if (total < 0) {
        log_line("bt: RTL8761B firmware has no patch for ROM %u (error %d)", rom, total);
        return 0;
    }
    int frags = (total + RTL_FRAGMENT_LEN - 1) / RTL_FRAGMENT_LEN;
    log_line("bt: loading RTL8761B firmware 0x%08x: patch %d (%d bytes) + config %d bytes, %d fragments",
             info.fw_version, info.patch_index, info.patch_len, info.config_len, frags);
    long t0 = now_ms();
    for (int n = 0; n < frags; n++) {
        uint8_t frag[1 + RTL_FRAGMENT_LEN];
        int len = total - n * RTL_FRAGMENT_LEN;
        if (len > RTL_FRAGMENT_LEN) len = RTL_FRAGMENT_LEN;
        frag[0] = rtl_fragment_index(n, frags);
        memcpy(frag + 1, data + n * RTL_FRAGMENT_LEN, (size_t)len);
        if (hci_cmd(d, 0xFC20, frag, 1 + len, r, sizeof(r), 3000) < 1 || r[0] != 0) {
            log_line("bt: firmware fragment %d of %d failed", n + 1, frags);
            return 0;
        }
    }
    version_t after;
    if (read_version(d, &after)) {
        log_line("bt: firmware loaded in %ld ms, subversion 0x%04x -> 0x%04x",
                 now_ms() - t0, v->lmp_sub, after.lmp_sub);
        *v = after;
    }
    return 1;
}

/* Reset, firmware and naming; the host sets the rest */
static int bring_up(dev_t_ *d, const char *local_name)
{
    usb_hci_info_t *info = d->info;
    uint8_t r[255];
    if (hci_cmd(d, 0x0C03, NULL, 0, r, sizeof(r), 3000) < 1 || r[0] != 0) {
        log_line("bt: HCI reset failed on %s", info->path);
        return 0;
    }
    version_t v;
    if (!read_version(d, &v)) {
        log_line("bt: Read_Local_Version failed");
        return 0;
    }
    const char *maker = manufacturer_name(v.manufacturer);
    log_line("bt: chip %s (0x%04x), HCI %u rev 0x%04x, LMP %u subversion 0x%04x",
             maker ? maker : "?", v.manufacturer, v.hci_ver, v.hci_rev, v.lmp_ver, v.lmp_sub);

    /* Clones sold as CSR8510 carry CSR's USB ids (0a12:0001) and another manufacturer. In OmniPad
     * one hung for 5 s when a command arrived while a link was busy (PS5, 2 Nov). */
    info->fake_csr = info->vid == 0x0a12 && info->pid == 0x0001 && v.manufacturer != 0x000A;
    if (v.manufacturer == 0x005D) info->firmware = realtek_firmware(d, &v);
    hci_cmd(d, 0x0C03, NULL, 0, r, sizeof(r), 3000);

    /* Bluetooth Classic is what A2DP needs; an LE-only stick answers HCI but never streams.
     * LMP features byte 4, bit 5: BR/EDR not supported. */
    if (hci_cmd(d, 0x1003, NULL, 0, r, sizeof(r), 1000) >= 9 && r[0] == 0 && (r[5] & 0x20)) {
        log_line("bt: this adapter has no Bluetooth Classic (LE only); audio needs Classic");
        return 0;
    }

    /* Seen by speakers as an audio source (computer, desktop; audio + rendering services) */
    static const uint8_t cod[3] = { 0x04, 0x01, 0x2C };
    hci_cmd(d, 0x0C24, cod, sizeof(cod), r, sizeof(r), 1000);
    uint8_t name[248];
    memset(name, 0, sizeof(name));
    snprintf((char *)name, sizeof(name), "%s", local_name);
    hci_cmd(d, 0x0C13, name, sizeof(name), r, sizeof(r), 1000);

    if (hci_cmd(d, 0x1009, NULL, 0, r, sizeof(r), 1000) >= 7 && r[0] == 0) fmt_addr(r + 1, info->address);
    if (info->fake_csr) snprintf(info->chip, sizeof(info->chip), "CSR8510 clone");
    else if (v.manufacturer == 0x005D && v.lmp_sub == 0x8761) snprintf(info->chip, sizeof(info->chip), "Realtek RTL8761B (ROM)");
    else if (v.manufacturer == 0x005D) snprintf(info->chip, sizeof(info->chip), "Realtek RTL8761B");
    else if (maker) snprintf(info->chip, sizeof(info->chip), "%s", maker);
    else snprintf(info->chip, sizeof(info->chip), "manufacturer 0x%04x", v.manufacturer);
    return !d->gone;
}

int usb_hci_start(const char *local_name, usb_hci_info_t *info)
{
    dev_t_ *d = &g_dev;
    if (d->fd < 0 || d->info != info) return 0;
    log_line("bt: using adapter %04x:%04x on %s (events 0x%02x, ACL 0x%02x/0x%02x)",
             info->vid, info->pid, info->path, d->ep_addr[EP_EVT], d->ep_addr[EP_ACL_IN], d->ep_addr[EP_ACL_OUT]);
    return dev_open(d) && bring_up(d, local_name);
}

void usb_hci_close(void)
{
    dev_t_ *d = &g_dev;
    if (d->fd < 0) return;
    for (int k = 0; k < EP_COUNT; k++) {
        if (!d->pending[k]) continue;
        struct usb_fs_stop st = { .ep_index = (uint8_t)k };
        ioctl(d->fd, USB_FS_STOP, &st);
    }
    struct usb_fs_uninit un = {0};
    ioctl(d->fd, USB_FS_UNINIT, &un);
    close(d->fd);
    d->fd = -1;
    memset(d->pending, 0, sizeof(d->pending));
    d->evt_acc_len = d->acl_acc_len = 0;
    d->evq.count = d->aclq.count = 0;
    bt_cmdq_reset(&d->cmdq);
    pthread_mutex_lock(&g_bus.lock);
    g_bus.current[0] = 0;
    pthread_mutex_unlock(&g_bus.lock);
}

int usb_hci_still_there(const usb_hci_info_t *info)
{
    int fd = open(info->path, O_RDWR | O_NONBLOCK);
    struct usb_device_descriptor dd;
    int same = fd >= 0 && ioctl(fd, USB_GET_DEVICE_DESC, &dd) == 0 &&
               UGETW(dd.idVendor) == info->vid && UGETW(dd.idProduct) == info->pid;
    if (fd >= 0) close(fd);
    return same;
}

int usb_hci_reset(const char *path)
{
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0) return 0;
    int zero = 0;
    int ok = ioctl(fd, USB_DEVICEENUMERATE, &zero) == 0;
    if (ok) log_line("bt: adapter reset over USB (%s)", path);
    else log_line("bt: USB reset of %s failed (%s)", path, strerror(errno));
    close(fd);
    return ok;
}

#else

int usb_hci_find(const char *prefer, usb_hci_info_t *info)
{
    (void)prefer;
    (void)info;
    return 0;
}

int usb_hci_start(const char *local_name, usb_hci_info_t *info)
{
    (void)local_name;
    (void)info;
    return 0;
}

hci_t usb_hci_transport(void)
{
    hci_t t = { NULL, NULL };
    return t;
}

void usb_hci_diag(void) {}
void usb_hci_close(void) {}

int usb_hci_still_there(const usb_hci_info_t *info)
{
    (void)info;
    return 0;
}

int usb_hci_reset(const char *path)
{
    (void)path;
    return 0;
}

#endif

void usb_hci_mark_failed(const usb_hci_info_t *info)
{
    snprintf(g_bus.failed, sizeof(g_bus.failed), "%s", info->path);
    g_bus.failed_ids = (uint32_t)info->vid << 16 | info->pid;
}

int usb_hci_list_json(char *out, int n)
{
    int len = snprintf(out, (size_t)n, "[");
    int first = 1;
    pthread_mutex_lock(&g_bus.lock);
    for (int bus = 0; bus < BUS_MAX; bus++) {
        for (int dev = 1; dev < DEV_MAX && len < n; dev++) {
            uint32_t ids = g_bus.ids[bus][dev];
            if (!ids) continue;
            char path[32];
            snprintf(path, sizeof(path), "/dev/ugen%d.%d", bus, dev);
            len += snprintf(out + len, (size_t)(n - len), "%s{\"path\":\"%s\",\"id\":\"%04x:%04x\",\"in_use\":%s}",
                            first ? "" : ",", path, ids >> 16, ids & 0xffff,
                            strcmp(path, g_bus.current) == 0 ? "true" : "false");
            first = 0;
        }
    }
    pthread_mutex_unlock(&g_bus.lock);
    if (len < n) len += snprintf(out + len, (size_t)(n - len), "]");
    return len < n ? len : n - 1;
}
