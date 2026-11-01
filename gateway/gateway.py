#!/usr/bin/env python3
"""SecureBank web gateway: browser (HTTP + JSON) <-> C banking server (TCP line protocol).
Browsers cannot open raw TCP sockets, so this small bridge keeps ONE TCP connection per
browser session (the C server's login state lives on the connection). Python 3 stdlib only."""
import argparse, json, os, re, secrets, socket, threading, time
from http.cookies import SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

IDLE = 280                                   # drop sessions before the C server's 300 s timeout
SAFE = re.compile(r'^[A-Za-z0-9_.\-]{1,50}$')  # whitelist: nothing can inject extra protocol lines
SPEC = {
    'register': ('REGISTER', ['name', 'pin']), 'login': ('LOGIN', ['account', 'pin']),
    'balance': ('BALANCE', []), 'deposit': ('DEPOSIT', ['amount']), 'withdraw': ('WITHDRAW', ['amount']),
    'transfer': ('TRANSFER', ['to', 'amount']), 'history': ('HISTORY', ['n']), 'logout': ('LOGOUT', []),
}
INDEX = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'web', 'index.html')
BACKEND = ('127.0.0.1', 8080)
SESSIONS, SLOCK = {}, threading.Lock()


class Session:
    def __init__(self):
        self.sock = socket.create_connection(BACKEND, timeout=10)
        self.f = self.sock.makefile('rb')
        self.lock, self.last = threading.Lock(), time.time()
        if not self.line().startswith('OK'):
            raise ConnectionError('server refused connection')

    def line(self):
        raw = self.f.readline(1024)
        if not raw:
            raise ConnectionError('closed')
        return raw.decode().rstrip('\r\n')

    def ask(self, text):
        self.sock.sendall(text.encode() + b'\n')
        self.last = time.time()
        return self.line()

    def close(self):
        try: self.sock.close()
        except OSError: pass


def mask(text):                               # never show PINs in the demo trace
    return re.sub(r'^((?:LOGIN|REGISTER) \S+) \S+$', r'\1 ****', text)


def reap():
    now = time.time()
    with SLOCK:
        for sid in [k for k, s in SESSIONS.items() if now - s.last > IDLE]:
            SESSIONS.pop(sid).close()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def reply(self, code, obj, cookie=None):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        if cookie:
            self.send_header('Set-Cookie', f'sid={cookie}; Path=/; HttpOnly; SameSite=Strict')
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.split('?')[0] not in ('/', '/index.html'):
            return self.reply(404, {'ok': False, 'error': 'Not found'})
        with open(INDEX, 'rb') as fh:
            page = fh.read()
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(page)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        self.wfile.write(page)

    def do_POST(self):
        reap()
        cmd = self.path[5:] if self.path.startswith('/api/') else ''
        if cmd not in SPEC:
            return self.reply(404, {'ok': False, 'error': 'Unknown command'})
        try:
            size = int(self.headers.get('Content-Length', 0))
            body = json.loads(self.rfile.read(size)) if 0 < size <= 2048 else {}
            assert isinstance(body, dict)
        except (ValueError, AssertionError):
            return self.reply(400, {'ok': False, 'error': 'Bad request'})

        verb, fields = SPEC[cmd]
        parts = [verb]
        for k in fields:
            v = str(body.get(k, ''))
            if not SAFE.match(v):
                return self.reply(400, {'ok': False, 'error': f'Invalid {k}'})
            parts.append(v)

        jar = SimpleCookie(self.headers.get('Cookie', ''))
        sid = jar['sid'].value if 'sid' in jar else None
        sess, new_cookie = SESSIONS.get(sid), None
        if sess is None:
            if cmd not in ('login', 'register'):
                return self.reply(401, {'ok': False, 'expired': True, 'error': 'Session expired. Please log in again.'})
            try:
                sess = Session()
            except OSError:
                return self.reply(502, {'ok': False, 'error': 'Cannot reach the bank server. Is it running?'})
            sid = new_cookie = secrets.token_hex(16)
            with SLOCK:
                SESSIONS[sid] = sess

        tr = [['http', f'POST /api/{cmd}']]
        if new_cookie:
            tr.append(['conn', f'socket() + connect() to {BACKEND[0]}:{BACKEND[1]}  (TCP 3-way handshake)'])
        line = ' '.join(parts)
        with sess.lock:
            try:
                tr.append(['send', mask(line)])
                resp = sess.ask(line)
                rows = []
                if cmd == 'history' and resp.startswith('OK'):
                    for _ in range(int(resp.split()[1])):
                        f = sess.line().split('|')
                        rows.append(dict(zip(('id', 'time', 'type', 'other', 'amount', 'balance'), f)))
                tr.append(['recv', resp + (f'  (+{len(rows)} ledger lines)' if rows else '')])
            except (OSError, ConnectionError, ValueError):
                with SLOCK:
                    SESSIONS.pop(sid, None)
                sess.close()
                return self.reply(401, {'ok': False, 'expired': True, 'error': 'Connection lost. Please log in again.'})

        tr.append(['resp', 'JSON reply'])
        extra = {'trace': tr, 'conn': {'local': sess.sock.getsockname()[1], 'server': f'{BACKEND[0]}:{BACKEND[1]}'}}
        if resp.startswith('OK'):
            out = {'ok': True, 'data': resp[3:], **extra}
            if cmd == 'history':
                out['rows'] = rows
            return self.reply(200, out, new_cookie)
        _, code, *msg = resp.split(' ', 2)
        return self.reply(200, {'ok': False, 'code': int(code), 'error': msg[0] if msg else 'Error', **extra}, new_cookie)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port', type=int, default=8000)
    ap.add_argument('--backend', default='127.0.0.1:8080', help='host:port of the C server')
    a = ap.parse_args()
    h, p = a.backend.rsplit(':', 1)
    BACKEND = (h, int(p))
    print(f'Gateway on http://127.0.0.1:{a.port}  ->  C server {h}:{p}')
    ThreadingHTTPServer(('127.0.0.1', a.port), Handler).serve_forever()
