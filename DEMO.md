# Demo script: how the network works in SecureBank

## Setup (3 terminals + browser)
    ./bin/server -p 8080 -d data       # terminal 1: watch this, it narrates every socket call
    python3 gateway/gateway.py         # terminal 2
    open http://127.0.0.1:8000         # browser: the "What the network is doing" panel is at the bottom

## The big idea (30 seconds)
A socket is an endpoint for a network connection: IP address + port number.
Two programs talk by each holding one socket and sending bytes over a TCP connection.
Our bank is a client-server system: many clients, one server.

## Server side (terminal 1) - the classic socket calls
| Call       | What it does                                   | Where in code        |
|------------|------------------------------------------------|----------------------|
| socket()   | create an endpoint                             | server/server.c      |
| bind()     | attach it to port 8080                         | server/server.c      |
| listen()   | start waiting for connections                  | server/server.c      |
| accept()   | a client connected: returns a NEW socket (fd)  | server/server.c      |
| recv()     | read the client's request line                 | common/netutil.c     |
| send()     | write the reply line                           | common/netutil.c     |
| close()    | end the connection                             | server/server.c      |

Client side (our gateway, or ./bin/client) only needs: socket() then connect(), then send()/recv().

## Walk-through (what to click, what to say)
1. Start the server. Point at the log line: "socket + bind + listen done, waiting on port 8080".
   Say: the server is now passively waiting. Nothing else happens until a client connects.
2. In the browser click "Open an account". In the panel you see a TCP line:
   "socket() + connect() ... (TCP 3-way handshake)". Terminal 1 shows "accept() returned socket fd=5
   for client 127.0.0.1:<port>". Say: the client got a random "ephemeral" port; the server stays on 8080;
   the pair (client ip:port, server ip:port) uniquely identifies the connection.
3. Show the messages: panel and server log both show `REGISTER Jay ****` going over and `OK 1001` coming back.
   Say: this is OUR application protocol: one text line per request, one per reply. TCP is only a byte
   stream, so we mark message boundaries with '\n' (see recv_line in common/netutil.c).
   PINs are masked in the logs on purpose.
4. Log in, then click the eye button. Say: the balance is only fetched/shown on request; hiding it is
   a screen-privacy feature in the browser, the number still travels over the connection.
5. Deposit, then withdraw too much. Show `ERR 105 Insufficient funds`: the server decides, not the client.
6. Open a second browser window (private window) and register a second user.
   Terminal 1 shows a different fd and port: one thread per client, each with its own socket.
   Transfer money between them and show both ledgers.
7. Press Ctrl+C on the server: "close()", clean shutdown. Restart and show the balances survived
   (the ledger txn.log is replayed).

## Likely viva questions
- Why TCP and not UDP? Reliable, ordered delivery; a bank cannot lose or reorder a deposit.
- Why can't the browser talk to the C server directly? Browsers only speak HTTP/WebSocket, not raw TCP.
  The gateway translates HTTP/JSON <-> our TCP protocol.
- What is a port? A number that tells the OS which program on the machine gets the data.
- What does accept() return? A new socket dedicated to that one client; the listening socket keeps listening.
- How do many clients work at once? One thread per connected client; per-account locks prevent races.
