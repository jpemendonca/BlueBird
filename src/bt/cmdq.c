#include "cmdq.h"

#include "util.h"

#include <string.h>

void bt_cmdq_reset(bt_cmdq_t *q)
{
    q->credits = 1;
    q->sent_at = 0;
    q->head = 0;
    q->count = 0;
}

int bt_cmdq_push(bt_cmdq_t *q, unsigned opcode, const void *params, int plen)
{
    if (plen < 0 || plen > 255 || q->count == BT_CMDQ_LEN) return 0;
    bt_cmd_t *c = &q->q[(q->head + q->count) % BT_CMDQ_LEN];
    c->opcode = (uint16_t)opcode;
    c->len = (uint8_t)plen;
    if (plen) memcpy(c->params, params, (size_t)plen);
    q->count++;
    return 1;
}

void bt_cmdq_on_event(bt_cmdq_t *q, const uint8_t *ev, int len)
{
    /* Every Command Complete and Command Status says how many commands it can take next */
    if (ev[0] == 0x0E && len >= 3) q->credits = ev[2];
    else if (ev[0] == 0x0F && len >= 4) q->credits = ev[3];
}

/* CSR8510 clones answer a command that arrives early with status 0x07 and drop it, an LE Create
 * Connection among them (PS5, 2 Nov). */
void bt_cmdq_flush(bt_cmdq_t *q, bt_cmdq_send_fn send, void *ctx)
{
    if (q->count && q->credits <= 0 && now_ms() - q->sent_at > BT_CMD_REPLY_WAIT_MS) {
        q->credits = 1;
    }
    while (q->count && q->credits > 0) {
        const bt_cmd_t *c = &q->q[q->head];
        q->credits--;
        q->sent_at = now_ms();
        int alive = send(ctx, c->opcode, c->params, c->len);
        q->head = (q->head + 1) % BT_CMDQ_LEN;
        q->count--;
        if (!alive) break;
    }
}
