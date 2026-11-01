#ifndef NETUTIL_H
#define NETUTIL_H
#include <stddef.h>

#define RL_CLOSED  (-1)   /* peer closed / socket error */
#define RL_TOOLONG (-2)   /* line exceeded buffer (line is discarded) */
#define RL_TIMEOUT (-3)   /* SO_RCVTIMEO expired */
#define RDBUF 1024

/* Buffered line reader: TCP is a byte stream, so we must frame lines ourselves. */
typedef struct {
    int    fd;
    size_t pos, len;
    char   buf[RDBUF];
} Reader;

void reader_init(Reader *r, int fd);
/* Returns line length (>=0, '\n'/'\r' stripped) or one of the RL_* codes. */
int  recv_line(Reader *r, char *out, size_t cap);
/* Sends all bytes (handles partial send). 0 on success, -1 on error. */
int  send_all(int fd, const char *buf, size_t len);
/* printf-style; appends '\n'. */
int  send_line(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#endif
