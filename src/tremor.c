// tremor — turn the MacBook trackpad into an expressive instrument.
//
// A single self-contained binary: it reads the private MultitouchSupport
// contact stream (position, area, force in grams, ellipse angle), serves the
// embedded web UI over HTTP, and pushes every frame to the browser via
// Server-Sent Events. No Node, no runtime deps — only CoreFoundation.
//
// Routes:
//   GET /          → the embedded page (public/index.html, baked in at build)
//   GET /stream    → SSE: one `data: {...}` line per contact frame
//   GET /parser?d=<i|-1>&on=<0|1>  → toggle a device as a SYSTEM input device
//
// With the parser disabled the trackpad stops driving the cursor/clicks, but
// our contact stream keeps flowing — i.e. instrument-only mode. That disabled
// state sticks on the driver across process exit, so we always re-enable on
// shutdown, on signals, and whenever the last client disconnects.
//
// Per contact we read these float fields of the 96-byte MTTouch struct:
//   0x20 normalized x   0x24 normalized y   (0..1, origin bottom-left)
//   0x30 size           0x34 force (grams)
//   0x38 angle          0x3C major axis     0x40 minor axis
//
// Build: see package.json `build` (xxd-embeds the page, then clang -Ibuild).

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "html_embed.h" // index_html[], index_html_len — generated from public/index.html

#ifndef TREMOR_VERSION
#define TREMOR_VERSION "dev"
#endif

typedef void *MTDeviceRef;
typedef struct { uint8_t _bytes[96]; } MTTouch;
typedef void (*FnSetParser)(MTDeviceRef, bool);
#define MAX_DEVICES 8
#define MAX_CLIENTS 64

static MTDeviceRef g_devs[MAX_DEVICES];
static int g_ndev = 0;
static FnSetParser g_setParser = NULL;
static CFRunLoopRef g_loop = NULL;

// Cached SSE payload of the device list, replayed to every client on connect.
static char g_devjson[512];
static int g_devjson_len = 0;

// Open SSE client sockets. Appended by the accept thread, broadcast to / pruned
// on the run-loop thread; the mutex guards that hand-off.
static int g_clients[MAX_CLIENTS];
static int g_nclient = 0;
static pthread_mutex_t g_clk = PTHREAD_MUTEX_INITIALIZER;

// Re-enable system input on every device — the safety net used on shutdown, on
// signals, and when no one is watching, so a disabled parser never outlives use.
static void reenable_all(void) {
    if (!g_setParser) return;
    for (int i = 0; i < g_ndev; i++) g_setParser(g_devs[i], true);
}
static void on_sig(int s) {
    reenable_all();
    signal(s, SIG_DFL);
    raise(s);
}

static float F(const MTTouch *t, int off) {
    float f;
    memcpy(&f, t->_bytes + off, sizeof(f));
    return f;
}
static int I(const MTTouch *t, int off) {
    int v;
    memcpy(&v, t->_bytes + off, sizeof(v));
    return v;
}
static int index_of(MTDeviceRef d) {
    for (int i = 0; i < g_ndev; i++)
        if (g_devs[i] == d) return i;
    return -1;
}

// Write the same bytes to every SSE client; drop (and close) any that error.
// Runs on the run-loop thread only (frames, ping, parser confirmations), so it
// never races itself — the lock only guards against the accept thread appending.
static void broadcast(const char *buf, int len) {
    pthread_mutex_lock(&g_clk);
    int w = 0;
    for (int i = 0; i < g_nclient; i++) {
        int fd = g_clients[i];
        if (write(fd, buf, len) == len) g_clients[w++] = fd;
        else close(fd);
    }
    int dropped = (w < g_nclient);
    g_nclient = w;
    int empty = (g_nclient == 0);
    pthread_mutex_unlock(&g_clk);
    // Last watcher gone → restore the trackpad to a normal system input device.
    if (dropped && empty) reenable_all();
}

typedef void       (*MTContactCallback)(MTDeviceRef, MTTouch *, int32_t, double, int32_t);
typedef CFArrayRef (*FnCreateList)(void);
typedef bool       (*FnIsBuiltIn)(MTDeviceRef);
typedef void       (*FnRegister)(MTDeviceRef, MTContactCallback);
typedef int        (*FnStart)(MTDeviceRef, int);

static void on_contacts(MTDeviceRef dev, MTTouch *c, int32_t n,
                        double ts, int32_t frame) {
    int idx = index_of(dev);
    if (idx < 0) return;
    char body[4096];
    int p = snprintf(body, sizeof body, "data: {\"d\":%d,\"c\":[", idx);
    for (int32_t i = 0; i < n && p < (int)sizeof body - 256; i++) {
        p += snprintf(body + p, sizeof body - p,
            "%s{\"id\":%d,\"x\":%.4f,\"y\":%.4f,\"g\":%.2f,\"s\":%.3f,\"a\":%.3f,\"j\":%.2f,\"n\":%.2f}",
            i ? "," : "", I(&c[i], 0x10),
            F(&c[i], 0x20), F(&c[i], 0x24), F(&c[i], 0x34),
            F(&c[i], 0x30), F(&c[i], 0x38), F(&c[i], 0x3C), F(&c[i], 0x40));
    }
    p += snprintf(body + p, sizeof body - p, "]}\n\n");
    broadcast(body, p);
}

// SSE keepalive through idle stretches (no touches → no frames); also the only
// time we notice a tab closed while idle, so we can re-enable the parser.
static void ping_cb(CFRunLoopTimerRef t, void *info) {
    broadcast(": ping\n\n", 8);
}

// Apply a parser toggle on the run-loop thread (MT calls must not race the
// contact callback), then confirm it to the page. Called from the accept thread.
static void schedule_parser(int idx, int on) {
    if (!g_loop) return;
    const int i = idx, o = on ? 1 : 0;
    CFRunLoopPerformBlock(g_loop, kCFRunLoopCommonModes, ^{
        if (!g_setParser) return;
        if (i < 0) {
            for (int k = 0; k < g_ndev; k++) g_setParser(g_devs[k], o);
        } else if (i < g_ndev) {
            g_setParser(g_devs[i], o);
        } else return;
        char b[80];
        int m = snprintf(b, sizeof b,
            "data: {\"parser\":{\"d\":%d,\"on\":%s}}\n\n", i, o ? "true" : "false");
        broadcast(b, m);
    });
    CFRunLoopWakeUp(g_loop);
}

static void http_write(int fd, const char *s) { write(fd, s, strlen(s)); }

// Reply with a small JSON body (used by /version and /update).
static void send_json(int fd, const char *json) {
    char hdr[160];
    int m = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
        "Access-Control-Allow-Origin: *\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n",
        strlen(json));
    write(fd, hdr, m);
    write(fd, json, strlen(json));
}

static const char *find_brew(void) {
    if (access("/opt/homebrew/bin/brew", X_OK) == 0) return "/opt/homebrew/bin/brew";
    if (access("/usr/local/bin/brew", X_OK) == 0) return "/usr/local/bin/brew";
    return NULL;
}

// Refresh just the tap (fast — not a full `brew update`) and upgrade tremor in
// place. Fixed command; nothing from the HTTP request is interpolated. The newly
// linked binary takes effect on the next launch (the running process is the old
// one), which the UI tells the user. Requires the tap to be trusted once at
// install time (`brew trust toolittlecakes/tremor`).
static const char *run_self_update(void) {
    const char *brew = find_brew();
    if (!brew) return "{\"ok\":false,\"error\":\"brew not found\"}";
    char cmd[512];
    snprintf(cmd, sizeof cmd,
        "git -C \"$(%s --repo toolittlecakes/tremor)\" pull -q --ff-only >/dev/null 2>&1; "
        "%s upgrade tremor >/dev/null 2>&1",
        brew, brew);
    int rc = system(cmd);
    return (rc == 0) ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"upgrade failed\"}";
}

static int g_listen = -1;

static void *accept_thread(void *arg) {
    for (;;) {
        int fd = accept(g_listen, NULL, NULL);
        if (fd < 0) { if (errno == EINTR) continue; break; }

        char req[2048];
        int total = 0;
        for (;;) {
            ssize_t r = read(fd, req + total, sizeof(req) - 1 - total);
            if (r <= 0) { total = 0; break; }
            total += r;
            req[total] = 0;
            if (strstr(req, "\r\n\r\n") || total >= (int)sizeof(req) - 1) break;
        }
        if (total == 0) { close(fd); continue; }

        char target[1024] = {0};
        if (sscanf(req, "GET %1023s", target) != 1) {
            http_write(fd, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            close(fd);
            continue;
        }
        char query[512] = {0};
        char *q = strchr(target, '?');
        if (q) { strncpy(query, q + 1, sizeof(query) - 1); *q = 0; }

        if (strcmp(target, "/") == 0) {
            char hdr[160];
            int m = snprintf(hdr, sizeof hdr,
                "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %u\r\nConnection: close\r\n\r\n", index_html_len);
            write(fd, hdr, m);
            write(fd, index_html, index_html_len);
            close(fd);
        } else if (strcmp(target, "/stream") == 0) {
            http_write(fd,
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n"
                "retry: 1000\n\n");
            if (g_devjson_len) write(fd, g_devjson, g_devjson_len);
            pthread_mutex_lock(&g_clk);
            if (g_nclient < MAX_CLIENTS) g_clients[g_nclient++] = fd;
            else { close(fd); fd = -1; }
            pthread_mutex_unlock(&g_clk);
            // kept open: the run-loop thread streams frames to it
        } else if (strcmp(target, "/parser") == 0) {
            int d = 0, on = 0;
            char *pd = strstr(query, "d="), *po = strstr(query, "on=");
            if (pd) d = atoi(pd + 2);
            if (po) on = atoi(po + 3);
            schedule_parser(d, on);
            http_write(fd, "HTTP/1.1 204 No Content\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            close(fd);
        } else if (strcmp(target, "/version") == 0) {
            char b[96];
            snprintf(b, sizeof b, "{\"version\":\"%s\"}", TREMOR_VERSION);
            send_json(fd, b);
            close(fd);
        } else if (strcmp(target, "/update") == 0) {
            send_json(fd, run_self_update());
            close(fd);
        } else {
            http_write(fd, "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
            close(fd);
        }
    }
    return NULL;
}

static int listen_on(int port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0) { close(s); return -1; }
    if (listen(s, 16) < 0) { close(s); return -1; }
    return s;
}

int main(void) {
    const char *path =
        "/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport";
    void *h = dlopen(path, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); return 1; }

    FnCreateList createList = (FnCreateList)dlsym(h, "MTDeviceCreateList");
    FnIsBuiltIn  isBuiltIn  = (FnIsBuiltIn)dlsym(h, "MTDeviceIsBuiltIn");
    FnRegister   registerCb = (FnRegister)dlsym(h, "MTRegisterContactFrameCallback");
    FnStart      start      = (FnStart)dlsym(h, "MTDeviceStart");
    if (!createList || !isBuiltIn || !registerCb || !start) {
        fprintf(stderr, "missing symbols\n"); return 1;
    }
    g_setParser = (FnSetParser)dlsym(h, "MTDeviceSetParserEnabled");

    CFArrayRef devs = createList();
    if (!devs) { fprintf(stderr, "no multitouch devices\n"); return 1; }
    CFIndex count = CFArrayGetCount(devs);
    for (CFIndex i = 0; i < count && g_ndev < MAX_DEVICES; i++)
        g_devs[g_ndev++] = (MTDeviceRef)CFArrayGetValueAtIndex(devs, i);
    if (g_ndev == 0) { fprintf(stderr, "no trackpads found\n"); return 1; }

    // Clean baseline + safety nets so a parser-disabled device is never left dead
    // (recovers even if a previous run crashed while disabled). Ignore SIGPIPE so
    // a write to a vanished client returns EPIPE instead of killing us.
    signal(SIGPIPE, SIG_IGN);
    reenable_all();
    atexit(reenable_all);
    int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGSEGV, SIGBUS, SIGABRT };
    for (int i = 0; i < 6; i++) signal(sigs[i], on_sig);

    // Cache the device list as an SSE payload for clients to read on connect.
    char dj[400];
    int dp = snprintf(dj, sizeof dj, "{\"dev\":[");
    for (int k = 0; k < g_ndev; k++)
        dp += snprintf(dj + dp, sizeof dj - dp, "%s{\"i\":%d,\"builtin\":%s}",
                       k ? "," : "", k, isBuiltIn(g_devs[k]) ? "true" : "false");
    dp += snprintf(dj + dp, sizeof dj - dp, "]}");
    g_devjson_len = snprintf(g_devjson, sizeof g_devjson, "data: %s\n\n", dj);

    int port = getenv("PORT") ? atoi(getenv("PORT")) : 8788;
    g_listen = listen_on(port);
    if (g_listen < 0) { fprintf(stderr, "cannot bind port %d: %s\n", port, strerror(errno)); return 1; }

    pthread_t th;
    pthread_create(&th, NULL, accept_thread, NULL);

    for (int k = 0; k < g_ndev; k++) {
        registerCb(g_devs[k], on_contacts);
        start(g_devs[k], 0);
    }

    g_loop = CFRunLoopGetCurrent();
    CFRunLoopTimerRef ping = CFRunLoopTimerCreate(NULL,
        CFAbsoluteTimeGetCurrent() + 10, 10, 0, 0, ping_cb, NULL);
    CFRunLoopAddTimer(g_loop, ping, kCFRunLoopCommonModes);

    fprintf(stderr, "tremor: http://127.0.0.1:%d  (%d device(s))\n", port, g_ndev);
    // Pop the page open only for an interactive run; stays quiet under a service.
    if (isatty(STDERR_FILENO) && !getenv("TREMOR_NO_OPEN")) {
        char cmd[64];
        snprintf(cmd, sizeof cmd, "open http://127.0.0.1:%d", port);
        system(cmd);
    }

    CFRunLoopRun();
    reenable_all();
    return 0;
}
