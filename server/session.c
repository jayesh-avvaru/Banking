#define _GNU_SOURCE
#include "session.h"
#include "bank.h"
#include "config.h"
#include "netutil.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

typedef struct {
    int      fd;
    Reader   rd;
    Account *acc;       /* NULL until LOGIN succeeds */
    const char *peer;
} Session;

/* Demo logging: shows every line crossing the socket (PINs are masked). */
static void log_wire(const char *dir, const char *peer, const char *text)
{
    char buf[MAX_LINE];
    snprintf(buf, sizeof buf, "%s", text);
    if (!strncasecmp(buf, "LOGIN ", 6) || !strncasecmp(buf, "REGISTER ", 9)) {
        char *sp = strrchr(buf, ' ');
        if (sp) snprintf(sp + 1, (size_t)(buf + sizeof buf - (sp + 1)), "****");
    }
    log_msg("[%s] %s \"%s\"", peer, dir, buf);
}

static void reply_ok(Session *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply_ok(Session *s, const char *fmt, ...)
{
    char body[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    char full[MAX_LINE + 8];
    snprintf(full, sizeof full, "OK %s", body);
    log_wire("send()   ->", s->peer, full);
    send_line(s->fd, "%s", full);
}

static void reply_err(Session *s, int code)
{
    char full[MAX_LINE];
    snprintf(full, sizeof full, "ERR %d %s", code, err_text(code));
    log_wire("send()   ->", s->peer, full);
    send_line(s->fd, "%s", full);
}

/* "500", "500.5", "500.50" -> paise. No floating point anywhere. */
static int parse_amount(const char *s, int64_t *out)
{
    int64_t whole = 0, frac = 0;
    int digits = 0;
    while (isdigit((unsigned char)*s)) {
        whole = whole * 10 + (*s - '0');
        if (whole > 1000000000000LL) return -1;
        s++;
        digits++;
    }
    if (!digits) return -1;
    if (*s == '.') {
        s++;
        int fd = 0;
        while (isdigit((unsigned char)*s) && fd < 2) {
            frac = frac * 10 + (*s - '0');
            s++;
            fd++;
        }
        if (fd == 0) return -1;
        if (fd == 1) frac *= 10;
    }
    if (*s) return -1;
    *out = whole * 100 + frac;
    return 0;
}

static int parse_int(const char *s, long *out)
{
    char *end;
    if (!*s) return -1;
    long v = strtol(s, &end, 10);
    if (*end) return -1;
    *out = v;
    return 0;
}

static int valid_pin(const char *p)
{
    size_t n = strlen(p);
    if (n < 4 || n > 6) return 0;
    for (; *p; p++) if (!isdigit((unsigned char)*p)) return 0;
    return 1;
}

static int valid_name(const char *p)
{
    size_t n = strlen(p);
    if (n == 0 || n >= NAME_LEN) return 0;
    for (; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-' && *p != '.') return 0;
    return 1;
}

#define M(x) (long long)((x) / 100), (long long)((x) % 100)

static void fmt_history_line(const LogRec *r, int acc, char *out, size_t cap)
{
    char tbuf[32];
    time_t t = (time_t)r->ts;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(tbuf, sizeof tbuf, "%Y-%m-%d %H:%M:%S", &tm);

    const char *type;
    char other[16] = "-";
    int64_t bal;
    switch (r->type) {
    case TX_DEPOSIT:  type = "DEPOSIT";  bal = r->bal_to;   break;
    case TX_WITHDRAW: type = "WITHDRAW"; bal = r->bal_from; break;
    default:
        if (r->from_acc == acc) {
            type = "TRANSFER_OUT"; bal = r->bal_from;
            snprintf(other, sizeof other, "%d", r->to_acc);
        } else {
            type = "TRANSFER_IN"; bal = r->bal_to;
            snprintf(other, sizeof other, "%d", r->from_acc);
        }
    }
    snprintf(out, cap, "%lld|%s|%s|%s|%lld.%02lld|%lld.%02lld\n",
             (long long)r->txn_id, tbuf, type, other, M(r->amount), M(bal));
}

/* returns -1 when the session should end */
static int dispatch(Session *s, char *line)
{
    char *tok[MAX_TOKENS + 1];
    int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
        if (n == MAX_TOKENS) { reply_err(s, ERR_SYNTAX); return 0; }
        tok[n++] = t;
    }
    if (n == 0) return 0;
    const char *cmd = tok[0];
    int64_t amt, bal;
    long v;
    int rc;

    if (!strcasecmp(cmd, "QUIT")) {
        reply_ok(s, "Goodbye");
        return -1;
    }

    if (!strcasecmp(cmd, "REGISTER")) {
        if (n != 3) { reply_err(s, ERR_SYNTAX); return 0; }
        if (!valid_name(tok[1])) { reply_err(s, ERR_INVALID_NAME); return 0; }
        if (!valid_pin(tok[2]))  { reply_err(s, ERR_INVALID_PIN);  return 0; }
        int no;
        rc = bank_register(tok[1], tok[2], &no);
        if (rc) { reply_err(s, rc); return 0; }
        log_msg("[%s] REGISTER ok -> account %d", s->peer, no);
        reply_ok(s, "%d", no);
        return 0;
    }

    if (!strcasecmp(cmd, "LOGIN")) {
        if (s->acc) { reply_err(s, ERR_ALREADY_LOGGED_IN); return 0; }
        if (n != 3 || parse_int(tok[1], &v) || v < 0 || v > 2000000000L) { reply_err(s, ERR_SYNTAX); return 0; }
        Account *a = NULL;
        rc = bank_login((int)v, tok[2], &a);
        if (rc) {
            log_msg("[%s] LOGIN failed for account %ld (code %d)", s->peer, v, rc);
            reply_err(s, rc);
            return 0;
        }
        s->acc = a;
        log_msg("[%s] LOGIN ok account %d", s->peer, a->acc_no);
        reply_ok(s, "Welcome %s", a->name);
        return 0;
    }

    /* everything below needs a session */
    if (!s->acc) { reply_err(s, ERR_NOT_LOGGED_IN); return 0; }

    if (!strcasecmp(cmd, "LOGOUT")) {
        log_msg("[%s] LOGOUT account %d", s->peer, s->acc->acc_no);
        s->acc = NULL;
        reply_ok(s, "Logged out");
    } else if (!strcasecmp(cmd, "BALANCE")) {
        bank_balance(s->acc, &bal);
        reply_ok(s, "%lld.%02lld", M(bal));
    } else if (!strcasecmp(cmd, "DEPOSIT") || !strcasecmp(cmd, "WITHDRAW")) {
        if (n != 2 || parse_amount(tok[1], &amt)) { reply_err(s, ERR_AMOUNT); return 0; }
        int dep = !strcasecmp(cmd, "DEPOSIT");
        rc = dep ? bank_deposit(s->acc, amt, &bal) : bank_withdraw(s->acc, amt, &bal);
        if (rc) { reply_err(s, rc); return 0; }
        log_msg("[%s] %s %lld.%02lld account %d", s->peer, dep ? "DEPOSIT" : "WITHDRAW", M(amt), s->acc->acc_no);
        reply_ok(s, "%lld.%02lld", M(bal));
    } else if (!strcasecmp(cmd, "TRANSFER")) {
        if (n != 3 || parse_int(tok[1], &v) || v < 0 || v > 2000000000L || parse_amount(tok[2], &amt)) {
            reply_err(s, n == 3 ? ERR_AMOUNT : ERR_SYNTAX);
            return 0;
        }
        rc = bank_transfer(s->acc, (int)v, amt, &bal);
        if (rc) { reply_err(s, rc); return 0; }
        log_msg("[%s] TRANSFER %lld.%02lld from %d to %ld", s->peer, M(amt), s->acc->acc_no, v);
        reply_ok(s, "%lld.%02lld", M(bal));
    } else if (!strcasecmp(cmd, "HISTORY")) {
        long cnt = 10;
        if (n > 2 || (n == 2 && (parse_int(tok[1], &cnt) || cnt < 1 || cnt > MAX_HISTORY))) {
            reply_err(s, ERR_SYNTAX);
            return 0;
        }
        LogRec recs[MAX_HISTORY];
        int got = bank_history(s->acc, (int)cnt, recs);
        if (got < 0) { reply_err(s, ERR_INTERNAL); return 0; }
        char out[MAX_HISTORY * 160 + 32];
        size_t len = (size_t)snprintf(out, sizeof out, "OK %d\n", got);
        for (int i = 0; i < got; i++) {
            fmt_history_line(&recs[i], s->acc->acc_no, out + len, sizeof out - len);
            len += strlen(out + len);
        }
        send_all(s->fd, out, len);          /* header + lines in a single send */
        log_msg("[%s] send()   -> \"OK %d (+%d ledger lines)\"", s->peer, got, got);
    } else {
        reply_err(s, ERR_SYNTAX);
    }
    return 0;
}

void session_run(int fd, const char *peer)
{
    Session s;
    s.fd = fd;
    s.acc = NULL;
    s.peer = peer;
    reader_init(&s.rd, fd);

    send_line(fd, "OK SecureBank ready. Commands: REGISTER LOGIN BALANCE DEPOSIT WITHDRAW TRANSFER HISTORY LOGOUT QUIT");

    char line[MAX_LINE];
    for (;;) {
        int n = recv_line(&s.rd, line, sizeof line);
        if (n == RL_CLOSED) break;
        if (n == RL_TIMEOUT) { reply_err(&s, ERR_TIMEOUT); break; }
        if (n == RL_TOOLONG) { reply_err(&s, ERR_SYNTAX); continue; }
        log_wire("recv()   <-", peer, line);     /* before dispatch() tokenises the line */
        if (dispatch(&s, line) < 0) break;
    }
    if (s.acc) log_msg("[%s] session ended (account %d)", peer, s.acc->acc_no);
}
