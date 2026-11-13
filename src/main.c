#include "version.h"
#include "speaker.h"
#include "launcher.h"
#include "web.h"
#include "audio/capture.h"
#include "log.h"
#include "ui_text.h"
#include "util.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#include <sys/sysctl.h>
#endif

#define STATE_DIR "/data/bluebird"
#define LOG_PATH  STATE_DIR "/bluebird.log"
#define STOP_FLAG STATE_DIR "/stop"
#define PID_FILE  STATE_DIR "/bluebird.pid"
#define GAP_MS    3000

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* The console's LAN address for the start notification: the first IPv4 interface up and not loopback */
static int local_ipv4(char *out, size_t n)
{
    struct ifaddrs *list, *ifa;
    int found = 0;
    if (getifaddrs(&list) != 0) return 0;
    for (ifa = list; ifa && !found; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        if (!(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) continue;
        found = inet_ntop(AF_INET, &((struct sockaddr_in *)ifa->ifa_addr)->sin_addr, out, (socklen_t)n) != NULL;
    }
    freeifaddrs(list);
    return found;
}

/* A process's name, as the PS5 kernel lays out kinfo_proc (the offsets klogsrv and HearBridge use).
 * Returns 1 if it was read. */
static int process_name(pid_t pid, char *out, size_t n)
{
#ifdef __PROSPERO__
    static unsigned char buf[4096];
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
    size_t sz = sizeof(buf);
    if (sysctl(mib, 4, buf, &sz, NULL, 0) != 0 || sz < 447 + 20) return 0;
    snprintf(out, n, "%.19s", (const char *)buf + 447);
    return out[0] != 0;
#else
    (void)pid;
    (void)out;
    (void)n;
    return 0;
#endif
}

/* A copy already running is asked to stop with the flag file. If it is still there after 3 s, it is
 * ended by its pid, but only when that pid carries our own process name: after a restart the pid
 * in the file can belong to anything. */
static void replace_running_copy(void)
{
    int fd = open(STOP_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) close(fd);
    int pfd = open(PID_FILE, O_RDONLY);
    if (pfd >= 0) {
        char buf[32];
        ssize_t r = read(pfd, buf, sizeof(buf) - 1);
        close(pfd);
        pid_t old = r > 0 ? (pid_t)atoi((buf[r] = 0, buf)) : 0;
        if (old > 1 && old != getpid() && kill(old, 0) == 0) {
            char mine[24] = "", theirs[24] = "";
            int named = process_name(getpid(), mine, sizeof(mine)) && process_name(old, theirs, sizeof(theirs));
            log_line("main: pid %d from the last run is alive (\"%s\", we are \"%s\")", (int)old, theirs, mine);
            if (named && strcmp(mine, theirs) == 0) {
                int t = 0;
                for (; t < 30 && kill(old, 0) == 0; t++) usleep(100000);
                if (kill(old, 0) == 0) {
                    log_line("main: the old copy did not stop in 3 s, ending it");
                    kill(old, SIGKILL);
                    usleep(300000);
                } else {
                    log_line("main: the old copy stopped after %d ms", t * 100);
                }
            } else {
                log_line("main: that pid is another process, left alone");
                usleep(500000);
            }
        }
    }
    unlink(STOP_FLAG);
    pfd = open(PID_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (pfd >= 0) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
        if (write(pfd, buf, (size_t)n) != n) log_line("main: pid file not written");
        close(pfd);
    }
}

/* The log is kept across runs for the dashboard; a long one starts over (log.c does the same
 * while running) */
static void trim_log(void)
{
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        char old[64];
        snprintf(old, sizeof(old), "%s.old", LOG_PATH);
        rename(LOG_PATH, old);
    }
}

/* The rights OmniPad runs its USB adapter with (PS5); the capture tries the payload's own as well */
static void log_system_and_elevate(void)
{
    char name[24] = "?";
    process_name(getpid(), name, sizeof(name));
    log_line("main: pid %d, parent %d, uid %d, process \"%s\"", (int)getpid(), (int)getppid(), (int)getuid(), name);
#ifdef __PROSPERO__
    uint32_t fw = kernel_get_fw_version();
    uint64_t authid = kernel_get_ucred_authid(-1);
    log_line("main: firmware %x.%02x (0x%08x), authid 0x%016llx", (fw >> 24) & 0xFF, (fw >> 16) & 0xFF, fw,
             (unsigned long long)authid);
    capture_set_payload_authid(authid);
    uint8_t caps[16];
    memset(caps, 0xff, sizeof(caps));
    int a = kernel_set_ucred_authid(-1, 0x4800000000010003ULL);
    int c = kernel_set_ucred_caps(-1, caps);
    log_line("main: rights raised (authid %s, caps %s), authid now 0x%016llx", a == 0 ? "ok" : "refused",
             c == 0 ? "ok" : "refused", (unsigned long long)kernel_get_ucred_authid(-1));
#endif
}

int main(void)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    mkdir("/data", 0755);
    mkdir(STATE_DIR, 0755);
    trim_log();
    log_init(LOG_PATH);
    log_thread_name("main");
    log_catch_crashes(ui_text(UI_CRASH));

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    log_line("==================================================");
    log_line("%s %s (build %d), %04d-%02d-%02d: %s", BB_NAME, BB_VERSION, BB_BUILD, tm.tm_year + 1900,
             tm.tm_mon + 1, tm.tm_mday, BB_DESCRIPTION);

    /* The first thing the user sees: the payload did start */
    char ip[INET_ADDRSTRLEN];
    int have_ip = local_ipv4(ip, sizeof(ip));
    if (have_ip) notify_ps5(ui_text(UI_RUNNING_IP), BB_VERSION, ip, BB_PORT);
    else notify_ps5(ui_text(UI_RUNNING), BB_VERSION);
    log_line("main: address %s, dashboard port %d, language %s", have_ip ? ip : "(none)", BB_PORT, ui_lang_code());

    log_step("replacing a running copy");
    replace_running_copy();
    log_step("raising rights");
    log_system_and_elevate();

    log_step("starting the dashboard");
    if (!web_init(BB_PORT, LOG_PATH)) log_line("main: the dashboard did not start");
    log_step("installing the tile");
    launcher_install(BB_PORT);
    log_step("starting Bluetooth");
    speaker_init(STATE_DIR);
    log_step("running");
    log_line("main: started, waiting for the stop flag");

    long last = now_ms();
    while (g_running) {
        if (access(STOP_FLAG, F_OK) == 0) {
            unlink(STOP_FLAG);
            log_line("main: asked to stop by a newer copy");
            break;
        }
        sleep_ms(250);
        long now = now_ms();
        /* A loop that ticks every 250 ms going silent for seconds means the console slept */
        if (now - last > GAP_MS) log_line("main: the loop stood still for %ld ms (rest mode?)", now - last);
        last = now;
    }

    log_step("stopping");
    log_line("main: shutting down");
    speaker_cleanup();
    web_cleanup();
    notify_ps5("%s", ui_text(UI_STOPPED));
    log_line("main: stopped");
    log_close();
    return 0;
}
