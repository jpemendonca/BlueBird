#ifndef A2DP_MEDIA_H
#define A2DP_MEDIA_H

#include "avdtp.h"
#include "sbc_enc.h"

#include <stdint.h>

/* A2DP media packets: 48 kHz stereo PCM in, RTP packets of SBC frames out (A2DP 1.3 section 4.3).
 * Converts to 44.1 kHz or mono when the sink asked for it. */

#define MEDIA_IN_RATE 48000

/* Gives `frames` stereo frames at 48 kHz, interleaved s16. Always fills them (silence if need be). */
typedef void (*media_pull_fn)(void *ud, int16_t *pcm, int frames);

typedef struct {
    hb_sbc_state st;
    sbc_choice_t cfg;
    int      rate, channels, samples;   /* samples per channel in one SBC frame */
    int      frame_len, mtu, per_pkt;
    uint16_t seq;
    uint32_t ts;
    int      bitpool;
    /* 48 -> 44.1 kHz: position of the next output sample between in[0] and in[1], in 1/2^16 */
    uint32_t frac;
    int16_t  last[2];
    int      have_last;
} media_t;

/* Sets up the encoder for a configuration and the sink's L2CAP MTU. Returns 0 if the encoder
 * refuses it. Starts at the configuration's highest bitpool. */
int  media_setup(media_t *m, const sbc_choice_t *cfg, int mtu);

/* Changes the bitpool, kept within the configuration. Returns the bitpool in use. */
int  media_set_bitpool(media_t *m, int bitpool);

/* How long one packet plays, in microseconds */
long media_packet_us(const media_t *m);

/* Builds the next packet, pulling the PCM it needs. Returns its length, or -1 if out is too small. */
int  media_build(media_t *m, media_pull_fn pull, void *ud, uint8_t *out, int max);

#endif /* A2DP_MEDIA_H */
