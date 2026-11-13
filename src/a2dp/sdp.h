#ifndef A2DP_SDP_H
#define A2DP_SDP_H

#include <stdint.h>

/* A small SDP server with one record: an A2DP audio source on AVDTP. Speakers that reconnect
 * often ask the source for it before they open AVDTP. */

#define PSM_SDP 0x0001

/* Answers one SDP request PDU. Returns the length of the answer written to out (at most max),
 * or 0 if the request is not worth an answer. */
int sdp_answer(const uint8_t *req, int len, uint8_t *out, int max);

#endif /* A2DP_SDP_H */
