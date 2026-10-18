#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace conker::netplay_helpers {
    struct LocalAddress {
        std::string address;
        std::string kind;
    };

    std::vector<LocalAddress> local_addresses();
    std::string address_kind(uint32_t host_order_ipv4);
    // Which of a host's addresses are worth trying from here, in the order given: a Tailscale address only if this
    // computer has one too, a home-network address only if this computer is on the same /24, and anything else
    // (public addresses, IPv6) always. Trying an unreachable private address would only cost the connection's timeout.
    std::vector<std::string> reachable_candidates(const std::vector<std::string>& host_addresses,
        const std::vector<LocalAddress>& mine);
}
