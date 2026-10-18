// Asks the router (UPnP, which miniupnpc also does NAT-PMP for) to forward the host's UDP port, so friends
// outside the home network can reach a match without the host touching the router's settings. It starts
// in a thread, since the router can take seconds to answer; a player sees its state and a sentence for
// each failure. Stopping (or the destructor) removes the mapping, and a mapping left behind by a crash
// expires by itself (the lease is an hour).
#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace conker::netplay_upnp {
    class Mapping {
    public:
        enum class State { idle, working, mapped, failed };

        Mapping() = default;
        ~Mapping();
        Mapping(const Mapping&) = delete;
        Mapping& operator=(const Mapping&) = delete;

        // Starts forwarding UDP `port` (the same number outside and in) and returns at once.
        void start(int port);
        // Stops, and removes the mapping if there is one. Safe to call again.
        void stop();

        State state() const;
        // The router's address on the internet, once mapped (empty before).
        std::string external_address() const;
        // Why it failed, in a sentence for a player (empty unless failed).
        std::string error() const;

    private:
        void run(int port);

        mutable std::mutex mutex;
        std::condition_variable wake;
        std::thread worker;
        bool stopping = false;
        State current = State::idle;
        std::string external;
        std::string failure;
    };

    // Looks for a router and reads its external address, changing nothing on it (for upnp_probe). True if one answered.
    bool discover_only(std::string& external_address, std::string& error);
}
