"""The room server: see API.md. Run `python3 room_server.py [--port 27080]`."""
import argparse
import collections
import hmac
import ipaddress
import secrets
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CROCKFORD_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
HEX_DIGITS = "0123456789abcdefABCDEF"

class ApiError(Exception):
    def __init__(self, status, message):
        self.status = status
        self.message = message
        super().__init__(message)

def normalize_join_code(raw):
    s = raw.upper()
    s = s.replace("-", "")
    s = s.replace("O", "0").replace("I", "1").replace("L", "1")
    if len(s) != 6 or any(c not in CROCKFORD_ALPHABET for c in s):
        raise ApiError(400, "code must be 6 characters from the room code alphabet")
    return s

def is_valid_code(code):
    return len(code) == 6 and all(c in CROCKFORD_ALPHABET for c in code)

def parse_key(value):
    if len(value) != 16 or any(c not in HEX_DIGITS for c in value):
        raise ApiError(400, "key must be exactly 16 hex digits")
    return value.lower()

def parse_port(value):
    if not (1 <= len(value) <= 5) or any(c not in "0123456789" for c in value):
        raise ApiError(400, "port must be a decimal number from 1 to 65535")
    n = int(value)
    if n < 1 or n > 65535:
        raise ApiError(400, "port must be a decimal number from 1 to 65535")
    return n

def parse_addr(value):
    if "%" in value:
        raise ApiError(400, "addr must not contain a scope id")
    try:
        ip = ipaddress.ip_address(value)
    except ValueError:
        raise ApiError(400, "addr must be an IPv4 or IPv6 literal")
    if ip.is_unspecified or ip.is_multicast or ip.is_loopback:
        raise ApiError(400, "addr must not be unspecified, multicast or loopback")
    return str(ip)

def parse_token(value):
    if len(value) != 32 or any(c not in HEX_DIGITS for c in value):
        raise ApiError(400, "token must be exactly 32 hex digits")
    return value.lower()

def parse_fields(raw, allowed_scalars, allowed_repeated, required):
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        raise ApiError(400, "body must be UTF-8 text")

    if text.endswith("\n"):
        text = text[:-1]

    if text == "":
        raise ApiError(400, "body is empty")

    result = {}
    for line in text.split("\n"):
        if line.endswith("\r"):
            line = line[:-1]
        if line == "":
            raise ApiError(400, "blank line in body")
        if "=" not in line:
            raise ApiError(400, "malformed line, expected key=value")
        key, value = line.split("=", 1)
        if key in allowed_scalars:
            if key in result:
                raise ApiError(400, "duplicate field: " + key)
            result[key] = value
        elif key in allowed_repeated:
            result.setdefault(key, []).append(value)
        else:
            raise ApiError(400, "unknown field: " + key)

    for name in required:
        if name not in result:
            raise ApiError(400, "missing required field: " + name)

    return result

class RateLimiter:
    def __init__(self, window, max_tracked):
        self.window = window
        self.max_tracked = max_tracked
        self.buckets = collections.OrderedDict()

    def _get(self, addr, now):
        if addr in self.buckets:
            self.buckets.move_to_end(addr)
            return self.buckets[addr]
        if len(self.buckets) >= self.max_tracked:
            self._prune_all(now)
            while len(self.buckets) >= self.max_tracked:
                self.buckets.popitem(last=False)
        entry = {}
        self.buckets[addr] = entry
        return entry

    def _prune_deque(self, dq, now):
        while dq and now - dq[0] >= self.window:
            dq.popleft()

    def _prune_all(self, now):
        for addr in list(self.buckets):
            entry = self.buckets[addr]
            for dq in entry.values():
                self._prune_deque(dq, now)
            if all(not dq for dq in entry.values()):
                del self.buckets[addr]

    def count(self, addr, bucket, now):
        entry = self._get(addr, now)
        dq = entry.setdefault(bucket, collections.deque())
        self._prune_deque(dq, now)
        return len(dq)

    def allow(self, addr, bucket, limit, now):
        return self.count(addr, bucket, now) < limit

    def record(self, addr, bucket, now):
        entry = self._get(addr, now)
        entry.setdefault(bucket, collections.deque()).append(now)

MAX_BODY = 1024
REG_LIMIT = 10
JOIN_FAIL_LIMIT = 20
RATE_WINDOW = 60.0
MAX_TRACKED = 4096
PATHS = ("/v1/host", "/v1/beat", "/v1/close", "/v1/join")
ALLOWED_FIELDS = {
    "/v1/host": ({"key", "port"}, {"addr"}, {"key", "port", "addr"}),
    "/v1/beat": ({"code", "token"}, set(), {"code", "token"}),
    "/v1/close": ({"code", "token"}, set(), {"code", "token"}),
    "/v1/join": ({"code"}, set(), {"code"}),
}

class RoomServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, server_address, ttl=120, max_rooms=500):
        super().__init__(server_address, RoomHandler)
        self.ttl = ttl
        self.max_rooms = max_rooms
        self.rooms = {}
        self.lock = threading.Lock()
        self.limiter = RateLimiter(RATE_WINDOW, MAX_TRACKED)

    def new_code(self):
        for _ in range(100):
            code = "".join(secrets.choice(CROCKFORD_ALPHABET) for _ in range(6))
            if code not in self.rooms:
                return code
        raise ApiError(503, "server full")

    def sweep(self, now):
        for code in [c for c, r in self.rooms.items() if now >= r.expires]:
            del self.rooms[code]
            print("room expired code=" + code, file=sys.stderr, flush=True)

class Room:
    def __init__(self, code, key, port, addrs, public, token, expires):
        self.code = code
        self.key = key
        self.port = port
        self.addrs = addrs
        self.public = public
        self.token = token
        self.expires = expires

class RoomHandler(BaseHTTPRequestHandler):
    server_version = "room-server"
    sys_version = ""
    timeout = 10
    protocol_version = "HTTP/1.0"

    def log_message(self, format, *args):
        pass

    def _send(self, status, text, extra_headers=()):
        data = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", len(data))
        self.send_header("Connection", "close")
        for key, value in extra_headers:
            self.send_header(key, value)
        self.end_headers()
        self.wfile.write(data)

    def _method_not_allowed(self):
        self._send(405, "error=method not allowed\n", [("Allow", "POST")])

    do_GET = do_PUT = do_DELETE = do_PATCH = do_HEAD = do_OPTIONS = _method_not_allowed

    def do_POST(self):
        try:
            self._handle_post()
        except ApiError as e:
            self._send(e.status, "error=" + e.message + "\n")
        except Exception:
            self._send(500, "error=internal error\n")

    def _caller(self):
        peer = self.client_address[0]
        ip = ipaddress.ip_address(peer)
        if ip.is_loopback:
            xff = self.headers.get("X-Forwarded-For")
            if xff:
                first = xff.split(",")[0].strip()
                try:
                    cand = ipaddress.ip_address(first)
                except ValueError:
                    cand = None
                if cand is not None and "%" not in first:
                    return str(cand)
        return str(ip)

    def _read_body(self):
        cl = self.headers.get("Content-Length")
        if cl is None:
            raise ApiError(411, "content-length required")
        if not (cl.isascii() and cl.isdigit()):
            raise ApiError(400, "bad content-length")
        n = int(cl)
        if n > MAX_BODY:
            raise ApiError(413, "body too large")
        data = self.rfile.read(min(n, MAX_BODY + 1))
        if len(data) != n:
            raise ApiError(400, "body shorter than content-length")
        return data

    def _handle_post(self):
        path = self.path
        if path not in PATHS:
            raise ApiError(404, "not found")
        caller = self._caller()
        now = time.monotonic()
        with self.server.lock:
            self.server.sweep(now)
            if path == "/v1/host":
                if not self.server.limiter.allow(caller, "reg", REG_LIMIT, now):
                    raise ApiError(429, "too many registrations")
                self.server.limiter.record(caller, "reg", now)
            elif path == "/v1/join":
                if not self.server.limiter.allow(caller, "fail", JOIN_FAIL_LIMIT, now):
                    raise ApiError(429, "too many failed joins")
        raw = self._read_body()
        scalars, repeated, required = ALLOWED_FIELDS[path]
        fields = parse_fields(raw, scalars, repeated, required)
        if path == "/v1/host":
            self._host(fields, caller)
        elif path == "/v1/beat":
            self._beat(fields)
        elif path == "/v1/close":
            self._close(fields)
        else:
            self._join(fields, caller)

    def _host(self, fields, caller):
        key = parse_key(fields["key"])
        port = parse_port(fields["port"])
        addrs = []
        for a in fields["addr"]:
            p = parse_addr(a)
            if p not in addrs:
                addrs.append(p)
        if len(fields["addr"]) > 6:
            raise ApiError(400, "at most 6 addr lines")
        with self.server.lock:
            now = time.monotonic()
            self.server.sweep(now)
            if len(self.server.rooms) >= self.server.max_rooms:
                raise ApiError(503, "server full")
            code = self.server.new_code()
            token = secrets.token_hex(16)
            self.server.rooms[code] = Room(code, key, port, addrs, caller, token, now + self.server.ttl)
            print("room opened code=" + code, file=sys.stderr, flush=True)
        self._send(200, "code=%s\ntoken=%s\npublic=%s\nttl=%d\n" % (code, token, caller, self.server.ttl))

    def _beat(self, fields):
        code = fields["code"]
        if not is_valid_code(code):
            raise ApiError(400, "code must be 6 characters from the room code alphabet")
        token = parse_token(fields["token"])
        with self.server.lock:
            now = time.monotonic()
            self.server.sweep(now)
            room = self.server.rooms.get(code)
            if room is None:
                raise ApiError(404, "no such room")
            if not hmac.compare_digest(room.token.encode("ascii"), token.encode("ascii")):
                raise ApiError(403, "wrong token")
            room.expires = now + self.server.ttl
        self._send(200, "ok=1\n")

    def _close(self, fields):
        code = fields["code"]
        if not is_valid_code(code):
            raise ApiError(400, "code must be 6 characters from the room code alphabet")
        token = parse_token(fields["token"])
        with self.server.lock:
            now = time.monotonic()
            self.server.sweep(now)
            room = self.server.rooms.get(code)
            if room is None:
                raise ApiError(404, "no such room")
            if not hmac.compare_digest(room.token.encode("ascii"), token.encode("ascii")):
                raise ApiError(403, "wrong token")
            del self.server.rooms[code]
            print("room closed code=" + code, file=sys.stderr, flush=True)
        self._send(200, "ok=1\n")

    def _join(self, fields, caller):
        code = normalize_join_code(fields["code"])
        with self.server.lock:
            now = time.monotonic()
            self.server.sweep(now)
            room = self.server.rooms.get(code)
            if room is None:
                self.server.limiter.record(caller, "fail", now)
                raise ApiError(404, "no such room")
            addrs = list(room.addrs)
            if room.public not in addrs:
                addrs.append(room.public)
            key = room.key
            port = room.port
        lines = ["key=" + key, "port=%d" % port] + ["addr=" + a for a in addrs]
        self._send(200, "\n".join(lines) + "\n")

def make_server(host, port, ttl=120, max_rooms=500):
    return RoomServer((host, port), ttl=ttl, max_rooms=max_rooms)

def main(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=27080)
    parser.add_argument("--ttl", type=int, default=120)
    parser.add_argument("--max-rooms", type=int, default=500)
    args = parser.parse_args(argv)
    if args.port < 0 or args.port > 65535:
        parser.error("Port must be between 0 and 65535")
    if args.ttl < 1:
        parser.error("TTL must be at least 1")
    if args.max_rooms < 1:
        parser.error("Max rooms must be at least 1")
    server = make_server(args.host, args.port, args.ttl, args.max_rooms)
    print("room server listening on %s:%d" % (args.host, server.server_address[1]), file=sys.stderr, flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    print("room server stopped", file=sys.stderr, flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())
