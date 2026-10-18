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
}
