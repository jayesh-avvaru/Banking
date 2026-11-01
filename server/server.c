#define _GNU_SOURCE
#include "bank.h"
#include "config.h"
#include "session.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "netutil.h"

static volatile sig_atomic_t g_stop = 0;
static int g_active = 0;
static pthread_mutex_t g_active_mutex = PTHREAD_MUTEX_INITIALIZER;

void log_msg(const char *fmt, ...)
{
    char ts[32], msg[512];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s] %s\n", ts, msg);
}

static void on_signal(int sig) { (void)sig; g_stop = 1; }

typedef struct {
    int  fd;
    char peer[64];
} ClientArg;

static void *client_thread(void *p)
{
    ClientArg *c = p;
    log_msg("accept() returned socket fd=%d for client %s -> handing it to a new thread", c->fd, c->peer);
    session_run(c->fd, c->peer);
    close(c->fd);
    log_msg("close() socket fd=%d (%s) - connection ended", c->fd, c->peer);
    free(c);
    pthread_mutex_lock(&g_active_mutex);
    g_active--;
    pthread_mutex_unlock(&g_active_mutex);
    return NULL;
}

static int make_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); close(fd); return -1; }
    if (listen(fd, 128) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

int main(int argc, char **argv)
{
    int port = DEFAULT_PORT, nosync = 0;
    const char *dir = "data";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--nosync")) nosync = 1;     /* faster, for load tests only */
        else { fprintf(stderr, "usage: %s [-p port] [-d datadir] [--nosync]\n", argv[0]); return 2; }
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;               /* no SA_RESTART: blocking calls return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    if (bank_init(dir, nosync) != 0) { log_msg("failed to initialise bank storage in '%s'", dir); return 1; }
    int lfd = make_listener(port);
    if (lfd < 0) return 1;
    log_msg("socket() + bind() + listen() done: waiting for TCP connections on port %d (data dir: %s%s)", port, dir, nosync ? ", NO FSYNC" : "");

    while (!g_stop) {
        struct pollfd pfd = { .fd = lfd, .events = POLLIN };
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0) continue;               /* timeout or EINTR: re-check g_stop */

        struct sockaddr_in ca;
        socklen_t cl = sizeof ca;
        int cfd = accept(lfd, (struct sockaddr *)&ca, &cl);
        if (cfd < 0) continue;

        pthread_mutex_lock(&g_active_mutex);
        int full = g_active >= MAX_CLIENTS;
        if (!full) g_active++;
        pthread_mutex_unlock(&g_active_mutex);
        if (full) {
            send_line(cfd, "ERR %d %s", ERR_SERVER_FULL, err_text(ERR_SERVER_FULL));
            close(cfd);
            continue;
        }

        struct timeval rt = { .tv_sec = IDLE_TIMEOUT_SEC }, st = { .tv_sec = 10 };
        int one = 1;
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof st);
        setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

        ClientArg *c = malloc(sizeof *c);
        if (!c) { close(cfd); pthread_mutex_lock(&g_active_mutex); g_active--; pthread_mutex_unlock(&g_active_mutex); continue; }
        c->fd = cfd;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ca.sin_addr, ip, sizeof ip);
        snprintf(c->peer, sizeof c->peer, "%s:%d", ip, ntohs(ca.sin_port));

        pthread_t th;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &attr, client_thread, c) != 0) {
            log_msg("pthread_create failed");
            close(cfd);
            free(c);
            pthread_mutex_lock(&g_active_mutex); g_active--; pthread_mutex_unlock(&g_active_mutex);
        }
        pthread_attr_destroy(&attr);
    }

    log_msg("Shutting down...");
    close(lfd);
    for (int i = 0; i < 30; i++) {           /* give active clients up to 3 s to finish */
        pthread_mutex_lock(&g_active_mutex);
        int a = g_active;
        pthread_mutex_unlock(&g_active_mutex);
        if (a == 0) break;
        usleep(100000);
    }
    pthread_mutex_lock(&g_active_mutex);
    int remaining = g_active;
    pthread_mutex_unlock(&g_active_mutex);
    if (remaining == 0) bank_shutdown();
    else log_msg("%d clients still active; skipping checkpoint (ledger is authoritative)", remaining);
    return 0;
}
