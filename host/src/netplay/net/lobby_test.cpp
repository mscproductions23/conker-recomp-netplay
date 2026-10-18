// Tests the lobby of a netplay session, between processes over loopback, with the public Session API
// only (session.hpp). The library keeps one session per process (its callbacks go to the latest one), so
// the host and each joiner are copies of this program, which test.sh starts:
//
//   lobby_test host <port>             hosts a match of 2 and checks what the joiners do to the lobby
//   lobby_test join <code> first       joins, names itself, sends an odd name, then floods the host with lobby
//                                      messages, and must be dropped for it
//   lobby_test join <code> second      joins the place that frees, names itself, and must be told the match has
//                                      started; its lobby messages after that must change nothing
//
// Names and Ready travel to everyone's roster, bad names are cleaned, a flood is dropped, the host starts
// the match, and the lobby doesn't change after the start. Each prints "ok:" and "FAIL:" lines and ends with
// "lobby_test <role>: all passed" (exit code 0) or "lobby_test <role>: N FAILED".

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "session.hpp"

using namespace conker::netplay;

namespace {
    int failures = 0;

    void check(bool ok, const char* what) {
        std::printf("%s: %s\n", ok ? "ok" : "FAIL", what);
        std::fflush(stdout);
        if (!ok) {
            failures++;
        }
    }

    std::chrono::steady_clock::time_point now() {
        return std::chrono::steady_clock::now();
    }

    // One session and what has happened to it.
    struct Peer {
        Session session;
        std::vector<Event> events;

        bool saw(Event::Type type, int port = -2) const {
            for (const Event& event : events) {
                if (event.type == type && (port == -2 || event.port == port)) {
                    return true;
                }
            }
            return false;
        }

        RosterEntry entry(int port) const {
            const std::vector<RosterEntry> roster = session.roster();
            return port >= 0 && port < (int)roster.size() ? roster[port] : RosterEntry{};
        }

        void pump(int ms) {
            const auto end = now() + std::chrono::milliseconds(ms);
            while (now() < end) {
                for (Event& event : session.poll()) {
                    events.push_back(std::move(event));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }

        // Polls until `done`, or `ms` pass. True if it became true.
        bool wait_for(int ms, const std::function<bool()>& done) {
            const auto end = now() + std::chrono::milliseconds(ms);
            while (now() < end) {
                pump(5);
                if (done()) {
                    return true;
                }
            }
            return done();
        }
    };

    bool clean_name(const std::string& name) {
        if (name.empty() || name.size() > name_size || name.front() == ' ' || name.back() == ' ') {
            return false;
        }
        for (unsigned char c : name) {
            if (c < 0x20 || c > 0x7E) {
                return false;
            }
        }
        return true;
    }

    // Says goodbye, then releases the session before the library is shut down (the other way round crashes).
    int finish(const char* role, std::unique_ptr<Peer>& owner) {
        owner->session.leave(Reason::quit);
        owner->pump(300);
        owner.reset();
        shutdown();
        if (failures != 0) {
            std::printf("lobby_test %s: %d FAILED\n", role, failures);
            return 1;
        }
        std::printf("lobby_test %s: all passed\n", role);
        return 0;
    }

    int run_host(uint16_t port) {
        const Hello me{ 1, 1, 1, 1 };
        std::string error;
        if (!startup(error)) {
            std::printf("FAIL: couldn't start the networking: %s\n", error.c_str());
            return 1;
        }
        std::unique_ptr<Peer> owner = std::make_unique<Peer>();
        Peer& host = *owner;
        if (!host.session.host(port, 2, me, error)) {
            std::printf("FAIL: couldn't host: %s\n", error.c_str());
            owner.reset();
            shutdown();
            return 1;
        }
        // test.sh reads the join code from this line.
        std::printf("join code %s\n", host.session.join_code("127.0.0.1:" + std::to_string(port)).c_str());
        std::fflush(stdout);

        // The first joiner takes the place, with the default names, nobody ready.
        const bool in = host.wait_for(30000, [&]() { return host.entry(1).present; });
        check(in, "a joiner is welcomed and takes the second place");
        check(host.saw(Event::Type::joined, 1) && host.saw(Event::Type::roster), "the host got a joined and a roster event");
        check(host.session.roster().size() == 2, "the roster has one entry per port");
        check(host.entry(0).present && host.entry(0).name == "Player 1" && host.entry(1).name == "Player 2",
            "the default names are Player 1 and Player 2");
        check(!host.entry(0).ready && !host.entry(1).ready, "nobody is ready at first");

        // Its name and Ready reach the host.
        const bool alice = host.wait_for(15000, [&]() { return host.entry(1).name == "Alice" && host.entry(1).ready; });
        check(alice, "the joiner's name and Ready reach the host's roster");
        check(host.entry(0).name == "Player 1", "the host's own entry is unchanged by it");

        // An odd name is cleaned: printable, trimmed, at most 16 characters, and Ready false as it said.
        const bool cleaned = host.wait_for(15000, [&]() { return host.entry(1).name != "Alice"; });
        check(cleaned && clean_name(host.entry(1).name) && !host.entry(1).ready,
            "a name with a control character, spaces and too many characters is cleaned");

        // The flood gets it dropped, and its place is freed.
        const bool dropped = host.wait_for(20000, [&]() { return host.saw(Event::Type::left, 1); });
        check(dropped, "a joiner sending its lobby over and over is dropped");
        check(!host.entry(1).present && host.entry(1).name.empty(), "its place in the roster is freed");

        // The second joiner takes the place; the host's own name reaches it, and the host starts the match.
        host.session.set_lobby("Bob", true);
        const bool carol = host.wait_for(30000, [&]() { return host.entry(1).name == "Carol" && host.entry(1).ready; });
        check(carol, "a new joiner takes the free place, and its name and Ready reach the host");
        check(host.entry(0).name == "Bob" && host.entry(0).ready, "the host's own name and Ready are in its roster");
        check(host.session.start(false), "the host can start once every place is taken");

        // After the start nothing in the lobby changes, and asking it to doesn't break anything.
        host.session.set_lobby("Eve", false);
        host.pump(2000);
        check(host.entry(1).name == "Carol" && host.entry(1).ready && host.entry(0).name == "Bob",
            "names sent after the start change nothing");
        return finish("host", owner);
    }

    int run_joiner(const std::string& code, const std::string& which) {
        const Hello me{ 1, 1, 1, 1 };
        std::string error;
        if (!startup(error)) {
            std::printf("FAIL: couldn't start the networking: %s\n", error.c_str());
            return 1;
        }
        std::unique_ptr<Peer> owner = std::make_unique<Peer>();
        Peer& joiner = *owner;
        if (!joiner.session.join(code, me, error)) {
            std::printf("FAIL: couldn't join: %s\n", error.c_str());
            owner.reset();
            shutdown();
            return 1;
        }
        const bool in = joiner.wait_for(15000, [&]() {
            return joiner.saw(Event::Type::welcomed) && joiner.entry(0).present &&
                joiner.session.port() >= 0 && joiner.entry(joiner.session.port()).present;
        });
        check(in, "welcomed, and the roster shows both players");
        if (!in) {
            return finish(which.c_str(), owner);
        }
        const int port = joiner.session.port();
        // The first joiner arrives before the host names itself (Player 1); the second after (Bob).
        check(joiner.entry(0).name == (which == "first" ? "Player 1" : "Bob"), "the host's name is in the roster");

        // Waits a moment, as the host drops a peer that sends its lobby more than 8 times a second.
        joiner.pump(600);
        if (which == "first") {
            joiner.session.set_lobby("Alice", true);
            check(joiner.wait_for(5000, [&]() { return joiner.entry(port).name == "Alice" && joiner.entry(port).ready; }),
                "the joiner's own name and Ready come back in the roster");
            joiner.pump(600);
            joiner.session.set_lobby("  Ev\x01il Name That Is Far Too Long  ", false);
            joiner.pump(1000);
            for (int i = 0; i < 30; i++) {
                joiner.session.set_lobby(i % 2 ? "Flood A" : "Flood B", i % 2 == 0);
            }
            check(joiner.wait_for(10000, [&]() { return joiner.saw(Event::Type::ended); }),
                "the host drops it for flooding its lobby");
        }
        else {
            joiner.session.set_lobby("Carol", true);
            check(joiner.wait_for(5000, [&]() { return joiner.entry(0).name == "Bob" || joiner.entry(port).name == "Carol"; }),
                "its name, or the host's, arrives in the roster");
            check(joiner.wait_for(30000, [&]() { return joiner.saw(Event::Type::started); }),
                "it is told the match has started");
            // After the start its lobby messages change nothing, and it isn't dropped for them.
            joiner.session.set_lobby("Dave", false);
            joiner.pump(1500);
            check(!joiner.saw(Event::Type::ended), "it is not dropped for asking");
        }
        return finish(which.c_str(), owner);
    }
}

int main(int argc, char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "host") == 0) {
        return run_host((uint16_t)std::atoi(argv[2]));
    }
    if (argc == 4 && std::strcmp(argv[1], "join") == 0 &&
        (std::strcmp(argv[3], "first") == 0 || std::strcmp(argv[3], "second") == 0)) {
        return run_joiner(argv[2], argv[3]);
    }
    std::fprintf(stderr, "usage: lobby_test host <port> | lobby_test join <code> first|second\n");
    return 2;
}
