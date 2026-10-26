#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <cstdio>
#include "addresses.hpp"

#include <algorithm>
#include <string>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#elif defined(_WIN32)
#include <winsock2.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace conker::netplay_helpers {

std::string address_kind(uint32_t host_order_ipv4) {
    if (host_order_ipv4 == 0x00000000 ||
        (host_order_ipv4 >= 0x7F000000 && host_order_ipv4 <= 0x7FFFFFFF) ||
        (host_order_ipv4 >= 0xA9FE0000 && host_order_ipv4 <= 0xA9FEFFFF)) {
        return "";
    }
    if (host_order_ipv4 >= 0x64400000 && host_order_ipv4 <= 0x647FFFFF) {
        return "Tailscale";
    }
    if ((host_order_ipv4 >= 0x0A000000 && host_order_ipv4 <= 0x0AFFFFFF) ||
        (host_order_ipv4 >= 0xAC100000 && host_order_ipv4 <= 0xAC1FFFFF) ||
        (host_order_ipv4 >= 0xC0A80000 && host_order_ipv4 <= 0xC0A8FFFF)) {
        return "Home network";
    }
    return "Other";
}

namespace {
    std::string format_ip(uint32_t host_order_ip) {
        char buf[INET_ADDRSTRLEN];
        in_addr a{};
        a.s_addr = htonl(host_order_ip);
        inet_ntop(AF_INET, &a, buf, sizeof buf);
        return std::string(buf);
    }
}

std::vector<LocalAddress> local_addresses() {
    std::vector<LocalAddress> addresses;

#if defined(__linux__) || defined(__APPLE__)
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return {};

    for (ifaddrs* ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET ||
            !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK)) {
            continue;
        }

        uint32_t host_ip = ntohl(reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr)->sin_addr.s_addr);
        std::string kind = address_kind(host_ip);
        if (!kind.empty()) {
            addresses.push_back({format_ip(host_ip), kind});
        }
    }
    freeifaddrs(list);
#elif defined(_WIN32)
    std::vector<unsigned char> buf(15000);
    ULONG size = static_cast<ULONG>(buf.size());
    ULONG ret;

    while (true) {
        ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                   nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &size);
        if (ret == ERROR_BUFFER_OVERFLOW) {
            buf.resize(size);
        } else {
            break;
        }
    }

    if (ret != NO_ERROR) return {};

    for (auto* ad = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()); ad; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }

        for (auto* ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
            if (!ua->Address.lpSockaddr || ua->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }

            uint32_t host_ip = ntohl(reinterpret_cast<const sockaddr_in*>(ua->Address.lpSockaddr)->sin_addr.S_un.S_addr);
            std::string kind = address_kind(host_ip);
            if (!kind.empty()) {
                addresses.push_back({format_ip(host_ip), kind});
            }
        }
    }
#endif

    std::vector<std::string> seen_addresses;
    std::vector<LocalAddress> unique_addresses;
    for (const auto& addr : addresses) {
        if (std::find(seen_addresses.begin(), seen_addresses.end(), addr.address) == seen_addresses.end()) {
            seen_addresses.push_back(addr.address);
            unique_addresses.push_back(addr);
        }
    }

    auto rank = [](const std::string& kind) {
        if (kind == "Tailscale") return 0;
        if (kind == "Home network") return 1;
        return 2;
    };

    std::stable_sort(unique_addresses.begin(), unique_addresses.end(), [&](const LocalAddress& a, const LocalAddress& b) {
        return rank(a.kind) < rank(b.kind);
    });

    return unique_addresses;
}


std::vector<std::string> reachable_candidates(const std::vector<std::string>& host_addresses,
    const std::vector<LocalAddress>& mine) {
    bool have_tailscale = false;
    for (const LocalAddress& local : mine) {
        have_tailscale = have_tailscale || local.kind == "Tailscale";
    }
    auto prefix24 = [](const std::string& address) {
        const size_t last = address.rfind('.');
        return last == std::string::npos ? std::string() : address.substr(0, last + 1);
    };
    std::vector<std::string> keep;
    for (const std::string& address : host_addresses) {
        unsigned a = 0, b = 0, c = 0, d = 0;
        char extra = 0;
        if (std::sscanf(address.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
            keep.push_back(address);    // IPv6 (or anything that isn't a dotted IPv4): not ours to judge
            continue;
        }
        const std::string kind = address_kind((a << 24) | (b << 16) | (c << 8) | d);
        if (kind.empty()) {
            continue;
        }
        if (kind == "Tailscale") {
            if (have_tailscale) {
                keep.push_back(address);
            }
        }
        else if (kind == "Home network") {
            for (const LocalAddress& local : mine) {
                if (local.kind == "Home network" && prefix24(local.address) == prefix24(address)) {
                    keep.push_back(address);
                    break;
                }
            }
        }
        else {
            keep.push_back(address);
        }
    }
    return keep;
}
} // namespace conker::netplay_helpers
