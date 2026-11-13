#ifndef WEB_H
#define WEB_H

/* The dashboard: the page and its HTTP API on `port`, served from its own thread.
 *   GET  /                 the page (web/index.html)
 *   GET  /start            the tile's page (web/start.html), kept by the browser through
 *                          /bluebird.appcache: it opens / when BlueBird answers /ping or /ping.gif,
 *                          and says how to start BlueBird when it does not
 *   GET  /api/state        adapter, speaker, stream, search results, saved speakers
 *   GET  /api/log          the last 32 KB of the log; ?since=<offset> for what follows it
 *   POST /api/scan         search for speakers
 *   POST /api/connect?addr=AA:BB:..   connect (and pair) a speaker
 *   POST /api/disconnect
 *   POST /api/forget?addr=AA:BB:..
 *   POST /api/volume?v=0..100
 *   POST /api/tone         a 3 s test tone on the speaker
 *   POST /api/probe        try each capture source, to find the controller speaker's audio
 *   POST /api/source?s=0..7  the capture source the stream uses (2, the Remote Play mix, by default)
 *   POST /api/auto?on=0|1  reconnect the last speaker by itself
 *   POST /api/adapter?prefer=vvvv:pppp   (empty for any) */

/* Starts the server. log_path is the file /api/log reads. Returns 1 on success. */
int  web_init(int port, const char *log_path);
void web_cleanup(void);

#endif /* WEB_H */
