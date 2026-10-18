// A netplay session over GameNetworkingSockets: the host listens on a UDP port and the
// others connect to it directly, by its join code (its address, port and the match's key). Connections
// are encrypted by the library (they aren't authenticated: there is no certificate authority, so someone
// in the path could still pose as the host). All that's sent are the packets in packets.hpp, reliably
// (resent until they arrive, in order); the library closes a connection that sends a bigger message, and
// buffers little from each peer.
//
// The handshake: a joiner sends Hello (the match's key, then build, ROM, settings profile and mods
// hashes) within 3 s. The host closes a wrong key at once, telling it nothing else; then it refuses any
// other difference, or a full or started match, and otherwise gives it the lowest free port (the host is
// port 0) with Welcome. The host calls start() when it decides to (lockstep's lobby: when everyone is ready; or at once, for the command line), and the match starts (Start). Then each peer sends its own port's pads (Inputs) and memory hashes (Hash) to
// the host, which passes Inputs on to the others: the host is the hub, so joiners only ever connect to
// it. A peer that sends anything else (another port's inputs, a second Hello) is dropped.
// Before the start, each joiner sends its name, readiness, and controller family (Lobby); the host keeps the roster of every port and sends it to every joiner (Roster).
// In the lobby, the host pings joiners once a second before the start (Ping/Pong) and measures their
// ping and jitter for the roster. The host forwards the joiners' pads to the others (LobbyPad); joiners send their own pads to the host. The start's input delay (Start) is also carried in the packet.
//
// Single-threaded: call poll() often (each controller read, or in a loop) from one thread; it runs the
// library's callbacks and returns what happened.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "packets.hpp"

namespace conker::netplay {
    struct Event {
        enum class Type {
            joined,     // host: a joiner passed the handshake, on `port`
            welcomed,   // joiner: accepted, on `port`, in a match of `players`
            started,    // joiner: every port is taken and the match starts
            packet,     // `packet` arrived (Inputs or Hash), from `port`'s peer (via the host for a joiner)
            left,       // host: `port`'s peer left or was dropped, for `reason`
            ended,      // joiner: refused, or the connection to the host ended, for `reason`
            roster,     // the lobby's roster changed (host: someone joined, left or changed; joiner: a Roster packet arrived). Read it with roster().
            refused     // host: a joiner was refused by the handshake; `reason` says why
        };
        Type type;
        int port = -1;
        int players = 0;
        bool own_view = false;  // started: the host's rule that every player sees only their own view
        int delay = 2;          // started: the input delay the match starts with, in reads (0 to 8)
        Reason reason = Reason::quit;
        std::string detail;     // the library's account of a closed connection, for the log
        Packet packet;
    };

    // The join code (join_code.hpp) for `address` ("1.2.3.4:5000" or "[::1]:5000") and a match's `key`, or
    // empty if it isn't an address and port.
    std::string join_code_for(const std::string& address, uint64_t key);

    // Starts and stops the library, once per process, around any sessions.
    bool startup(std::string& error);
    void shutdown();

    // One port's player in the lobby, as the host knows it, or as a joiner was last told it: present (a
    // player is there), ready, the name, controller family, ping/jitter (ms), and pad (seen or not).
    struct RosterEntry {
        bool present = false;
        bool ready = false;
        std::string name;
        uint8_t controller = 0;
        uint16_t ping_ms = 0;
        uint16_t jitter_ms = 0;
        Pad pad{};
        bool pad_seen = false;
    };

    class Session {
    public:
        Session();
        ~Session();
        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // Hosts a match of `players` (2 to 4) on UDP `port`, on every local address, with a new random key.
        bool host(uint16_t port, int players, const Hello& me, std::string& error);
        // Host: the join code for this match at `address` (the host's, as the others reach it), or empty
        // if it isn't an address and port.
        std::string join_code(const std::string& address) const;
        // Joins the host by its join code (`me`'s key is the code's).
        bool join(const std::string& code, const Hello& me, std::string& error);
        // Host: once every port is taken (false until then), starts the match: tells the joiners, and
        // refuses any more. `own_view`: every player sees only their own view (the match's rule). `delay`: the input delay the match starts with, in reads (0 to 8).
        bool start(bool own_view = false, int delay = 2);
        // Before the match starts: this peer's name, whether it is ready, and its controller family, for the lobby.
        // The name is cut to 16 printable characters (anything else is dropped), or "Player N" (N the port + 1) if none is left.
        // The host sends the roster to every joiner (and pushes a roster event); a joiner sends it to the host.
        // After Start, nothing.
        void set_lobby(const std::string& name, bool ready, uint8_t controller = 0);
        // Sends this peer's pad to the lobby (the caller limits how often it's called; Session does not).
        void send_lobby_pad(const Pad& pad);
        // The lobby's roster: one entry per port (match_players of them), the host's own table or a joiner's
        // last one from the host. Empty before the joiner is welcomed.
        std::vector<RosterEntry> roster() const;

        // What has happened since the last poll.
        std::vector<Event> poll();

        // This peer's own port's pads (a joiner's go to the host, the host's to everyone).
        void send(const Inputs& inputs);
        // This peer's memory hash (a joiner's go to the host, the host's to everyone).
        void send(const Hash& hash);
        // Says goodbye to every peer and closes the connections. The goodbyes go out as the library is
        // polled: poll a moment before deleting the session or stopping the library, or they may be lost.
        void leave(Reason reason);
        // The furthest read a peer's Inputs may reach (their first read plus their count); beyond it they're
        // refused as broken or hostile, before the host passes them on. Lockstep sets it as the game goes;
        // until then, the first reads only.
        void set_horizon(uint64_t reads) { horizon = reads; }

        bool hosting() const { return is_host; }
        int port() const { return own_port; }
        int players() const { return match_players; }

    private:
        struct Peer {
            uint32_t connection = 0;
            int port = -1;          // -1 until the handshake is done
            double connected_at = 0;
            int64_t last_hash = -1; // the read of the last Hash from it (one per read, in order)
            double lobby_window = 0; // the start of this peer's current second of Lobby packets
            int lobby_count = 0; // the Lobby packets it has sent in that second
            uint32_t ping_seq = 0;       // the seq of the last Ping sent to it
            double ping_sent_at = 0;     // when that Ping was sent (now())
            double rtt_samples[8] = {};  // ring of the last 8 round trips, in ms
            int rtt_count = 0;           // samples held (up to 8)
            int rtt_next = 0;            // next ring slot to write
            bool rtt_sent = false;       // a roster broadcast has carried this peer's ping/jitter
            double pong_window = 0;      // the start of this peer's current second of Pong packets
            int pong_count = 0;          // the Pong packets it has sent in that second
            double pad_window = 0;       // the start of this peer's current second of LobbyPad packets
            int pad_count = 0;           // the LobbyPad packets it has sent in that second
        };

        void on_status_changed(const void* info);
        void handle(Peer& peer, const Packet& packet, std::vector<Event>& events);
        void drop(Peer& peer, Reason reason, std::vector<Event>* events);
        void send_to(uint32_t connection, const Packet& packet);
        Peer* find(uint32_t connection);
        // Lobby (host): the Roster packet for `port`'s entry, and sends every entry to every joiner that has a port.
        Roster roster_packet(int port) const;
        void broadcast_roster();
        // Lobby (host): clears `port`'s entry (a joiner left or was dropped before the start) and broadcasts it,
        // pushing a roster event; nothing before the start, for port 0 or with no port.
        void vacate(int port, std::vector<Event>& events);
        friend void status_changed_callback(void* info);

        bool is_host = false;
        bool entry_open = true;
        bool started = false;       // joiner: Start has arrived
        int own_port = -1;
        int match_players = 0;
        uint64_t horizon = 64;
        Hello hello;
        uint32_t listen_socket = 0;
        uint32_t poll_group = 0;
        std::vector<Peer> peers;            // host: everyone connected; joiner: the host
        std::vector<Event> pending_events;  // from the status callback, until the next poll
        // Lobby: the table (host: the roster; joiner: the host's last Roster for each port), and this peer's
        // own name, ready flag, and controller family as set_lobby gave them (sent once welcomed, if set).
        std::vector<RosterEntry> table;
        std::string lobby_raw;      // the name as given, before it's cleaned (a blank one gets "Player N" once the port is known)
        std::string lobby_name;
        bool lobby_ready = false;
        uint8_t lobby_controller = 0; // the controller family set_lobby gave (for the Lobby packet)
        bool lobby_set = false;
        uint32_t ping_counter = 0;   // the seq of the last Ping sent (host); the first Ping is 1
        double last_ping_at = 0;     // host: when Pings were last sent to the joiners (every second, at most)
        double last_roster_at = 0;   // when the roster was last broadcast (host)
        std::vector<int> sent_ping;  // the ping_ms each port had in the last broadcast (host)
        std::vector<int> sent_jitter;// the jitter_ms each port had in the last broadcast (host)
    };
}
