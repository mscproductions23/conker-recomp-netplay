# Room server API

A directory, nothing more: it maps a short room code to where a host can be reached. It never sees game
traffic or the ROM. A host registers its addresses and the match's key and gets a code like `K7QF2M`; a
joiner sends the code and gets the addresses and key back. Run it with `python3 room_server.py` (Python
3.9+, standard library only); put Tailscale Funnel or any HTTPS proxy in front of it for friends outside a
tailnet. Everything is plain text, so the game needs no JSON library.

All requests are `POST` with a `text/plain` body of `key=value` lines (a key may repeat); all responses are
`text/plain` the same way. Bodies over 1024 bytes are refused (413). Unknown or malformed requests get 400
and a line `error=<sentence>`.

| Request | Body | Success (200) | Failures |
|---|---|---|---|
| `POST /v1/host` | `key=<16 hex digits>`, `port=<1-65535>`, 1 to 6 `addr=<IPv4 or IPv6 literal>` | `code=<6 chars>`, `token=<32 hex>`, `public=<the address the server saw>`, `ttl=<seconds>` | 400 bad fields, 429 too many registrations from this address, 503 server full |
| `POST /v1/beat` | `code=`, `token=` | `ok=1` | 404 no such room (it expired or was closed), 403 wrong token |
| `POST /v1/close` | `code=`, `token=` | `ok=1` | 404, 403 |
| `POST /v1/join` | `code=<code, any case, dashes ignored, O as 0, I and L as 1>` | `key=<16 hex>`, `port=<n>`, then `addr=` lines: the host's own, in the order it gave, then the public one the server saw (if not already listed) | 404 no such room, 429 too many wrong codes from this address |

* Codes: 6 characters from Crockford's base32 (`0123456789ABCDEFGHJKMNPQRSTVWXYZ`), random, never reused
  while a room is open.
* A room lives `ttl` seconds (120) after the last host/beat; the host beats every 30 s. A closed or expired
  room's code stops working at once.
* The server's idea of the caller's address is the socket peer, or, when that is loopback (a proxy such as
  Tailscale Funnel), the first entry of `X-Forwarded-For`.
* Limits, per caller address: 10 registrations a minute, 20 failed joins a minute. At most 500 rooms.
* The server logs room open/close with the code only, never the key.
