#ifndef AUDIO_STREAM_H
#define AUDIO_STREAM_H

#include "../a2dp/avdtp.h"

#include <stdint.h>

/* From the console's audio to the speaker: a capture thread fills a ring, and the Bluetooth
 * worker takes packets out of it in real time, encodes them and sends them. A short ring keeps
 * the delay low; when it runs dry the speaker gets silence, never a stall. */

/* Sends one media packet. Returns 1 sent, 0 dropped (no room in the adapter), -1 not streaming. */
typedef int (*stream_send_fn)(void *ctx, const uint8_t *pkt, int len);

void stream_init(void);

/* The stream is playing with this configuration and media MTU: start packing. */
void stream_start(const sbc_choice_t *cfg, int mtu);

/* The stream stopped (suspended, closed, link down). */
void stream_stop(void);

/* Sends the packets that are due. Call after every poll of the host. */
void stream_tick(stream_send_fn send, void *ctx);

/* How long the worker may wait before the next packet is due, in ms (at most `cap`). */
int  stream_wait_ms(int cap);

/* Keeps the capture running (1) or lets it close after a while (0). */
void stream_want_capture(int on);

/* Uses another capture source from now on (capture_set_source), reopening the session. */
void stream_set_capture_source(int source);

/* Runs the capture source probe (capture_probe) on the capture thread. Returns 0 if one runs. */
int  stream_probe_capture(void);

/* Volume 0..100, applied before encoding. */
void stream_set_volume(int pct);
int  stream_volume(void);

/* Plays a 440 Hz tone instead of the console's audio for `ms`. */
void stream_test_tone(int ms);

/* Writes the stream's numbers as a JSON object. Returns its length. */
int  stream_status_json(char *out, int n);

/* For host tests: puts PCM into the ring as the capture thread would. */
void stream_feed(const int16_t *pcm, int frames);

#endif /* AUDIO_STREAM_H */
