#include "web.h"
#include "speaker.h"
#include "ui_text.h"
#include "version.h"
#include "log.h"
#include "util.h"
#include "web_index.h"  /* generated from web/index.html by tools/embed_web.py */

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAIL 32768

static struct {
    int          fd, port;
    pthread_t    thread;
    volatile int running;
    long         started;
    char         log_path[200];
} g_web = { .fd = -1 };

static void write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w <= 0) return;
        p += w;
        len -= (size_t)w;
    }
}

static void respond(int fd, int code, const char *type, const char *extra, const void *body, size_t len)
{
    char hdr[512];
    const char *text = code == 200 ? "OK" : code == 404 ? "Not Found" : code == 409 ? "Conflict" : "Bad Request";
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\nAccess-Control-Expose-Headers: X-Log-Offset\r\n%sConnection: close\r\n\r\n",
                     code, text, type, len, extra ? extra : "");
    write_all(fd, hdr, (size_t)n);
    if (len) write_all(fd, body, len);
}

static void respond_ok(int fd, int ok)
{
    const char *body = ok ? "{\"ok\":true}" : "{\"ok\":false}";
    respond(fd, ok ? 200 : 409, "application/json", NULL, body, strlen(body));
}

/* The value of `key` in a query string, URL-decoded */
static int query_get(const char *query, const char *key, char *out, size_t cap)
{
    size_t klen = strlen(key);
    for (const char *p = query; p && *p;) {
        if (!strncmp(p, key, klen) && p[klen] == '=') {
            p += klen + 1;
            size_t o = 0;
            while (*p && *p != '&' && o + 1 < cap) {
                if (*p == '%' && p[1] && p[2]) {
                    char hex[3] = { p[1], p[2], 0 };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    p += 3;
                } else {
                    out[o++] = *p == '+' ? ' ' : *p;
                    p++;
                }
            }
            out[o] = 0;
            return 1;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    return 0;
}

static void send_state(int fd)
{
    static char body[20000];
    int n = snprintf(body, sizeof(body), "{\"version\":\"%s\",\"build\":%d,\"lang\":\"%s\",\"port\":%d,\"uptime\":%ld,",
                     BB_VERSION, BB_BUILD, ui_lang_code(), g_web.port, (now_ms() - g_web.started) / 1000);
    char *rest = body + n;
    int m = speaker_status_json(rest, (int)sizeof(body) - n);
    if (m > 0 && rest[0] == '{') {
        memmove(rest, rest + 1, (size_t)m);     /* its opening brace is ours */
        n += m - 1;
    }
    respond(fd, 200, "application/json", NULL, body, (size_t)n);
}

static void send_log(int fd, const char *query)
{
    static char buf[LOG_TAIL];
    char since_s[24];
    struct stat st;
    long size = stat(g_web.log_path, &st) == 0 ? (long)st.st_size : 0;
    long from = size > LOG_TAIL ? size - LOG_TAIL : 0;
    if (query_get(query, "since", since_s, sizeof(since_s))) {
        long since = atol(since_s);
        if (since >= 0 && since <= size) from = since;
        if (size - from > LOG_TAIL) from = size - LOG_TAIL;
    }
    size_t got = 0;
    FILE *f = fopen(g_web.log_path, "rb");
    if (f) {
        fseek(f, from, SEEK_SET);
        got = fread(buf, 1, sizeof(buf), f);
        fclose(f);
    }
    char extra[64];
    snprintf(extra, sizeof(extra), "X-Log-Offset: %ld\r\n", from + (long)got);
    respond(fd, 200, "text/plain; charset=utf-8", extra, buf, got);
}

static void handle(int fd)
{
    char req[4096];
    int n = 0;
    struct pollfd p = { fd, POLLIN, 0 };
    while (n < (int)sizeof(req) - 1 && poll(&p, 1, 2000) > 0) {
        ssize_t r = read(fd, req + n, sizeof(req) - 1 - (size_t)n);
        if (r <= 0) break;
        n += (int)r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    if (n <= 0) return;
    req[n] = 0;
    char method[8], target[1024];
    if (sscanf(req, "%7s %1023s", method, target) != 2) {
        respond(fd, 400, "text/plain", NULL, "bad request", 11);
        return;
    }
    static char none[1];
    char *query = strchr(target, '?');
    if (query) *query++ = 0;
    else query = none;
    int post = !strcmp(method, "POST");
    char v[64];
    if (post) log_line("web: POST %s%s%s", target, query[0] ? "?" : "", query);

    if (!strcmp(target, "/") || !strcmp(target, "/index.html")) {
        respond(fd, 200, "text/html; charset=utf-8", NULL, HTML_INDEX, HTML_INDEX_LEN);
    } else if (!strcmp(target, "/api/state")) {
        send_state(fd);
    } else if (!strcmp(target, "/api/log")) {
        send_log(fd, query);
    } else if (!post && !strncmp(target, "/api/", 5)) {
        respond(fd, 400, "text/plain", NULL, "use POST", 8);
    } else if (!strcmp(target, "/api/scan")) {
        respond_ok(fd, speaker_scan());
    } else if (!strcmp(target, "/api/connect")) {
        respond_ok(fd, query_get(query, "addr", v, sizeof(v)) && speaker_connect(v));
    } else if (!strcmp(target, "/api/disconnect")) {
        respond_ok(fd, speaker_disconnect());
    } else if (!strcmp(target, "/api/forget")) {
        respond_ok(fd, query_get(query, "addr", v, sizeof(v)) && speaker_forget(v));
    } else if (!strcmp(target, "/api/volume")) {
        int ok = query_get(query, "v", v, sizeof(v));
        if (ok) speaker_set_volume(atoi(v));
        respond_ok(fd, ok);
    } else if (!strcmp(target, "/api/tone")) {
        speaker_test_tone();
        respond_ok(fd, 1);
    } else if (!strcmp(target, "/api/auto")) {
        int ok = query_get(query, "on", v, sizeof(v));
        if (ok) speaker_set_auto(atoi(v));
        respond_ok(fd, ok);
    } else if (!strcmp(target, "/api/adapter")) {
        if (!query_get(query, "prefer", v, sizeof(v))) v[0] = 0;
        speaker_set_adapter(v);
        respond_ok(fd, 1);
    } else {
        respond(fd, 404, "text/plain", NULL, "not found", 9);
    }
}

static int open_listener(int quiet)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons((uint16_t)g_web.port);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    int bound = 0;
    for (int tries = 0; tries < 20 && !bound; tries++) {
        bound = bind(fd, (struct sockaddr *)&sin, sizeof(sin)) == 0;
        if (!bound) usleep(250000);     /* a copy being replaced still holds the port */
    }
    if (!bound || listen(fd, 8) != 0) {
        if (!quiet) log_line("web: cannot listen on port %d (%s)", g_web.port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void *server(void *arg)
{
    (void)arg;
    while (g_web.running) {
        struct pollfd p = { g_web.fd, POLLIN, 0 };
        int r = poll(&p, 1, 500);
        if (r == 0) continue;
        int c = r > 0 ? accept(g_web.fd, NULL, NULL) : -1;
        if (c < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            /* Rest mode closes the payload's sockets: OmniPad's dashboard stayed refused after the
             * console woke until the socket was opened again (PS5, 27 Oct) */
            log_line("web: listening socket lost (%s), reopening", strerror(errno));
            close(g_web.fd);
            g_web.fd = -1;
            while (g_web.running && g_web.fd < 0) {
                sleep_ms(1000);
                g_web.fd = open_listener(1);
            }
            if (g_web.fd >= 0) log_line("web: listening again on port %d", g_web.port);
            continue;
        }
        handle(c);
        close(c);
    }
    return NULL;
}

int web_init(int port, const char *log_path)
{
    g_web.port = port;
    snprintf(g_web.log_path, sizeof(g_web.log_path), "%s", log_path);
    g_web.fd = open_listener(0);
    if (g_web.fd < 0) return 0;
    g_web.started = now_ms();
    g_web.running = 1;
    if (!thread_start(&g_web.thread, server, NULL, "web server")) {
        g_web.running = 0;
        close(g_web.fd);
        return 0;
    }
    log_line("web: dashboard on port %d", port);
    return 1;
}

void web_cleanup(void)
{
    if (!g_web.running) return;
    g_web.running = 0;
    pthread_join(g_web.thread, NULL);
    if (g_web.fd >= 0) close(g_web.fd);
    g_web.fd = -1;
}
