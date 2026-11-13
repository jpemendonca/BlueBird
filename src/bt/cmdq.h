#ifndef BT_CMDQ_H
#define BT_CMDQ_H

#include <stdint.h>

/* The turn of HCI commands: one is sent only when the controller has room for it. Apart from
 * usb_hci.c because that file needs USB, and this rule has to run in host tests. */

#define BT_CMDQ_LEN 32
#define BT_CMD_REPLY_WAIT_MS 500  /* a controller that never gives the turn back gets the next command anyway */

typedef struct {
    uint16_t opcode;
    uint8_t  len;
    uint8_t  params[255];
} bt_cmd_t;

typedef struct {
    int      credits;   /* commands the controller can take now (Num_HCI_Command_Packets) */
    long     sent_at;
    bt_cmd_t q[BT_CMDQ_LEN];
    int      head, count;
} bt_cmdq_t;

/* Sends one command to the controller. Returns 0 once the transport is gone. */
typedef int (*bt_cmdq_send_fn)(void *ctx, uint16_t opcode, const uint8_t *params, int len);

/* Empties the queue and gives the controller one credit, as it has when it starts. */
void bt_cmdq_reset(bt_cmdq_t *q);

/* Adds a command at the end. Returns 0 if the queue is full or plen is out of range. */
int  bt_cmdq_push(bt_cmdq_t *q, unsigned opcode, const void *params, int plen);

/* Takes the credits from an HCI event (code first). Call for every event that arrives. */
void bt_cmdq_on_event(bt_cmdq_t *q, const uint8_t *ev, int len);

/* Sends waiting commands while the controller has room. */
void bt_cmdq_flush(bt_cmdq_t *q, bt_cmdq_send_fn send, void *ctx);

#endif /* BT_CMDQ_H */
