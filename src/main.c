#include "version.h"
#include "speaker.h"
#include "launcher.h"
#include "web.h"
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
#include <unistd.h>

#ifdef __PROSPERO__
#include <ps5/kernel.h>
#endif

#define STATE_DIR "/data/bluebird"
#define LOG_PATH  STATE_DIR "/bluebird.log"
#define STOP_FLAG STATE_DIR "/stop"
#define PID_FILE  STATE_DIR "/bluebird.pid"
#define LOG_KEEP  (512 * 1024)

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

/* A copy already running is asked to stop (the flag file), then ended by its pid. Only BlueBird's
 * own pid is touched: HearBridge stops any "payload.elf", which can be another app's. */
static void replace_running_copy(void)
{
    int fd = open(STOP_FLAG, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) close(fd);
    int pfd = open(PID_FILE, O_RDONLY);
    if (pfd >= 0) {
        char buf[32];
        ssize_t r = read(pfd, buf, sizeof(buf) - 1);
        close(pfd);
        if (r > 0) {
            buf[r] = 0;
            pid_t old = (pid_t)atoi(buf);
            if (old > 1 && old != getpid() && kill(old, 0) == 0) {
                log_line("main: stopping the BlueBird already running (pid %d)", (int)old);
                for (int t = 0; t < 30 && kill(old, 0) == 0; t++) usleep(100000);
                if (kill(old, 0) == 0) kill(old, SIGKILL);
                usleep(300000);
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

/* The log is kept across runs for the dashboard; a long one starts over */
static void trim_log(void)
{
    struct stat st;
    if (stat(LOG_PATH, &st) == 0 && st.st_size > LOG_KEEP) {
        char old[64];
        snprintf(old, sizeof(old), "%s.old", LOG_PATH);
        rename(LOG_PATH, old);
    }
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
    log_line("==================================================");
    log_line("%s %s (build %d): %s", BB_NAME, BB_VERSION, BB_BUILD, BB_DESCRIPTION);
#ifdef __PROSPERO__
    uint32_t fw = kernel_get_fw_version();
    log_line("main: firmware %x.%02x (0x%08x), authid 0x%llx", (fw >> 24) & 0xFF, (fw >> 16) & 0xFF, fw,
             (unsigned long long)kernel_get_ucred_authid(-1));
#endif
    replace_running_copy();

    if (!web_init(BB_PORT, LOG_PATH)) log_line("main: the dashboard did not start");
    launcher_install(BB_PORT);
    speaker_init(STATE_DIR);

    char ip[INET_ADDRSTRLEN];
    if (local_ipv4(ip, sizeof(ip))) notify_ps5(ui_text(UI_RUNNING_IP), BB_VERSION, ip, BB_PORT);
    else notify_ps5(ui_text(UI_RUNNING), BB_VERSION);

    while (g_running) {
        if (access(STOP_FLAG, F_OK) == 0) {
            unlink(STOP_FLAG);
            log_line("main: asked to stop by a newer copy");
            break;
        }
        sleep_ms(250);
    }

    log_line("main: shutting down");
    speaker_cleanup();
    web_cleanup();
    log_close();
    return 0;
}
