// Looks for a UPnP router and prints its external address. Changes nothing on it.
#include <cstdio>

#include "upnp.hpp"

int main() {
    std::string address, error;
    if (conker::netplay_upnp::discover_only(address, error)) {
        std::printf("router found, external address %s\n", address.c_str());
        return 0;
    }
    std::printf("%s\n", error.c_str());
    return 1;
}
