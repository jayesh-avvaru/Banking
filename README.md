# SecureBank - Client-Server Banking System (C, TCP sockets, pthreads)

## Build & run
    make                          # builds bin/server, bin/client, bin/stress
    ./bin/server -p 8080 -d data  # start server (Ctrl+C = graceful shutdown + checkpoint)
    ./bin/client 127.0.0.1 8080   # start a client (run as many as you like)
    ./bin/stress 40 100 8080      # 40 parallel clients x 100 ops, then verifies total money

Server flags: `-p port`, `-d datadir`, `--nosync` (skip fsync; load tests only).
Requires Linux/WSL/macOS with gcc and pthreads. On Windows, use WSL.

## Architecture
    client.c  <--TCP, line protocol-->  server.c (accept loop, thread per client)
                                           session.c  parse + authorize + dispatch
                                           bank.c     account logic, per-account mutexes
                                           storage.c  write-ahead ledger + checkpoint
                                           auth.c     salted, iterated SHA-256 PIN hashes
    common/netutil.c  recv_line() / send_all(): message framing over TCP's byte stream

## Protocol (one line per message, '\n' terminated)
    REGISTER <name> <pin>   -> OK <account_no>
    LOGIN <acc> <pin>       -> OK Welcome <name>
    BALANCE | DEPOSIT <amt> | WITHDRAW <amt> | TRANSFER <acc> <amt>  -> OK <new_balance>
    HISTORY [n]             -> OK <count>, then <count> lines: id|time|type|other|amount|balance
    LOGOUT | QUIT           -> OK ...
    Errors: ERR <code> <text>   (codes in common/protocol.h)

## Correctness design
- Money is integer paise; amounts are parsed without floating point.
- One mutex per account; transfers lock the lower account number first, so no deadlock.
- Write-ahead log: every change is appended to txn.log and fdatasync'd BEFORE memory is updated
  and the client is told OK. Log records hold absolute balances, so replay is idempotent.
- accounts.dat is a checkpoint (temp file + fsync + atomic rename). On startup: load the
  checkpoint, replay newer log records, trim any torn last record.
- 3 wrong PINs lock an account (in memory; a restart clears it). 5-minute idle timeout.
- Partial send/recv handled by send_all()/recv_line(). SIGPIPE ignored. Bounded line length.

## Tested
- SHA-256 against known vectors; protocol edge cases; 40 concurrent clients (total money conserved);
  kill -9 plus corrupted log tail recovery; graceful shutdown checkpoint.

## Known limits / ideas for extension
- fsync under one global log mutex serialises commits (group commit would raise throughput).
- Plain TCP, no TLS (add OpenSSL). History scans the ledger linearly (add an index).
- Account lockout is not persisted; no admin role; single node only.

## Web frontend
Browsers cannot open raw TCP sockets, so `gateway/gateway.py` bridges them to the C server:

    browser  --HTTP/JSON-->  gateway.py (port 8000)  --TCP line protocol-->  C server (port 8080)

Run (3 steps, each in its own terminal, from the banking folder):

    make
    ./bin/server -p 8080 -d data
    python3 gateway/gateway.py                  # then open http://127.0.0.1:8000

Options: `--port 8000` (web port), `--backend 127.0.0.1:8080` (C server address).
Python 3 standard library only, nothing to install. The gateway keeps one TCP connection per
browser session, whitelists every value it forwards (no protocol injection), binds to localhost
only, and drops idle sessions after 280 s.

## Presenting it
See DEMO.md for a step-by-step demo script, the socket-call table and likely viva questions.
