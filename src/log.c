#include "log.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#include <ps5/klog.h>
#endif

typedef struct {
    char reserved[45];
    char message[3075];
} notify_request_t;

#ifdef __PROSPERO__
extern int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);
#endif

static FILE *g_log_file = NULL;
static char g_log_path[200];
static int g_log_fd = -1;                 /* the same file, for the crash handler */
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static __thread const char *t_name;
static const char *volatile g_step = "start-up";
static const char *g_crash_text;

void log_init(const char *path)
{
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) fclose(g_log_file);
    snprintf(g_log_path, sizeof(g_log_path), "%s", path);
    g_log_file = fopen(path, "a");
    g_log_fd = g_log_file ? fileno(g_log_file) : -1;
    pthread_mutex_unlock(&g_log_mutex);
}

void log_thread_name(const char *name)
{
    t_name = name;
}

void log_step(const char *step)
{
    g_step = step;
}

void log_line(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    struct timeval tv;
    struct tm tm;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm);
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    char line[1100];
    snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s%s%s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
             (int)(tv.tv_usec / 1000), t_name ? t_name : "", t_name ? " | " : "", buf);
    pthread_mutex_lock(&g_log_mutex);
    fputs(line, stdout);
    fflush(stdout);
    if (g_log_file) {
        fputs(line, g_log_file);
        fflush(g_log_file);
        if (ftell(g_log_file) > LOG_MAX_BYTES) {
            char old[220];
            snprintf(old, sizeof(old), "%s.old", g_log_path);
            fclose(g_log_file);
            rename(g_log_path, old);
            g_log_file = fopen(g_log_path, "a");
            g_log_fd = g_log_file ? fileno(g_log_file) : -1;
        }
    }
    pthread_mutex_unlock(&g_log_mutex);
#ifdef __PROSPERO__
    klog_printf("[BlueBird] %s\n", buf);
#endif
}

void log_close(void)
{
    pthread_mutex_lock(&g_log_mutex);
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
        g_log_fd = -1;
    }
    pthread_mutex_unlock(&g_log_mutex);
}

void notify_ps5(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    log_line("notify: %s", msg);
#ifdef __PROSPERO__
    notify_request_t req;
    memset(&req, 0, sizeof(req));
    snprintf(req.message, sizeof(req.message), "%s", msg);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
#endif
}

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSEGV: return "SIGSEGV (bad memory access)";
    case SIGBUS:  return "SIGBUS (bad memory access)";
    case SIGILL:  return "SIGILL (bad instruction)";
    case SIGFPE:  return "SIGFPE (arithmetic error)";
    case SIGABRT: return "SIGABRT (abort)";
    default:      return "signal";
    }
}

/* Plain writes only: the stdio lock may be held by the thread that crashed */
static void on_crash(int sig, siginfo_t *info, void *uc)
{
    (void)uc;
    char line[300];
    int n = snprintf(line, sizeof(line), "!!! crash: %s (%d) in thread \"%s\" at address %p, step \"%s\"\n",
                     signal_name(sig), sig, t_name ? t_name : "main", info ? info->si_addr : NULL, g_step);
    if (g_log_fd >= 0 && n > 0) {
        ssize_t w = write(g_log_fd, line, (size_t)n);
        (void)w;
    }
    ssize_t w = write(2, line, n > 0 ? (size_t)n : 0);
    (void)w;
#ifdef __PROSPERO__
    klog_printf("[BlueBird] %s", line);
    if (g_crash_text) {
        notify_request_t req;
        memset(&req, 0, sizeof(req));
        snprintf(req.message, sizeof(req.message), g_crash_text, sig);
        sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
    }
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

void log_catch_crashes(const char *crash_text)
{
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };
    g_crash_text = crash_text;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&sa.sa_mask);
    for (int i = 0; i < 5; i++) sigaction(sigs[i], &sa, NULL);
}
