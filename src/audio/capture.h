#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>

/* The console's audio, as Remote Play gets it, through libSceAvcap2: 48 kHz stereo. The NIDs,
 * the open parameters and the format checks come from HearBridge-PS5 (see docs/THIRD_PARTY.md). */

/* Loads the libraries and starts a capture session. Returns 1 on success. */
int  capture_open(void);

/* Reads up to max_frames stereo frames (interleaved s16). Returns frames read, 0 when nothing is
 * ready, -1 once the session is gone. */
int  capture_read(int16_t *out, int max_frames);

void capture_close(void);

/* What the last open or read said, for the dashboard */
const char *capture_status(void);

#endif /* AUDIO_CAPTURE_H */
