#ifndef A2DP_AVDTP_H
#define A2DP_AVDTP_H

#include <stdint.h>

/* AVDTP messages and the SBC codec choice (A2DP 1.3, AVDTP 1.3). Pure functions, no I/O, so
 * host tests can run every message the stack builds or reads. */

#define PSM_AVDTP 0x0019

enum {
    AVDTP_DISCOVER = 0x01,
    AVDTP_GET_CAPABILITIES = 0x02,
    AVDTP_SET_CONFIGURATION = 0x03,
    AVDTP_GET_CONFIGURATION = 0x04,
    AVDTP_RECONFIGURE = 0x05,
    AVDTP_OPEN = 0x06,
    AVDTP_START = 0x07,
    AVDTP_CLOSE = 0x08,
    AVDTP_SUSPEND = 0x09,
    AVDTP_ABORT = 0x0A,
    AVDTP_SECURITY_CONTROL = 0x0B,
    AVDTP_GET_ALL_CAPABILITIES = 0x0C,
    AVDTP_DELAYREPORT = 0x0D,
};

enum { AVDTP_MSG_CMD = 0, AVDTP_MSG_GEN_REJECT = 1, AVDTP_MSG_ACCEPT = 2, AVDTP_MSG_REJECT = 3 };

#define AVDTP_OUR_SEID 1        /* our one stream end point: an SBC audio source */

typedef struct {
    uint8_t seid, in_use, media, tsep;   /* tsep: 0 source, 1 sink; media 0: audio */
} avdtp_sep_t;

/* One SBC configuration: a single bit set in each field, as in the codec information element. */
typedef struct {
    uint8_t freq;       /* 0x80 16 kHz, 0x40 32, 0x20 44.1, 0x10 48 */
    uint8_t mode;       /* 0x08 mono, 0x04 dual, 0x02 stereo, 0x01 joint stereo */
    uint8_t blocks;     /* 0x80 4, 0x40 8, 0x20 12, 0x10 16 */
    uint8_t subbands;   /* 0x08 4, 0x04 8 */
    uint8_t alloc;      /* 0x02 SNR, 0x01 loudness */
    uint8_t min_bitpool, max_bitpool;
} sbc_choice_t;

/* The highest bitpool we ask for: "high quality" joint stereo at 48 kHz (~345 kbit/s) */
#define SBC_BITPOOL_HQ 53

/* Splits a header byte pair. Returns 1 if d holds at least a header. */
int  avdtp_header(const uint8_t *d, int len, int *label, int *msg_type, int *signal);

/* The stream end points in a Discover answer (d after the 2-byte header). Returns how many. */
int  avdtp_parse_discover(const uint8_t *d, int len, avdtp_sep_t *out, int max);

/* Finds the SBC codec information element (4 bytes) in a list of service capabilities.
 * Returns 1 if there is one. delay_report is set when the sink offers delay reporting. */
int  avdtp_find_sbc(const uint8_t *caps, int len, uint8_t ie[4], int *delay_report);

/* Our pick from what the sink supports: 48 kHz before 44.1, joint stereo, 16 blocks, 8 subbands,
 * loudness, the highest bitpool up to max_bitpool. Returns 0 if nothing usable (16/32 kHz only). */
int  sbc_choose(const uint8_t sink_ie[4], int max_bitpool, sbc_choice_t *out);

/* Reads a configuration the sink set on our end point. Returns 0 if it is not one SBC choice we
 * can encode. */
int  sbc_from_ie(const uint8_t ie[4], sbc_choice_t *out);

void sbc_to_ie(const sbc_choice_t *c, uint8_t ie[4]);

/* Sample rate in Hz, channel count, blocks and subbands of a choice */
int  sbc_rate(const sbc_choice_t *c);
int  sbc_channels(const sbc_choice_t *c);
int  sbc_blocks(const sbc_choice_t *c);
int  sbc_subbands(const sbc_choice_t *c);

/* Builders: each returns the message length written to out (room for 64 bytes is enough). */
int  avdtp_build_cmd(uint8_t *out, int label, int signal, int acp_seid);
int  avdtp_build_set_config(uint8_t *out, int label, int acp_seid, int int_seid, const sbc_choice_t *c);
/* An accept of signal with no parameters */
int  avdtp_build_accept(uint8_t *out, int label, int signal);
/* A reject: for SET_CONFIGURATION and RECONFIGURE the service category goes first */
int  avdtp_build_reject(uint8_t *out, int label, int signal, int category, int error);
int  avdtp_build_general_reject(uint8_t *out, int label, int signal);
/* Our answer to Discover: one SBC audio source, in use once configured */
int  avdtp_build_discover_rsp(uint8_t *out, int label, int in_use);
/* Our capabilities (Get Capabilities and Get All Capabilities): everything the encoder can do */
int  avdtp_build_caps_rsp(uint8_t *out, int label, int signal);
/* Get Configuration: what the stream uses now */
int  avdtp_build_config_rsp(uint8_t *out, int label, const sbc_choice_t *c);

#endif /* A2DP_AVDTP_H */
