#include "roomclient.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace conker::netplay_rooms;

int failures = 0;

void check(bool ok, const char* what) {
    printf("%s: %s\n", ok ? "ok" : "FAIL", what);
    fflush(stdout);
    if (!ok) ++failures;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool wait_open(Room& room) {
    for (int i = 0; i < 500; ++i) {
        if (room.state() == Room::State::open) return true;
        if (room.state() == Room::State::failed) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: roomclient_test <server base url>\n");
        return 2;
    }

    const std::string server = argv[1];
    const std::string key = "0123456789abcdef";
    const int port = 27015;
    const std::vector<std::string> addrs = {"203.0.113.5"};

    Room room;
    room.open(server, key, port, addrs);
    bool opened = wait_open(room);
    check(opened, "the host's room opened on the server");
    if (!opened) {
        std::string err = room.error();
        check(false, err.c_str());
        printf("roomclient_test: %d FAILED\n", failures);
        return 1;
    }

    const std::string code = room.code();
    check(code.size() == 6, "the room code is 6 characters");
    const std::string pub = room.public_address();
    check(!pub.empty(), "the server reports the host's public address");

    const std::string joiner_code = lower(code.substr(0, 3)) + "-" + lower(code.substr(3));
    Lookup l = lookup(server, joiner_code);
    check(l.ok, "a joiner finds the open room with a lowercase code and a dash");
    check(l.key_hex == key, "the key matches");
    check(l.port == port, "the port matches");
    check(std::find(l.addrs.begin(), l.addrs.end(), std::string("203.0.113.5")) != l.addrs.end(), "the host's own address is listed");
    check(std::find(l.addrs.begin(), l.addrs.end(), pub) != l.addrs.end(), "the public address is listed");

    room.close();
    check(room.state() == Room::State::idle, "close() leaves the room idle");
    Lookup gone = lookup(server, code);
    check(!gone.ok && gone.error == "No room has that code (it may have closed).", "after close the code gives the not-found sentence");

    Lookup bogus = lookup(server, "ZZZZZZ");
    check(!bogus.ok, "an unknown code fails");

    auto t0 = std::chrono::steady_clock::now();
    Http u = post("http://127.0.0.1:1", "", 6);
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(u.status == 0 && u.error == "Couldn't reach the room server.", "an unreachable server gives the couldn't-reach sentence");
    check(secs < 10.0, "the unreachable server does not hang");

    Http bad1 = post("ftp://x", "", 6);
    check(bad1.status == 0 && !bad1.error.empty(), "a non-http URL is refused");
    Http bad2 = post("http://a b", "", 6);
    check(bad2.status == 0 && !bad2.error.empty(), "a URL with a space is refused");

    room.open(server, key, port, addrs);
    check(wait_open(room), "the room opens again after close");
    room.close();
    check(room.state() == Room::State::idle, "the room is idle again after the second close");

    if (failures == 0) {
        printf("roomclient_test: all passed\n");
        return 0;
    }
    printf("roomclient_test: %d FAILED\n", failures);
    return 1;
}
