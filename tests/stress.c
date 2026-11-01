/* Concurrency stress test: many clients hammer the server, then we verify that
 * total money == initial deposits + successful deposits - successful withdrawals. */
#define _GNU_SOURCE
#include "netutil.h"
#include "protocol.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define K 8
static const char *g_host = "127.0.0.1";
static int g_port = DEFAULT_PORT, g_ops = 200;
static int g_acc[K];
static _Atomic long long g_dep, g_wd, g_tx_ok, g_tx_funds, g_total_ops;

typedef struct { int fd; Reader rd; char resp[MAX_LINE]; } Conn;

static int conn_open(Conn *c)
{
    c->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)g_port) };
    inet_pton(AF_INET, g_host, &a.sin_addr);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) < 0) return -1;
    reader_init(&c->rd, c->fd);
    return recv_line(&c->rd, c->resp, sizeof c->resp) >= 0 ? 0 : -1;
}

/* 0 = OK, >0 = ERR code, -1 = I/O failure */
static int cmd(Conn *c, const char *fmt, ...)
{
    char buf[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (send_line(c->fd, "%s", buf) < 0) return -1;
    if (recv_line(&c->rd, c->resp, sizeof c->resp) < 0) return -1;
    if (!strncmp(c->resp, "OK", 2)) return 0;
    return atoi(c->resp + 4);
}

static void *worker(void *arg)
{
    int id = (int)(long)arg;
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)(id * 2654435761u);
    Conn c;
    if (conn_open(&c) < 0) { fprintf(stderr, "worker %d: connect failed\n", id); return NULL; }
    int me = id % K;
    if (cmd(&c, "LOGIN %d 1234", g_acc[me]) != 0) { fprintf(stderr, "worker %d: login failed\n", id); return NULL; }

    for (int i = 0; i < g_ops; i++) {
        int pick = rand_r(&seed) % 100;
        long long p = 1 + rand_r(&seed) % 5000;           /* 0.01 .. 50.00 */
        int rc;
        if (pick < 70) {
            int other = (me + 1 + rand_r(&seed) % (K - 1)) % K;
            rc = cmd(&c, "TRANSFER %d %lld.%02lld", g_acc[other], p / 100, p % 100);
            if (rc == 0) g_tx_ok++; else if (rc == ERR_FUNDS) g_tx_funds++; else { fprintf(stderr, "transfer err %d\n", rc); }
        } else if (pick < 85) {
            rc = cmd(&c, "DEPOSIT %lld.%02lld", p / 100, p % 100);
            if (rc == 0) g_dep += p; else fprintf(stderr, "deposit err %d\n", rc);
        } else {
            rc = cmd(&c, "WITHDRAW %lld.%02lld", p / 100, p % 100);
            if (rc == 0) g_wd += p; else if (rc != ERR_FUNDS) fprintf(stderr, "withdraw err %d\n", rc);
        }
        g_total_ops++;
    }
    cmd(&c, "QUIT");
    close(c.fd);
    return NULL;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int threads = argc > 1 ? atoi(argv[1]) : 20;
    g_ops = argc > 2 ? atoi(argv[2]) : 200;
    g_port = argc > 3 ? atoi(argv[3]) : DEFAULT_PORT;

    Conn c;
    if (conn_open(&c) < 0) { fprintf(stderr, "cannot connect to server\n"); return 2; }
    for (int i = 0; i < K; i++) {                          /* setup: K accounts with 1000.00 each */
        char name[32];
        snprintf(name, sizeof name, "stress%d_%ld", i, (long)time(NULL));
        if (cmd(&c, "REGISTER %s 1234", name) != 0) { fprintf(stderr, "register failed\n"); return 2; }
        g_acc[i] = atoi(c.resp + 3);
        if (cmd(&c, "LOGIN %d 1234", g_acc[i]) != 0 || cmd(&c, "DEPOSIT 1000.00") != 0 || cmd(&c, "LOGOUT") != 0) {
            fprintf(stderr, "setup failed\n"); return 2;
        }
    }
    cmd(&c, "QUIT"); close(c.fd);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_t th[threads];
    for (long i = 0; i < threads; i++) pthread_create(&th[i], NULL, worker, (void *)i);
    for (int i = 0; i < threads; i++) pthread_join(th[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    long long sum = 0;                                     /* verification */
    if (conn_open(&c) < 0) return 2;
    for (int i = 0; i < K; i++) {
        long long w = 0, f = 0;
        if (cmd(&c, "LOGIN %d 1234", g_acc[i]) != 0 || cmd(&c, "BALANCE") != 0) { fprintf(stderr, "verify failed\n"); return 2; }
        sscanf(c.resp + 3, "%lld.%lld", &w, &f);
        sum += w * 100 + f;
        cmd(&c, "LOGOUT");
    }
    cmd(&c, "QUIT"); close(c.fd);

    long long expected = (long long)K * 100000 + g_dep - g_wd;
    printf("threads=%d ops=%lld (%.0f ops/s)  transfers ok=%lld insufficient=%lld\n",
           threads, (long long)g_total_ops, (double)g_total_ops / secs, (long long)g_tx_ok, (long long)g_tx_funds);
    printf("expected total = %lld.%02lld   actual total = %lld.%02lld\n",
           expected / 100, expected % 100, sum / 100, sum % 100);
    puts(sum == expected ? "RESULT: PASS (no money created or lost)" : "RESULT: FAIL");
    return sum == expected ? 0 : 1;
}
