#ifndef A2DP_STORE_H
#define A2DP_STORE_H

#include <stdint.h>

/* The paired speakers: address, name, link key, and when each was last used. Kept in a text
 * file, safe to call from any thread. Addresses are in HCI order (least significant byte first). */

#define STORE_MAX 8

typedef struct {
    uint8_t addr[6];
    char    name[64];
    uint8_t key[16];
    int     has_key;
    long    last_used;      /* seconds since the epoch, 0: never */
} store_entry_t;

/* Loads the file; a missing file is an empty store. */
void store_init(const char *path);

/* Copies the link key of addr. Returns 1 if there is one. */
int  store_get_key(const uint8_t addr[6], uint8_t key[16]);

/* Adds or updates a speaker and saves. key may be NULL to keep the one stored; name may be NULL or "". */
void store_put(const uint8_t addr[6], const char *name, const uint8_t *key);

/* Drops the key of addr (the speaker forgot us) and saves. */
void store_drop_key(const uint8_t addr[6]);

/* Marks addr as used now, so it becomes the speaker reconnected at start-up. */
void store_touch(const uint8_t addr[6]);

/* Removes a speaker. Returns 1 if it was there. */
int  store_forget(const uint8_t addr[6]);

/* 1 if addr is stored. */
int  store_has(const uint8_t addr[6]);

/* The speaker used most recently. Returns 1 and fills out, 0 if the store is empty. */
int  store_last(store_entry_t *out);

/* The stored speakers, most recent first. Returns how many were copied. */
int  store_list(store_entry_t *out, int max);

/* "AA:BB:CC:DD:EE:FF" from an address in HCI order (out holds 18 bytes). */
void store_addr_str(const uint8_t addr[6], char *out);

/* Parses "AA:BB:CC:DD:EE:FF" into HCI order. Returns 1 on success. */
int  store_addr_parse(const char *s, uint8_t addr[6]);

#endif /* A2DP_STORE_H */
