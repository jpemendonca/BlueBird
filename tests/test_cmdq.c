/* The HCI command queue (src/bt/cmdq.c): one command per credit the controller gives */
#include "check.h"
#include "cmdq.h"

#include <string.h>

static int sent_ops[16], nsent;

static int fake_send(void *ctx, uint16_t opcode, const uint8_t *params, int len)
{
    (void)ctx; (void)params; (void)len;
    sent_ops[nsent++] = opcode;
    return 1;
}

static void test_cmdq_flush_oneCredit_sendsOneCommand(void)
{
    /* arrange */
    static bt_cmdq_t q;
    bt_cmdq_reset(&q);
    nsent = 0;
    bt_cmdq_push(&q, 0x0401, NULL, 0);
    bt_cmdq_push(&q, 0x0405, NULL, 0);

    /* act */
    bt_cmdq_flush(&q, fake_send, NULL);

    /* assert */
    CHECK(nsent == 1 && sent_ops[0] == 0x0401, "a fresh controller takes one command, sent %d", nsent);
}

static void test_cmdq_onEvent_commandStatus_releasesNext(void)
{
    /* arrange */
    static bt_cmdq_t q;
    bt_cmdq_reset(&q);
    nsent = 0;
    bt_cmdq_push(&q, 0x0401, NULL, 0);
    bt_cmdq_push(&q, 0x0405, NULL, 0);
    bt_cmdq_flush(&q, fake_send, NULL);
    const uint8_t status[6] = { 0x0F, 4, 0x00, 1, 0x01, 0x04 };

    /* act */
    bt_cmdq_on_event(&q, status, 6);
    bt_cmdq_flush(&q, fake_send, NULL);

    /* assert */
    CHECK(nsent == 2 && sent_ops[1] == 0x0405, "a Command Status with one credit lets the next command go, sent %d", nsent);
}

static void test_cmdq_push_full_refuses(void)
{
    /* arrange */
    static bt_cmdq_t q;
    bt_cmdq_reset(&q);
    for (int i = 0; i < BT_CMDQ_LEN; i++) bt_cmdq_push(&q, 0x0C03, NULL, 0);

    /* act */
    int ok = bt_cmdq_push(&q, 0x0C03, NULL, 0);

    /* assert */
    CHECK(ok == 0, "a full queue refuses another command");
}

int main(void)
{
    test_cmdq_flush_oneCredit_sendsOneCommand();
    test_cmdq_onEvent_commandStatus_releasesNext();
    test_cmdq_push_full_refuses();
    return REPORT("test_cmdq");
}
