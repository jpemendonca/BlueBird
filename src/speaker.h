#ifndef SPEAKER_H
#define SPEAKER_H

/* The Bluetooth worker: finds BlueBird's USB adapter, runs the host on it, reconnects the last
 * speaker, and feeds the stream. The dashboard talks to it through these calls, from its own
 * thread; the worker applies each request on its next turn. */

/* Starts the worker. settings_dir holds speakers.txt and settings.txt. Returns 1 on success. */
int  speaker_init(const char *settings_dir);
void speaker_cleanup(void);

/* Requests from the dashboard. Each returns 1 if it was taken, 0 if not possible now. */
int  speaker_scan(void);
int  speaker_connect(const char *addr);       /* "AA:BB:CC:DD:EE:FF" */
int  speaker_disconnect(void);
int  speaker_forget(const char *addr);
void speaker_set_auto(int on);                /* reconnect the last speaker by itself */
void speaker_set_volume(int pct);
void speaker_test_tone(void);
/* The capture source the stream uses (0..7, 2 by default): an experiment to find the
 * controller speaker's audio. Saved with the settings. */
void speaker_set_capture_source(int source);
/* "vvvv:pppp" to use that adapter model when several are plugged, "" for any. Restarts Bluetooth. */
void speaker_set_adapter(const char *prefer);

/* Everything the dashboard shows, as JSON. Returns its length. */
int  speaker_status_json(char *out, int n);

#endif /* SPEAKER_H */
