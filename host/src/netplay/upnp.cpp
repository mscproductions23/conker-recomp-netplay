#include "upnp.hpp"

#include <chrono>
#include <cstdio>

#include <miniupnpc.h>
#include <upnpcommands.h>

namespace conker::netplay_upnp {
    namespace {
        constexpr int discover_ms = 2000;
        constexpr int lease_seconds = 3600;
        constexpr auto renew_every = std::chrono::minutes(30);

        // A router (or whatever answered) found and its details; freed by the destructor.
        struct Router {
            UPNPDev* devices = nullptr;
            UPNPUrls urls{};
            IGDdatas data{};
            char lan[64] = "";
            char wan[64] = "";
            int kind = UPNP_NO_IGD;   // UPNP_GetValidIGD's answer

            ~Router() {
                if (kind != UPNP_NO_IGD) {
                    FreeUPNPUrls(&urls);
                }
                if (devices != nullptr) {
                    freeUPNPDevlist(devices);
                }
            }

            bool find() {
                int error = 0;
                devices = upnpDiscover(discover_ms, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &error);
                if (devices == nullptr) {
                    return false;
                }
                kind = UPNP_GetValidIGD(devices, &urls, &data, lan, sizeof(lan), wan, sizeof(wan));
                return kind != UPNP_NO_IGD;
            }
        };

        // The ranges no one outside can reach: private, shared (carrier-grade NAT, which includes Tailscale's) and link-local.
        bool unreachable_from_outside(const std::string& address) {
            unsigned a = 0, b = 0, c = 0, d = 0;
            if (std::sscanf(address.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
                return address.empty();
            }
            return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
                (a == 169 && b == 254) || (a == 100 && b >= 64 && b <= 127) || a == 0;
        }

        std::string describe_failure(const Router& router) {
            if (router.kind == UPNP_NO_IGD) {
                return "No router answered (UPnP is off, or the router doesn't have it).";
            }
            if (router.kind == UPNP_DISCONNECTED_IGD) {
                return "The router says it isn't connected to the internet.";
            }
            return "The router refused to open the port.";
        }
    }

    Mapping::~Mapping() {
        stop();
    }

    void Mapping::start(int port) {
        stop();
        std::scoped_lock lock(mutex);
        stopping = false;
        current = State::working;
        external.clear();
        failure.clear();
        worker = std::thread([this, port] { run(port); });
    }

    void Mapping::stop() {
        {
            std::scoped_lock lock(mutex);
            stopping = true;
        }
        wake.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        std::scoped_lock lock(mutex);
        if (current != State::failed) {
            current = State::idle;
        }
    }

    Mapping::State Mapping::state() const {
        std::scoped_lock lock(mutex);
        return current;
    }

    std::string Mapping::external_address() const {
        std::scoped_lock lock(mutex);
        return external;
    }

    std::string Mapping::error() const {
        std::scoped_lock lock(mutex);
        return failure;
    }

    void Mapping::run(int port) {
        auto fail = [this](const std::string& why) {
            std::scoped_lock lock(mutex);
            current = State::failed;
            failure = why;
        };
        Router router;
        if (!router.find()) {
            fail(describe_failure(router));
            return;
        }
        const char* service = router.data.first.servicetype;
        char external_ip[64] = "";
        if (UPNP_GetExternalIPAddress(router.urls.controlURL, service, external_ip) != UPNPCOMMAND_SUCCESS || external_ip[0] == '\0') {
            fail("The router wouldn't say its address on the internet.");
            return;
        }
        if (unreachable_from_outside(external_ip)) {
            fail("The router's own address is private, so it is behind another router: use Tailscale.");
            return;
        }
        const std::string text = std::to_string(port);
        const std::string lease = std::to_string(lease_seconds);
        const auto add = [&] {
            return UPNP_AddPortMapping(router.urls.controlURL, service, text.c_str(), text.c_str(), router.lan,
                "Conker netplay", "UDP", nullptr, lease.c_str()) == UPNPCOMMAND_SUCCESS;
        };
        if (!add()) {
            fail("The router refused to open the port.");
            return;
        }
        {
            std::scoped_lock lock(mutex);
            current = State::mapped;
            external = external_ip;
        }
        std::unique_lock lock(mutex);
        while (!wake.wait_for(lock, renew_every, [this] { return stopping; })) {
            lock.unlock();
            const bool renewed = add();
            lock.lock();
            if (!renewed && !stopping) {
                current = State::failed;
                failure = "The router stopped forwarding the port.";
                break;
            }
        }
        lock.unlock();
        UPNP_DeletePortMapping(router.urls.controlURL, service, text.c_str(), "UDP", nullptr);
    }

    bool discover_only(std::string& external_address, std::string& error) {
        Router router;
        if (!router.find()) {
            error = describe_failure(router);
            return false;
        }
        char external_ip[64] = "";
        if (UPNP_GetExternalIPAddress(router.urls.controlURL, router.data.first.servicetype, external_ip) != UPNPCOMMAND_SUCCESS) {
            error = "The router wouldn't say its address on the internet.";
            return false;
        }
        external_address = external_ip;
        return true;
    }
}
