#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>

/* The console's audio, as Remote Play gets it, through libSceAvcap2: 48 kHz stereo. The NIDs,
 * the open parameters and the format checks come from HearBridge-PS5 (see docs/THIRD_PARTY.md). */

/* The source field the stream opens the capture with (0..7; 2, the Remote Play mix, by default).
 * Taken at the next open. */
void capture_set_source(int source);
int  capture_source(void);

/* The rights the payload started with, tried last when opening the capture (0: none). */
void capture_set_payload_authid(uint64_t authid);

/* Loads the libraries and starts a capture session. Returns 1 on success. */
int  capture_open(void);

/* Reads up to max_frames stereo frames (interleaved s16). Returns frames read, 0 when nothing is
 * ready, -1 once the session is gone. */
int  capture_read(int16_t *out, int max_frames);

void capture_close(void);

/* Closes the session and opens the capture once with each source value, measuring what comes,
 * to find the controller speaker's stream. Takes about 12 s. Returns 1 if it ran. */
int  capture_probe(void);

/* The probe's results as JSON. Returns its length. */
int  capture_probe_json(char *out, int n);

/* What the last open or read said, for the dashboard */
const char *capture_status(void);

#endif /* AUDIO_CAPTURE_H */
