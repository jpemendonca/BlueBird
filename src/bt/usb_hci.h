#ifndef BT_USB_HCI_H
#define BT_USB_HCI_H

#include "hci.h"

#include <stdint.h>

/* BlueBird's own USB Bluetooth adapter: finding it, its Realtek firmware, the chip quirks, and
 * the HCI transport over ugen(4). The console's internal radio (MediaTek 0e8d, Marvell 1286) is
 * never opened beyond its device descriptor: sharing it locks up the DualSense. */

typedef struct {
    char     path[32];          /* /dev/ugenX.Y */
    uint16_t vid, pid;
    char     chip[48];          /* "Realtek RTL8761B", "CSR8510 clone", ... */
    char     address[18];       /* the adapter's own address */
    int      firmware;          /* 1 patch loaded, 0 stayed on ROM, -1 none needed */
    int      fake_csr;          /* a CSR8510 clone: see bring_up */
} usb_hci_info_t;

/* Makes the transport give up its waits once *flag is set (shutdown). */
void usb_hci_set_stop_flag(volatile int *flag);

/* Looks for a Bluetooth adapter that is not the console's radio and not in use by another app.
 * prefer is "vvvv:pppp" to pick one model when several are plugged, or "" for the first.
 * Returns 1 and fills info, with the device open. */
int  usb_hci_find(const char *prefer, usb_hci_info_t *info);

/* Opens its endpoints, resets it, loads the Realtek patch when needed and names it.
 * Returns 1 when the controller answers. */
int  usb_hci_start(const char *local_name, usb_hci_info_t *info);

/* The transport for the host, valid between usb_hci_start and usb_hci_close. */
hci_t usb_hci_transport(void);

/* Writes the transport's counters to the log (when a link stalls or the adapter is lost). */
void usb_hci_diag(void);

void usb_hci_close(void);

/* An adapter that did not start is skipped until it is unplugged. */
void usb_hci_mark_failed(const usb_hci_info_t *info);

/* 1 if the adapter is still on its node with the same ids (not unplugged). */
int  usb_hci_still_there(const usb_hci_info_t *info);

/* Re-enumerates the adapter at the USB level, as unplugging it would. Returns 1 if it was asked. */
int  usb_hci_reset(const char *path);

/* Every Bluetooth adapter on the bus, as a JSON array, for the dashboard. Returns its length. */
int  usb_hci_list_json(char *out, int n);

#endif /* BT_USB_HCI_H */
