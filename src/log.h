#ifndef LOG_H
#define LOG_H

#include <stddef.h>

/* Past this size the log file becomes <path>.old and starts over, so at most two of them are kept */
#define LOG_MAX_BYTES (1024 * 1024)

/* Opens (appends to) the log file. Every line also goes to stdout and the kernel log. */
void log_init(const char *path);

/* One log line, with time and the calling thread's name. */
void log_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void log_close(void);

/* A PS5 notification (stdout on the PC). */
void notify_ps5(const char *fmt, ...);

/* Names the calling thread in log lines and crash reports. The string must stay valid. */
void log_thread_name(const char *name);

/* What the program is doing now, told in a crash report. The string must stay valid. */
void log_step(const char *step);

/* Writes a report to the log and notifies the user if the process dies on a signal
 * (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT). crash_text is the notification, with one %d. */
void log_catch_crashes(const char *crash_text);

#endif /* LOG_H */
