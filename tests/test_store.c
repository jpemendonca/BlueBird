/* The paired speakers file (src/a2dp/store.c) */
#include "check.h"
#include "store.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PATH "build/host/test_store.txt"

static void test_store_put_reload_keepsKeyAndName(void)
{
    /* arrange */
    unlink(PATH);
    store_init(PATH);
    uint8_t a[6], key[16], got[16];
    store_addr_parse("11:22:33:44:55:66", a);
    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(i * 7);

    /* act */
    store_put(a, "JBL Flip 5", key);
    store_touch(a);
    store_init(PATH);

    /* assert */
    store_entry_t e;
    CHECK(store_get_key(a, got) && memcmp(got, key, 16) == 0, "the link key survives a reload");
    CHECK(store_last(&e) && strcmp(e.name, "JBL Flip 5") == 0, "the name with its space survives, got \"%s\"", e.name);
    char s[18];
    store_addr_str(e.addr, s);
    CHECK(strcmp(s, "11:22:33:44:55:66") == 0, "the address is written most significant byte first, got %s", s);
}

static void test_store_drop_key_keepsSpeaker(void)
{
    /* arrange */
    unlink(PATH);
    store_init(PATH);
    uint8_t a[6], key[16] = { 1 }, got[16];
    store_addr_parse("AA:BB:CC:DD:EE:FF", a);
    store_put(a, "Speaker", key);

    /* act */
    store_drop_key(a);

    /* assert */
    CHECK(!store_get_key(a, got), "no key after the speaker forgot us");
    CHECK(store_has(a), "the speaker stays listed, to pair again");
}

static void test_store_last_twoSpeakers_mostRecentFirst(void)
{
    /* arrange */
    unlink(PATH);
    store_init(PATH);
    uint8_t a[6], b[6];
    store_addr_parse("00:00:00:00:00:01", a);
    store_addr_parse("00:00:00:00:00:02", b);
    store_put(a, "A", NULL);
    store_put(b, "B", NULL);
    store_touch(a);
    sleep(1);

    /* act */
    store_touch(b);

    /* assert */
    store_entry_t e;
    CHECK(store_last(&e) && strcmp(e.name, "B") == 0, "the one used last is reconnected first, got %s", e.name);
}

int main(void)
{
    test_store_put_reload_keepsKeyAndName();
    test_store_drop_key_keepsSpeaker();
    test_store_last_twoSpeakers_mostRecentFirst();
    return REPORT("test_store");
}
