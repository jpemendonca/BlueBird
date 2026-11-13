#ifndef A2DP_HOST_H
#define A2DP_HOST_H

#include "../bt/hci.h"
#include "avdtp.h"

#include <stdint.h>

/* The Bluetooth Classic host for one A2DP sink (a speaker or headphones), on BlueBird's own
 * adapter. One thread: host_poll pumps the transport and moves the link along.
 *
 *   search:     host_scan inquires; every audio device found is reported with its name.
 *   connect:    host_connect pages a speaker, pairs it (Secure Simple Pairing, no input/output,
 *               or PIN 0000), encrypts, and sets up the AVDTP stream up to Streaming.
 *   call-back:  a stored speaker switched on may call us; it is accepted and the stream is set
 *               up by whichever side starts first.
 */

typedef struct host host_t;

enum {
    HOST_IDLE,          /* no link */
    HOST_PAGING,        /* calling the speaker */
    HOST_SECURING,      /* pairing or encrypting */
    HOST_SIGNALING,     /* opening AVDTP */
    HOST_CONFIGURING,   /* choosing the codec */
    HOST_OPENING,       /* opening the media channel */
    HOST_READY,         /* stream open, not playing */
    HOST_STREAMING,
};

typedef struct {
    /* A device found by host_scan. audio: its class says it plays sound. */
    void (*on_found)(void *ud, const uint8_t addr[6], const char *name, uint32_t cod, int rssi, int audio);
    /* The search ended */
    void (*on_scan_done)(void *ud);
    /* The link came up (state HOST_SECURING) or went down (HOST_IDLE), or the stream changed */
    void (*on_state)(void *ud, int state);
    void *ud;
} host_events;

typedef struct {
    int      state;
    uint8_t  addr[6];
    char     name[64];
    int      incoming;          /* the speaker called us */
    sbc_choice_t cfg;           /* valid from HOST_READY */
    int      mtu;               /* the sink's media MTU, from HOST_READY */
    int      acl_mtu, acl_bufs, acl_free;
    unsigned media_sent, media_dropped;
    char     last_error[96];
} host_status_t;

/* Sets the controller up (event mask, Simple Pairing, page scan, buffers). Returns NULL if it
 * does not answer. fake_csr: a CSR8510 clone, which gets no commands while it streams. */
host_t *host_open(hci_t hci, const host_events *ev, int fake_csr);
void    host_close(host_t *h);

/* Pumps the transport for up to timeout_ms and moves the link along. Returns -1 once the
 * transport is gone. */
int  host_poll(host_t *h, int timeout_ms);

/* Searches for audio devices for `seconds`. Returns 0 while a link is being set up. */
int  host_scan(host_t *h, int seconds);
int  host_scanning(const host_t *h);

/* Calls a speaker and sets up its stream. Returns 0 if a link exists already or a call is going on. */
int  host_connect(host_t *h, const uint8_t addr[6], const char *name);

/* Drops the link. */
void host_disconnect(host_t *h);

/* Sends one media packet (RTP header and SBC frames) while streaming. Returns 1 if sent, 0 if the
 * controller has no room (the packet is dropped), -1 if not streaming. */
int  host_send_media(host_t *h, const uint8_t *pkt, int len);

/* Asks the sink to start the stream again after it suspended it. */
void host_resume(host_t *h);

void host_status(const host_t *h, host_status_t *out);

const char *host_state_name(int state);

#endif /* A2DP_HOST_H */
