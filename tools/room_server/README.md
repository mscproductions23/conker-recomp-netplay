# Room server

A small directory that maps a 6-character room code to a host's addresses and
match key. It never sees game traffic or the ROM. Protocol: see `API.md`.

Requires Python 3.9+ and nothing else.

## Run

    python3 room_server.py                  # plain HTTP on 0.0.0.0:27080
    python3 room_server.py --port 8080 --ttl 120 --max-rooms 500

Plain HTTP is just the default; the server does no TLS itself.

## Friends outside your tailnet (Tailscale Funnel)

    tailscale funnel --bg --https=8443 27080

This publishes the local port 27080 over HTTPS through Funnel. Any HTTPS
proxy in front of the server works the same way.

## Privacy

The server holds a host's IP addresses only while that host's room is open.
They are dropped when the room is closed or expires. Logs contain room codes
only, never keys, tokens or addresses.
