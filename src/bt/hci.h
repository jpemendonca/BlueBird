#ifndef BT_HCI_H
#define BT_HCI_H

/* The HCI transport the host sees: commands out, events and ACL packets in. The console uses the
 * USB adapter (usb_hci.c); host tests use a simulated controller and speaker. */

#define HCI_PKT_MAX 1100

typedef struct hci_ops {
    /* Sends an HCI command. Returns 0 on a transport error. */
    int  (*cmd)(void *ctx, unsigned opcode, const void *params, int plen);
    /* Sends one ACL packet, 4-byte header included. Returns 0 on error. */
    int  (*acl_send)(void *ctx, const unsigned char *pkt, int len);
    /* Waits up to timeout_ms for incoming packets. Returns 1 if any queued, 0 if none, -1 once
     * the transport is gone (unplugged, rest mode). */
    int  (*pump)(void *ctx, int timeout_ms);
    /* Pops one queued packet; returns its length or 0. Events come as code, length, parameters;
     * ACL packets with their 4-byte header. */
    int  (*next_event)(void *ctx, unsigned char *out, int max);
    int  (*next_acl)(void *ctx, unsigned char *out, int max);
} hci_ops;

typedef struct {
    const hci_ops *ops;
    void *ctx;
} hci_t;

#endif /* BT_HCI_H */
