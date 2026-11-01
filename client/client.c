#define _GNU_SOURCE
#include "netutil.h"
#include "protocol.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

static int    g_sock = -1;
static Reader g_rd;
static char   g_resp[MAX_LINE];
static const char *g_payload = "";

static int connect_to(const char *host, const char *port)
{
    struct addrinfo hints, *res, *p;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* Reads one response line. 0 = OK, >0 = server error code, -1 = connection lost. */
static int read_response(void)
{
    int n = recv_line(&g_rd, g_resp, sizeof g_resp);
    if (n < 0) return -1;
    if (!strncmp(g_resp, "OK", 2)) {
        g_payload = g_resp[2] == ' ' ? g_resp + 3 : g_resp + 2;
        return 0;
    }
    if (!strncmp(g_resp, "ERR ", 4)) {
        int code = atoi(g_resp + 4);
        const char *sp = strchr(g_resp + 4, ' ');
        g_payload = sp ? sp + 1 : "";
        return code ? code : -1;
    }
    return -1;
}

static int rpc(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static int rpc(const char *fmt, ...)
{
    char cmd[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof cmd, fmt, ap);
    va_end(ap);
    if (send_line(g_sock, "%s", cmd) < 0) return -1;
    return read_response();
}

static void die_lost(void)
{
    puts("\nConnection to server lost.");
    exit(1);
}
#define CHECK(rc) do { if ((rc) == -1) die_lost(); } while (0)

static int read_line(const char *prompt, char *buf, size_t cap)
{
    fputs(prompt, stdout);
    fflush(stdout);
    if (!fgets(buf, (int)cap, stdin)) return 0;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 1;
}

static int read_secret(const char *prompt, char *buf, size_t cap)
{
    struct termios oldt, newt;
    int tty = isatty(STDIN_FILENO);
    fputs(prompt, stdout);
    fflush(stdout);
    if (tty) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(tcflag_t)ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    }
    char *r = fgets(buf, (int)cap, stdin);
    if (tty) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        putchar('\n');
    }
    if (!r) return 0;
    buf[strcspn(buf, "\r\n")] = '\0';
    return 1;
}

static void show_result(int rc, const char *okfmt)
{
    if (rc == 0) printf(okfmt, g_payload);
    else printf("  Failed: %s\n", g_payload);
}

static void do_register(void)
{
    char name[NAME_LEN + 16], pin[32], pin2[32];
    if (!read_line("Full name: ", name, sizeof name)) return;
    for (char *c = name; *c; c++) if (*c == ' ') *c = '_';
    if (!read_secret("Choose a 4-6 digit PIN: ", pin, sizeof pin)) return;
    if (!read_secret("Confirm PIN: ", pin2, sizeof pin2)) return;
    if (strcmp(pin, pin2)) { puts("  PINs do not match."); return; }
    int rc = rpc("REGISTER %s %s", name, pin);
    CHECK(rc);
    show_result(rc, "  Account created! Your account number is %s\n  Keep it safe - you need it to login.\n");
}

static int do_login(void)
{
    char acc[32], pin[32];
    if (!read_line("Account number: ", acc, sizeof acc)) return 0;
    if (!read_secret("PIN: ", pin, sizeof pin)) return 0;
    int rc = rpc("LOGIN %s %s", acc, pin);
    CHECK(rc);
    if (rc == 0) { printf("  Login successful. %s\n", g_payload); return 1; }
    printf("  Login failed: %s\n", g_payload);
    return 0;
}

static void do_amount(const char *cmd, const char *verb)
{
    char amt[64];
    char prompt[64];
    snprintf(prompt, sizeof prompt, "Amount to %s: ", verb);
    if (!read_line(prompt, amt, sizeof amt)) return;
    int rc = rpc("%s %s", cmd, amt);
    CHECK(rc);
    show_result(rc, "  Done. New balance: %s\n");
}

static void do_transfer(void)
{
    char to[32], amt[64];
    if (!read_line("Destination account: ", to, sizeof to)) return;
    if (!read_line("Amount: ", amt, sizeof amt)) return;
    int rc = rpc("TRANSFER %s %s", to, amt);
    CHECK(rc);
    show_result(rc, "  Transfer complete. New balance: %s\n");
}

static void do_history(void)
{
    char n[16];
    if (!read_line("How many recent transactions (1-50, default 10): ", n, sizeof n)) return;
    int rc = n[0] ? rpc("HISTORY %s", n) : rpc("HISTORY");
    CHECK(rc);
    if (rc) { printf("  Failed: %s\n", g_payload); return; }
    int count = atoi(g_payload);
    if (count == 0) { puts("  No transactions yet."); return; }
    printf("  %-5s %-19s %-13s %-8s %12s %12s\n", "ID", "Time", "Type", "Other", "Amount", "Balance");
    for (int i = 0; i < count; i++) {
        char line[MAX_LINE];
        if (recv_line(&g_rd, line, sizeof line) < 0) die_lost();
        char *f[6] = {0};
        char *save = NULL;
        int k = 0;
        for (char *t = strtok_r(line, "|", &save); t && k < 6; t = strtok_r(NULL, "|", &save)) f[k++] = t;
        if (k == 6) printf("  %-5s %-19s %-13s %-8s %12s %12s\n", f[0], f[1], f[2], f[3], f[4], f[5]);
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    char port[16];
    snprintf(port, sizeof port, "%d", argc > 2 ? atoi(argv[2]) : DEFAULT_PORT);

    g_sock = connect_to(host, port);
    if (g_sock < 0) { fprintf(stderr, "Cannot connect to %s:%s\n", host, port); return 1; }
    reader_init(&g_rd, g_sock);
    int rc = read_response();
    if (rc != 0) { printf("Server refused connection: %s\n", rc > 0 ? g_payload : "no response"); return 1; }

    puts("=====================================");
    puts("        SecureBank - Client");
    puts("=====================================");

    int logged_in = 0;
    char choice[16];
    for (;;) {
        if (!logged_in) {
            puts("\n1. Register new account\n2. Login\n3. Exit");
            if (!read_line("Choice: ", choice, sizeof choice)) break;
            switch (atoi(choice)) {
            case 1: do_register(); break;
            case 2: logged_in = do_login(); break;
            case 3: goto quit;
            default: puts("  Invalid choice.");
            }
        } else {
            puts("\n1. Balance\n2. Deposit\n3. Withdraw\n4. Transfer\n5. Transaction history\n6. Logout");
            if (!read_line("Choice: ", choice, sizeof choice)) break;
            switch (atoi(choice)) {
            case 1: rc = rpc("BALANCE"); CHECK(rc); show_result(rc, "  Current balance: %s\n"); break;
            case 2: do_amount("DEPOSIT", "deposit"); break;
            case 3: do_amount("WITHDRAW", "withdraw"); break;
            case 4: do_transfer(); break;
            case 5: do_history(); break;
            case 6: rc = rpc("LOGOUT"); CHECK(rc); logged_in = 0; puts("  Logged out."); break;
            default: puts("  Invalid choice.");
            }
        }
    }
quit:
    rpc("QUIT");
    close(g_sock);
    puts("Goodbye!");
    return 0;
}
