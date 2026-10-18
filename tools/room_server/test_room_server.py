"""Tests for room_server.py: `python3 -I -m unittest discover -s tools/room_server -v` from the repo root."""
import http.client
import socket
import threading
import time
import unittest

import room_server as rs

KEY = "0123456789abcdef"


def start(ttl=120, max_rooms=500):
    server = rs.make_server("127.0.0.1", 0, ttl=ttl, max_rooms=max_rooms)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def post(server, path, body="", headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", server.server_address[1], timeout=5)
    conn.request("POST", path, body=body.encode() if isinstance(body, str) else body, headers=headers or {})
    response = conn.getresponse()
    text = response.read().decode()
    conn.close()
    return response.status, text


def fields(text):
    return dict(line.split("=", 1) for line in text.splitlines())


def host_body(key=KEY, port="27015", addrs=("2001:db8::1", "192.0.2.10")):
    return "key=%s\nport=%s\n" % (key, port) + "".join("addr=%s\n" % a for a in addrs)


class RoomServerTest(unittest.TestCase):
    def setUp(self):
        self.server = start()
        self.addCleanup(self.server.server_close)
        self.addCleanup(self.server.shutdown)

    def open_room(self, **kwargs):
        status, text = post(self.server, "/v1/host", host_body(**kwargs))
        self.assertEqual(status, 200, text)
        return fields(text)

    def test_round_trip_and_normalised_code(self):
        room = self.open_room()
        self.assertEqual((len(room["code"]), len(room["token"]), room["public"], room["ttl"]), (6, 32, "127.0.0.1", "120"))
        code = room["code"].lower()
        status, text = post(self.server, "/v1/join", "code=%s-%s\n" % (code[:3], code[3:]))
        self.assertEqual(status, 200)
        self.assertEqual(text.splitlines(), ["key=" + KEY, "port=27015", "addr=2001:db8::1", "addr=192.0.2.10", "addr=127.0.0.1"])

    def test_public_address_is_not_repeated(self):
        status, text = post(self.server, "/v1/host", host_body(addrs=("198.51.100.7",)), {"X-Forwarded-For": "198.51.100.7"})
        status, text = post(self.server, "/v1/join", "code=" + fields(text)["code"])
        self.assertEqual(text.splitlines(), ["key=" + KEY, "port=27015", "addr=198.51.100.7"])

    def test_forwarded_for_is_the_caller_when_the_peer_is_loopback(self):
        status, text = post(self.server, "/v1/host", host_body(), {"X-Forwarded-For": "198.51.100.7, 10.0.0.1"})
        self.assertEqual(fields(text)["public"], "198.51.100.7")
        status, text = post(self.server, "/v1/host", host_body(), {"X-Forwarded-For": "not-an-address"})
        self.assertEqual(fields(text)["public"], "127.0.0.1")

    def test_beat_close_and_wrong_token(self):
        room = self.open_room()
        body = "code=%s\ntoken=%s\n" % (room["code"], room["token"])
        self.assertEqual(post(self.server, "/v1/beat", "code=%s\ntoken=%s\n" % (room["code"], "0" * 32))[0], 403)
        self.assertEqual(post(self.server, "/v1/beat", body), (200, "ok=1\n"))
        self.assertEqual(post(self.server, "/v1/close", body), (200, "ok=1\n"))
        self.assertEqual(post(self.server, "/v1/join", "code=" + room["code"])[0], 404)
        self.assertEqual(post(self.server, "/v1/beat", body)[0], 404)

    def test_bad_requests_are_400(self):
        bad = {
            "short key": host_body(key="0123456789abcde"), "non-hex key": host_body(key="0123456789abcdeg"),
            "port 0": host_body(port="0"), "port 65536": host_body(port="65536"), "port text": host_body(port="abc"),
            "port sign": host_body(port="+80"), "hostname": host_body(addrs=("example.com",)),
            "loopback": host_body(addrs=("127.0.0.1",)), "unspecified": host_body(addrs=("0.0.0.0",)),
            "multicast": host_body(addrs=("ff02::1",)), "seven addrs": host_body(addrs=("192.0.2.10",) * 7),
            "duplicate key": host_body() + "key=" + KEY + "\n", "unknown field": host_body() + "foo=1\n",
            "no addr": "key=%s\nport=27015\n" % KEY, "no port": "key=%s\naddr=192.0.2.10\n" % KEY,
            "malformed": host_body() + "nonsense\n", "empty": "", "bad utf-8": b"key=%s\nport=1\naddr=\xff\n" % KEY.encode(),
        }
        for name, body in bad.items():
            with self.subTest(name):
                server = start()   # a fresh one: the registration rate limit counts bad requests too
                self.addCleanup(server.server_close)
                self.addCleanup(server.shutdown)
                self.assertEqual(post(server, "/v1/host", body)[0], 400)

    def test_size_and_method_limits(self):
        self.assertEqual(post(self.server, "/v1/host", "x" * 2000)[0], 413)
        conn = http.client.HTTPConnection("127.0.0.1", self.server.server_address[1], timeout=5)
        conn.request("GET", "/v1/host")
        self.assertEqual(conn.getresponse().status, 405)
        conn.close()
        self.assertEqual(post(self.server, "/v1/nothing", "a=b")[0], 404)
        sock = socket.create_connection(("127.0.0.1", self.server.server_address[1]), timeout=5)
        sock.sendall(b"POST /v1/host HTTP/1.0\r\n\r\n")
        self.assertIn(b" 411 ", sock.recv(200).split(b"\r\n")[0])
        sock.close()

    def test_registration_rate_limit(self):
        statuses = [post(self.server, "/v1/host", host_body())[0] for _ in range(11)]
        self.assertEqual(statuses, [200] * 10 + [429])

    def test_failed_join_rate_limit(self):
        statuses = [post(self.server, "/v1/join", "code=ZZZZZZ\n")[0] for _ in range(21)]
        self.assertEqual(statuses, [404] * 20 + [429])

    def test_server_full(self):
        server = start(max_rooms=2)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        self.assertEqual([post(server, "/v1/host", host_body())[0] for _ in range(3)], [200, 200, 503])


class ExpiryTest(unittest.TestCase):
    def test_a_room_expires_unless_it_beats(self):
        server = start(ttl=1)
        self.addCleanup(server.server_close)
        self.addCleanup(server.shutdown)
        room = fields(post(server, "/v1/host", host_body())[1])
        body = "code=%s\ntoken=%s\n" % (room["code"], room["token"])
        time.sleep(0.6)
        self.assertEqual(post(server, "/v1/beat", body)[0], 200)
        time.sleep(0.6)
        self.assertEqual(post(server, "/v1/join", "code=" + room["code"])[0], 200)   # the beat extended it
        time.sleep(1.2)
        self.assertEqual(post(server, "/v1/join", "code=" + room["code"])[0], 404)


if __name__ == "__main__":
    unittest.main()
