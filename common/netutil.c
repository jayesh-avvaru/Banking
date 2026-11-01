#include "netutil.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>

/* macOS has no MSG_NOSIGNAL; callers ignore SIGPIPE instead */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

void reader_init(Reader *r, int fd)
{
    r->fd = fd;
    r->pos = r->len = 0;
}

int recv_line(Reader *r, char *out, size_t cap)
{
    size_t n = 0;
    int overflow = 0;

    for (;;) {
        if (r->pos == r->len) {
            ssize_t k = recv(r->fd, r->buf, sizeof r->buf, 0);
            if (k == 0) return RL_CLOSED;
            if (k < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return RL_TIMEOUT;
                return RL_CLOSED;
            }
            r->pos = 0;
            r->len = (size_t)k;
        }
        char c = r->buf[r->pos++];
        if (c == '\n') {
            if (overflow) return RL_TOOLONG;
            while (n > 0 && out[n - 1] == '\r') n--;
            out[n] = '\0';
            return (int)n;
        }
        if (n + 1 < cap) out[n++] = c;
        else overflow = 1;
    }
}

int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t w = send(fd, buf, len, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        buf += w;
        len -= (size_t)w;
    }
    return 0;
}

int send_line(int fd, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n > sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n';
    return send_all(fd, buf, (size_t)n);
}
