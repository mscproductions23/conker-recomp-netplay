// Netplay's lockstep: every peer runs the whole game, and at each controller read gives
// it every player's pad for that read, so the same inputs give each the same game. A peer's own pad is
// sent a few reads ahead (the input delay) to hide the time it takes to arrive; the game waits at a read
// until every pad for it is there. Every so often the peers compare hashes of the game's memory, and a
// difference ends the match (a desync).
//
// Part of the harness (harness.cpp), which gives it the deterministic timeline, the settings profile
// and an empty save on each peer. For now it's started from the command line (the menu is #9):
//
//   --netplay-host <port>        hosts a match on UDP <port> (port 1 of the game is the host's)
//   --players <n>                ...of <n> players, 2 to 4 (2 by default)
//   --split-screen               (hosting) let each player choose what they see; by default every player
//                                sees only their own view of the split screen (the match's rule, for all)
//   --game-mode <name>           (hosting) starts a scripted match of that game mode, instead of leaving the
//                                players to the game's menus: deathmatch, war, tank, race, beach, raptor or heist
//   --game-variant <0-3>         (hosting, with --game-mode) which entry of the mode's list the script picks
//                                (0 by default)
//   --netplay-address <address>  (hosting) this computer's address as the others reach it: the join code
//                                is made from it and printed (it holds the match's random key)
//   --netplay-join <code>        joins the host by its join code
//   --netplay-join-candidates <code1,code2,...>  joins the host by the first of several join codes (comma separated)
//                                that answers: the same host at several addresses, tried in order
//   --input-delay <reads>        how many reads ahead a peer's own pads are sent (2 by default, 0 to 8;
//                                every peer must give the same)
//   --netplay-desync-at <read>   (a test) changes a byte of game memory at that read, which the next
//                                hash comparison must catch
//
// The game waits at its start for every player to join, or in lobby mode (see set_lobby_mode) for the host
// to start it. With --replay, each peer's own pads come from its port in the recording instead of its
// controller: peers replaying the same recording give the game exactly what a replay alone would
// (tools/netplay/lockstep.sh tests that).

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ultramodern/input.hpp"

#include "net/packets.hpp"

namespace conker::lockstep {
    // A command line option of lockstep's, taking its value too (true if it was one).
    bool parse_option(int argc, char** argv, int& i);
    // Hosting or joining.
    bool active();
    // The same as the options, from the menu (frontend's netplay.cpp).
    void set_host(int port, int players, bool own_view = true);
    void set_join(const std::string& where);
    // Joiner: the join codes to try in order, for one host reached at several addresses (the same port and key,
    // as a room server gives them). The first is joined; when one doesn't answer (the connection times out before
    // the welcome) the next is tried, and so on, until one welcomes or the last fails. A refusal (wrong build, ROM,
    // mods or key) or a started match is not passed on to the next code. Replaces any set_join code, and set_join
    // clears the list again; an empty list does nothing.
    void set_join_candidates(const std::vector<std::string>& codes);
    // Checks the options, before the game starts (false: a message was printed).
    bool prepare();
    // Hosting: the join code for this match at `address` (this computer's, as the others reach it; the
    // port is added if it hasn't one), or empty if it isn't an address or nothing is hosted yet. It holds
    // the match's key, which is new each time a match is hosted.
    std::string join_code(std::string address);

    // Connecting, a step at a time, so a menu can show how it goes (and the networking is started the
    // first time): begin hosts or joins (false: `error` says why), then poll_connect until every player
    // is in and the match starts, or it fails. `status` says how it's going, in a sentence for the
    // player. cancel says goodbye and stops.
    enum class Progress { waiting, started, failed };
    bool begin(const netplay::Hello& me, std::string& error);
    Progress poll_connect(std::string& status);
    void cancel();
    // The match has started (from the menu or connect).
    bool connected();
    // The Hello begin was given.
    const netplay::Hello& sent_hello();
    // begin, then poll_connect until the match starts, printing how it goes (the command line's way, in
    // harness::start; false: it couldn't, and a message was printed).
    bool connect(const netplay::Hello& me);
    // Whether the match's rule is that every player sees only their own view of the split screen (the
    // host's choice, sent with Start), once connected. Not enforceable: every machine draws every view,
    // so a modified build could show them; it keeps honest players on the same footing.
    bool own_view_for_everyone();
    // This peer's port and the match's players, once connected.
    int port();
    int players();

    // Lobby mode: the host starts the match by an explicit call, not when the last player joins. Off by
    // default, which is the command line's and the test's way. Each peer names itself and says if it's ready;
    // the roster is what everyone has, one entry per port.
    // A port's entry: the name, Ready flag and controller, the ping and jitter in ms (the host's view of
    // this peer's connection, 0 until measured), and the last pad this peer sent in the lobby.
    struct LobbyEntry {
        bool present = false;
        bool ready = false;
        std::string name;
        uint8_t controller = 0;
        int ping_ms = 0;
        int jitter_ms = 0;
        netplay::Pad pad{};
        bool pad_seen = false;
    };
    // on: the host starts the match with lobby_start(), not when the last player joins. Set before begin().
    void set_lobby_mode(bool on);
    // This peer's name and Ready flag, shown to everyone (does nothing without a session).
    void set_lobby(const std::string& name, bool ready, uint8_t controller = 0);
    // This peer's pad, sent in the lobby (does nothing without a session).
    void send_lobby_pad(const netplay::Pad& pad);
    // The roster as this peer knows it, one entry per port (empty without a session).
    std::vector<LobbyEntry> lobby();
    // Host in lobby mode: every port is taken and every present player is ready (the host's own entry too).
    bool lobby_can_start();
    // Host in lobby mode: starts the match now if lobby_can_start(). True if it started.
    bool lobby_start();
    // Host: the input delay, in reads, that the roster's pings call for (2 with no pings yet).
    int recommended_delay();
    // Host's choice of the input delay: 0 is automatic (recommended_delay()), 1 to 8 forces that many reads.
    void set_delay_override(int reads);
    // The input delay the host will start with: the override if set, else recommended_delay().
    int chosen_delay();
    // Host's pick for the next lobby_start: mode 0 is none (the players use the game's menus), 1 to 7
    // is a scripted match of that game mode (see Start in packets.hpp), variant 0 to 3 is which entry
    // of its list the script picks. Out-of-range values are 0.
    void set_game_mode(int mode, int variant);
    // The game mode of the match under way (see set_game_mode), 0 before a match and for none.
    int match_mode();
    // The variant of the match under way, 0 before a match and for none.
    int match_variant();
    // A notice for the menu about a refused player (empty if none), cleared once read.
    std::string lobby_notice();

    // This peer's own pad for read `for_read`: `live` holds the pads the controllers give now, or is null
    // for the reads before the first (sent at the start, as nobody has played yet).
    using PadSource = std::function<netplay::Pad(uint64_t for_read, const ultramodern::input::Pad* live)>;
    // At controller read `read`, on the game thread: sends this peer's pad for read + delay, waits for
    // every player's pad for `read` and puts them in `pads`, and compares the memory hash (`hash` is
    // called for it every so often). False if the match has ended (a message was printed): the game
    // can't go on.
    bool on_read(uint64_t read, ultramodern::input::Pad* pads, const PadSource& source,
        const std::function<uint64_t()>& hash, uint8_t* rdram);
    // Ends the match for `reason`, saying `why` (the reason is the peers').
    void end(netplay::Reason reason, const char* why);
    // Why the match ended, in a sentence for the player (after on_read returned false).
    const std::string& why_ended();
    // Says goodbye (after the last read of a replay): what's been sent still arrives.
    void leave();
    // Says goodbye if not yet, and stops the networking, as the game quits.
    void finish();
}
