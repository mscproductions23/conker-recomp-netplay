// The launcher's Netplay menu: host a match or join one by its code, wait for
// every player there, then the game starts on every machine at once. It's a modal of its own over the
// launcher; the connecting is lockstep's (lockstep.hpp), a step at each of the menu's updates, so a
// refusal (another build, ROM or settings, a full match) is said here and the player can try again.
//
// A match started from here doesn't quit when it ends (a desync, a player leaving): the game stops taking
// input and a prompt says why, with Quit.
//
// The host's join code is made, once hosting, from the address the host gives and the match's random key:
// the game can't know its public address without a server to ask, so the host looks it up (or gives its
// local one, for a match at home). The code is the host's address, so the menu says to share it only with
// people the host trusts. The input delay is lockstep's default, the same on every machine.

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <SDL.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#include "recompui/recompui.h"
#include "elements/ui_button.h"
#include "elements/ui_container.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"
#include "elements/ui_radio.h"
#include "elements/ui_text_input.h"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

#include "../conker.hpp"
#include "hello.hpp"
#include "lockstep.hpp"
#include "addresses.hpp"
#include "diag.hpp"
#include "lobby.hpp"
#include "settings.hpp"
#include "net/join_code.hpp"
#include "roomclient.hpp"
#include "upnp.hpp"
#include "net/session.hpp"

namespace {
    constexpr int default_port = 27015;

    std::u8string game_id;
    std::filesystem::path stored_rom;
    recompui::Modal* modal = nullptr;

    // From the game thread (the harness), for the main thread: why the match is over.
    std::mutex over_mutex;
    std::string over_text;
    bool over_pending = false;

    // The build as a player can compare it with a friend's: "0.1.6 9296b4d", the version and the first seven
    // characters of the commit, and "(modified)" if the program was built from changed files.
    std::string short_build() {
        const std::string id = conker::netplay::build_id();
        const size_t space = id.find(' ');
        if (space == std::string::npos) {
            return id;
        }
        const std::string commit = id.substr(space + 1);
        const bool modified = commit.find("-dirty-") != std::string::npos;
        return id.substr(0, space) + " " + commit.substr(0, 7) + (modified ? " (modified)" : "");
    }

    // An IPv4 or IPv6 literal as a join code's endpoint (without its port and key), false if it isn't one.
    bool parse_ip(const std::string& text, conker::netplay::Endpoint& out) {
        out = {};
        in_addr v4{};
        in6_addr v6{};
        if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
            std::memcpy(out.ip.data(), &v4, 4);
            return true;
        }
        if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
            out.ipv6 = true;
            std::memcpy(out.ip.data(), &v6, 16);
            return true;
        }
        return false;
    }

    // What was typed without its spaces and dashes; a room server's short code is 4 to 8 of them, far fewer
    // than a join code's 24 or 44.
    std::string without_separators(const std::string& text) {
        std::string out;
        for (char c : text) {
            if (c != ' ' && c != '-') {
                out.push_back(c);
            }
        }
        return out;
    }

    bool looks_like_room_code(const std::string& text) {
        const size_t n = without_separators(text).size();
        return n >= 4 && n <= 8;
    }

    class NetplayPage : public recompui::Element {
        enum class State { idle, connecting, starting };
        State state = State::idle;
        recompui::TextInput* port_input = nullptr;
        recompui::TextInput* address_input = nullptr;
        recompui::TextInput* join_input = nullptr;
        recompui::Label* code_label = nullptr;
        recompui::Button* copy_button = nullptr;
        std::string shown_code;             // the join code on show, for Copy
        recompui::Label* status_label = nullptr;
        recompui::Button* host_button = nullptr;
        recompui::Button* join_button = nullptr;
        recompui::Button* cancel_button = nullptr;
        // The sections that make way for the lobby while connecting, and the lobby itself.
        recompui::Element* host_section = nullptr;
        recompui::Element* join_section = nullptr;
        recompui::Element* display_section = nullptr;
        recompui::Element* rooms_section = nullptr;
        recompui::TextInput* room_input = nullptr;
        std::string room_server_text;           // the room server's address, or empty for none
        // The host's help from outside, started with the lobby and ended with it: a room (the short code) and
        // the router's port. Each stops on its own thread, as the router and the server can take seconds.
        std::shared_ptr<conker::netplay_rooms::Room> room;
        std::shared_ptr<conker::netplay_upnp::Mapping> upnp;
        std::future<conker::netplay_rooms::Lookup> lookup;   // a join by short code, waiting for the server
        bool looking_up = false;
        conker::netplay_lobby::LobbyPanel* lobby_panel = nullptr;
        bool lobby_shown = false;
        int players = 2;
        bool hosting = false;               // a match is hosted (connecting, as the host)
        bool own_view_for_everyone = true;  // hosting: the match's screens rule
        std::string port_text = std::to_string(default_port);
        std::string address_text;
        std::string join_text;
        recompui::Radio* address_radio = nullptr;
        std::vector<conker::netplay_helpers::LocalAddress> addresses;   // this computer's, for the picker
        recompui::Label* join_hint = nullptr;
        recompui::Button* diag_button = nullptr;
        std::string last_status;            // the last status line noted for the diagnostics

        recompui::Label* text(recompui::Element* parent, const std::string& words, recompui::theme::Typography typography,
            recompui::theme::color color = recompui::theme::color::Text) {
            recompui::Label* label = recompui::get_current_context().create_element<recompui::Label>(parent, words, typography);
            label->set_color(color);
            label->set_flex_shrink(0.0f);
            return label;
        }

        recompui::Element* column(recompui::Element* parent, float gap) {
            recompui::Element* box = recompui::get_current_context().create_element<recompui::Element>(parent);
            box->set_display(recompui::Display::Flex);
            box->set_flex_direction(recompui::FlexDirection::Column);
            box->set_gap(gap);
            box->set_width(100.0f, recompui::Unit::Percent);
            box->set_align_items(recompui::AlignItems::FlexStart);
            // The page scrolls when it's taller than the window: its rows keep their size, not squash together.
            box->set_flex_shrink(0.0f);
            return box;
        }

        recompui::Element* row(recompui::Element* parent) {
            recompui::Element* box = recompui::get_current_context().create_element<recompui::Element>(parent);
            box->set_display(recompui::Display::Flex);
            box->set_flex_direction(recompui::FlexDirection::Row);
            box->set_align_items(recompui::AlignItems::Center);
            box->set_gap(16.0f);
            box->set_flex_shrink(0.0f);
            return box;
        }

        recompui::TextInput* input(recompui::Element* parent, const std::string& value, float width) {
            recompui::TextInput* field = recompui::get_current_context().create_element<recompui::TextInput>(parent);
            field->set_width(width);
            field->set_min_width(width);
            field->set_flex_shrink(0.0f);
            field->set_text(value);
            return field;
        }

        // The join code, once hosting (it holds the match's key, new each time).
        void show_code() {
            const bool blank = address_text.find_first_not_of(' ') == std::string::npos;
            const std::string code = hosting ? conker::lockstep::join_code(address_text) : std::string();
            if (code != shown_code) {
                copy_button->set_text("Copy");
            }
            shown_code = code;
            copy_button->set_display(code.empty() ? recompui::Display::None : recompui::Display::Flex);
            if (blank) {
                code_label->set_text("Give your address: the join code is made from it when you host.");
            }
            else if (!hosting) {
                code_label->set_text("Host, and the join code to send the others appears here.");
            }
            else if (code.empty()) {
                code_label->set_text("That isn't an address: four numbers such as 203.0.113.7, or an IPv6 one.");
            }
            else {
                code_label->set_text("Join code: " + code);
            }
            // The lobby shows the code too, as the settings that hold it are hidden while connecting.
            if (lobby_panel != nullptr) {
                lobby_panel->set_join_code(code);
            }
        }

        void set_state(State next, const std::string& status) {
            state = next;
            if (state == State::idle) {
                hosting = false;
                stop_outside_help();
            }
            show_code();
            status_label->set_text(status);
            // Focus moves to a control that stays on screen before the focused one is hidden or disabled:
            // navigating from a hidden element throws in the nav tree.
            const bool lobby = state == State::connecting;
            const recompui::Display settings = lobby ? recompui::Display::None : recompui::Display::Flex;
            if (lobby) {
                // Focus goes to Copy diagnostics, which a held key or pad button can press without harm (Cancel
                // would leave the lobby).
                cancel_button->set_display(recompui::Display::Flex);
                diag_button->focus();
            }
            else {
                host_section->set_display(settings);
                join_section->set_display(settings);
                display_section->set_display(settings);
                rooms_section->set_display(settings);
                host_button->set_enabled(true);
                join_button->set_enabled(true);
                host_button->focus();
                cancel_button->set_display(recompui::Display::None);
            }
            if (lobby) {
                host_button->set_enabled(false);
                join_button->set_enabled(false);
                host_section->set_display(settings);
                join_section->set_display(settings);
                display_section->set_display(settings);
                rooms_section->set_display(settings);
            }
            if (lobby && !lobby_shown) {
                lobby_panel->open(hosting);
            }
            else if (!lobby) {
                lobby_panel->close();
            }
            lobby_shown = lobby;
            // An update applies the text (and, while connecting, polls).
            queue_update();
        }

        // Hosting or joining: the Hello from the stored ROM, then lockstep's begin.
        void begin() {
            std::ifstream file(stored_rom, std::ios::binary);
            const std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (rom.empty()) {
                set_state(State::idle, "Load the ROM first (Start Game on the launcher).");
                return;
            }
            std::string error;
            // The host starts the match from the lobby, once everyone is in and ready.
            conker::lockstep::set_lobby_mode(true);
            if (!conker::lockstep::begin(conker::netplay::hello_for_rom(rom), error)) {
                set_state(State::idle, "Couldn't: " + error);
                return;
            }
            conker::netplay_helpers::note(hosting ? "hosting a match of " + std::to_string(players) + " on port " + port_text
                : "joining by code");
            diag_button->set_text("Copy diagnostics");
            set_state(State::connecting, "Connecting...");
        }

        void on_host() {
            const int port = std::atoi(port_text.c_str());
            if (port < 1 || port > 65535) {
                set_state(State::idle, "The port is a number from 1 to 65535.");
                return;
            }
            if (address_text.find_first_not_of(' ') == std::string::npos) {
                set_state(State::idle, "Give your address first: the join code is made from it.");
                return;
            }
            conker::lockstep::set_host(port, players, own_view_for_everyone);
            hosting = true;
            begin();
            if (state == State::connecting) {
                start_outside_help(port);
            }
            show_code();
        }

        // Hosting: asks the router to forward the port, and the room server for a short code, both in threads.
        void start_outside_help(int port) {
            upnp = std::make_shared<conker::netplay_upnp::Mapping>();
            upnp->start(port);
            room.reset();
            if (room_server_text.empty()) {
                return;
            }
            const auto endpoint = conker::netplay::decode_join_code(conker::lockstep::join_code(address_text));
            if (!endpoint) {
                return;
            }
            char key[17];
            std::snprintf(key, sizeof(key), "%016llx", (unsigned long long)endpoint->key);
            // The address typed, then this computer's own; the server adds the one it sees.
            std::vector<std::string> addrs;
            conker::netplay::Endpoint ignored;
            std::string typed = address_text;
            while (!typed.empty() && typed.back() == ' ') typed.pop_back();
            while (!typed.empty() && typed.front() == ' ') typed.erase(typed.begin());
            if (parse_ip(typed, ignored)) {
                addrs.push_back(typed);
            }
            for (const auto& local : addresses) {
                if (std::find(addrs.begin(), addrs.end(), local.address) == addrs.end()) {
                    addrs.push_back(local.address);
                }
            }
            addrs.resize(std::min<size_t>(addrs.size(), 6));
            room = std::make_shared<conker::netplay_rooms::Room>();
            room->open(room_server_text, key, port, addrs);
        }

        // Ends them without making the player wait for the router or the server.
        void stop_outside_help() {
            if (room || upnp) {
                std::thread([room = std::move(room), upnp = std::move(upnp)]() {
                    if (room) room->close();
                    if (upnp) upnp->stop();
                }).detach();
            }
            room.reset();
            upnp.reset();
            lobby_panel->set_room("", "");
        }

        // What the lobby says of the short code and the router: the code once the room is open.
        void show_outside_help() {
            std::string code;
            std::string status;
            if (room != nullptr) {
                if (room->state() == conker::netplay_rooms::Room::State::open) {
                    code = room->code();
                    if (code.size() == 6) {
                        code.insert(3, "-");
                    }
                }
                else if (room->state() == conker::netplay_rooms::Room::State::failed) {
                    status = "Room server: " + room->error();
                }
                else {
                    status = "Opening a room...";
                }
            }
            if (upnp != nullptr) {
                const auto state_now = upnp->state();
                const std::string router = state_now == conker::netplay_upnp::Mapping::State::mapped
                    ? "Router port open (UPnP)." : state_now == conker::netplay_upnp::Mapping::State::failed
                    ? upnp->error() : "Asking the router to open the port...";
                status += std::string(status.empty() ? "" : " ") + router;
            }
            lobby_panel->set_room(code, status);
        }

        void on_join() {
            if (join_text.find_first_not_of(' ') == std::string::npos) {
                set_state(State::idle, "Type the host's join code first.");
                return;
            }
            if (looking_up) {
                return;
            }
            if (looks_like_room_code(join_text)) {
                if (room_server_text.empty()) {
                    set_state(State::idle, "A short code needs the room server: its address goes under Room server, below.");
                    return;
                }
                lookup = std::async(std::launch::async, [server = room_server_text, code = join_text]() {
                    return conker::netplay_rooms::lookup(server, code);
                });
                looking_up = true;
                status_label->set_text("Looking up the room...");
                queue_update();
                return;
            }
            conker::lockstep::set_join(join_text);
            begin();
        }

        // The room server answered a short code: the host's addresses worth trying from here become join codes.
        void finish_lookup(const conker::netplay_rooms::Lookup& found) {
            if (!found.ok) {
                set_state(State::idle, found.error);
                return;
            }
            const std::vector<std::string> tries = conker::netplay_helpers::reachable_candidates(found.addrs, addresses);
            std::vector<std::string> codes;
            for (const std::string& address : tries) {
                conker::netplay::Endpoint endpoint;
                if (parse_ip(address, endpoint)) {
                    endpoint.port = (uint16_t)found.port;
                    endpoint.key = std::strtoull(found.key_hex.c_str(), nullptr, 16);
                    const std::string code = conker::netplay::encode_join_code(endpoint);
                    if (!code.empty()) {
                        codes.push_back(code);
                    }
                }
            }
            if (codes.empty()) {
                set_state(State::idle, "None of the host's addresses can be reached from here: ask them for a Tailscale address.");
                return;
            }
            conker::netplay_helpers::note("joining by room code: " + std::to_string(codes.size()) + " address(es) to try");
            conker::lockstep::set_join_candidates(codes);
            begin();
        }

        void on_cancel() {
            conker::lockstep::cancel();
            conker::lockstep::set_host(-1, players);
            conker::netplay_helpers::note("cancelled");
            set_state(State::idle, "Cancelled.");
        }

        // What this player picked, kept for next time (the name is the lobby's, kept in the same file).
        void save_settings() {
            const std::filesystem::path folder = recomp::get_config_path();
            conker::netplay_helpers::Settings settings = conker::netplay_helpers::load_settings(folder);
            settings.port = std::clamp(std::atoi(port_text.c_str()), 1, 65535);
            settings.players = players;
            settings.address = address_text;
            settings.own_view_for_everyone = own_view_for_everyone;
            settings.room_server = room_server_text;
            conker::netplay_helpers::save_settings(folder, settings);
        }

        void note_status(const std::string& status) {
            if (status != last_status) {
                last_status = status;
                conker::netplay_helpers::note(status);
            }
        }

        // What to send someone helping with a problem: the build, the system, the controllers, the lobby, and
        // the last things that happened.
        std::string build_diagnostics() const {
            std::string out = "Conker netplay diagnostics\n";
            out += std::string("Build: ") + conker::netplay::build_id() + "\n";
            SDL_version version{};
            SDL_GetVersion(&version);
            out += std::string("System: ") + SDL_GetPlatform() + ", SDL " + std::to_string(version.major) + "." +
                std::to_string(version.minor) + "." + std::to_string(version.patch) + "\n";
            out += "Controllers:\n";
            const auto controllers = conker::frontend::connected_controllers();
            for (_SDL_GameController* controller : controllers) {
                const char* name = SDL_GameControllerName(controller);
                out += std::string("  ") + (name != nullptr ? name : "controller") + " (" +
                    conker::controller_glyphs::info_of(controller).name + ")\n";
            }
            if (controllers.empty()) {
                out += "  none\n";
            }
            const char* states[] = { "idle", "connecting", "starting" };
            out += std::string("State: ") + states[(int)state] + (hosting ? ", hosting" : "") + "\n";
            const auto roster = conker::lockstep::lobby();
            for (size_t i = 0; i < roster.size(); i++) {
                const auto& entry = roster[i];
                out += "  Player " + std::to_string(i + 1) + ": " + (entry.present ? entry.name : std::string("(empty)")) +
                    (entry.present ? std::string(entry.ready ? ", ready" : ", not ready") + ", controller " +
                    std::to_string(entry.controller) + ", ping " + std::to_string(entry.ping_ms) + " ms, jitter " +
                    std::to_string(entry.jitter_ms) + " ms" : std::string()) + "\n";
            }
            out += "Recent events:\n" + conker::netplay_helpers::recent(60) + "\n";
            return out;
        }

        void copy_diagnostics() {
            const std::string report = build_diagnostics();
            if (SDL_SetClipboardText(report.c_str()) == 0) {
                diag_button->set_text("Copied");
                queue_update();
            }
        }

    protected:
        std::string_view get_type_name() override { return "NetplayPage"; }

        void process_event(const recompui::Event& e) override {
            if (e.type != recompui::EventType::Update) {
                return;
            }
            if (looking_up) {
                if (lookup.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    looking_up = false;
                    finish_lookup(lookup.get());
                }
                else {
                    queue_update();
                }
            }
            if (state == State::connecting) {
                std::string status;
                switch (conker::lockstep::poll_connect(status)) {
                    case conker::lockstep::Progress::waiting:
                        status_label->set_text(status);
                        lobby_panel->refresh();
                        show_outside_help();
                        note_status(status);
                        {
                            const std::string notice = conker::lockstep::lobby_notice();
                            if (!notice.empty()) {
                                lobby_panel->show_notice(notice);
                                conker::netplay_helpers::note(notice);
                            }
                        }
                        break;
                    case conker::lockstep::Progress::failed:
                        conker::lockstep::set_host(-1, players);
                        set_state(State::idle, status);
                        break;
                    case conker::lockstep::Progress::started:
                        set_state(State::starting, "Starting the game...");
                        conker::harness::begin_menu_match([](const std::string& why) {
                            std::lock_guard lock(over_mutex);
                            over_text = why;
                            over_pending = true;
                        });
                        recomp::start_game(game_id, {});
                        recompui::hide_all_contexts();
                        return;
                }
                // Polled again next frame while connecting (set_state starts it).
                queue_update();
            }
        }

    public:
        NetplayPage(recompui::ResourceId rid, recompui::Element* parent)
            : Element(rid, parent, recompui::Events(recompui::EventType::Update)) {
            using recompui::theme::Typography;
            using recompui::theme::color;
            recompui::ContextId context = recompui::get_current_context();
            set_display(recompui::Display::Flex);
            set_flex_direction(recompui::FlexDirection::Column);
            set_gap(28.0f);
            set_padding(32.0f);
            set_width(100.0f, recompui::Unit::Percent);
            set_background_color(recompui::theme::color::Background1);
            set_as_navigation_container(recompui::NavigationType::Vertical);
            // Taller than the window (the lobby with four players): scroll, don't squash.
            set_overflow_y(recompui::Overflow::Auto);

            recompui::Element* intro = column(this, 8.0f);
            text(intro, "Netplay", Typography::Header3);
            text(intro, "Play multiplayer over the internet: everyone runs the game, and every player's controller "
                "is passed to the others. Everyone needs this same build and the same ROM, with no mods on (texture "
                "packs are fine). You play with your first controller; the game starts from the "
                "beginning, with an empty save.",
                Typography::Body, color::TextDim);

            text(intro, "Build " + short_build() + ". Everyone in a match needs the same one: compare this line.",
                Typography::LabelSM, color::TextDim);

            // What this player picked last time.
            const conker::netplay_helpers::Settings saved = conker::netplay_helpers::load_settings(recomp::get_config_path());
            players = saved.players;
            port_text = std::to_string(saved.port);
            address_text = saved.address;
            own_view_for_everyone = saved.own_view_for_everyone;
            room_server_text = saved.room_server;

            // Hosting.
            recompui::Element* host = column(this, 12.0f);
            host_section = host;
            text(host, "Host a match", Typography::LabelLG);
            recompui::Element* host_row = row(host);
            text(host_row, "Players", Typography::LabelMD);
            recompui::Radio* count_radio = context.create_element<recompui::Radio>(host_row);
            for (int n = 2; n <= conker::netplay::max_players; n++) {
                count_radio->add_option(std::to_string(n));
            }
            count_radio->set_index((uint32_t)(players - 2));
            count_radio->add_index_changed_callback([this](uint32_t index) {
                players = 2 + (int)index;
                save_settings();
            });
            text(host_row, "UDP port", Typography::LabelMD);
            port_input = input(host_row, port_text, 120.0f);
            port_input->add_text_changed_callback([this](const std::string& value) {
                port_text = value;
                save_settings();
            });
            text(host_row, "Your address", Typography::LabelMD);
            address_input = input(host_row, address_text, 320.0f);
            address_input->add_text_changed_callback([this](const std::string& value) {
                address_text = value;
                save_settings();
                show_code();
            });
            // This computer's addresses, to pick one instead of looking it up: Tailscale first (the friends on the
            // same Tailscale network reach it without opening anything), then the home network.
            addresses = conker::netplay_helpers::local_addresses();
            if (!addresses.empty()) {
                recompui::Element* picker_row = row(host);
                text(picker_row, "Use", Typography::LabelMD);
                address_radio = context.create_element<recompui::Radio>(picker_row);
                int pick = -1;
                for (size_t i = 0; i < addresses.size(); i++) {
                    address_radio->add_option(addresses[i].kind + " " + addresses[i].address);
                    if (addresses[i].address == address_text) {
                        pick = (int)i;
                    }
                }
                address_radio->add_option("Other: type it above");
                if (pick < 0) {
                    // Nothing typed (or an address from another network): the best one found.
                    pick = address_text.empty() ? 0 : (int)addresses.size();
                    if (address_text.empty()) {
                        address_text = addresses[0].address;
                        address_input->set_text(address_text);
                    }
                }
                address_radio->set_index((uint32_t)pick);
                address_radio->add_index_changed_callback([this](uint32_t index) {
                    if (index < addresses.size()) {
                        address_text = addresses[index].address;
                        address_input->set_text(address_text);
                        save_settings();
                        show_code();
                        queue_update();
                    }
                });
            }
            recompui::Element* screens_row = row(host);
            text(screens_row, "Screens", Typography::LabelMD);
            recompui::Radio* screens_radio = context.create_element<recompui::Radio>(screens_row);
            screens_radio->add_option("Everyone sees only their own view");
            screens_radio->add_option("Players choose");
            screens_radio->set_index(own_view_for_everyone ? 0 : 1);
            screens_radio->add_index_changed_callback([this](uint32_t index) {
                own_view_for_everyone = index == 0;
                save_settings();
            });
            recompui::Element* code_row = row(host);
            code_label = text(code_row, "", Typography::LabelMD);
            copy_button = context.create_element<recompui::Button>(code_row, "Copy", recompui::ButtonStyle::Tertiary);
            copy_button->add_pressed_callback([this]() {
                // Said on the button: the status line is busy while waiting for players.
                if (!shown_code.empty() && SDL_SetClipboardText(shown_code.c_str()) == 0) {
                    copy_button->set_text("Copied");
                    queue_update();
                }
            });
            text(host, "The others connect to that port on your computer: if you're behind a router, forward it "
                "(UDP) to this computer. Look up your public address on any \"what is my IP\" site; at home, "
                "your local one works too.", Typography::Body, color::TextDim);
            text(host, "The join code is your IP address (with a key for this match): only share it with people you "
                "trust. Joining a match shows the host your IP address too.", Typography::Body, color::TextDim);
            host_button = context.create_element<recompui::Button>(host, "Host", recompui::ButtonStyle::Primary);
            host_button->add_pressed_callback([this]() { on_host(); });

            // Joining.
            recompui::Element* join = column(this, 12.0f);
            join_section = join;
            text(join, "Join a match", Typography::LabelLG);
            recompui::Element* join_row = row(join);
            text(join_row, "Join code", Typography::LabelMD);
            join_input = input(join_row, "", 520.0f);
            join_input->add_text_changed_callback([this](const std::string& value) { join_text = value; });
            recompui::Button* paste_button = context.create_element<recompui::Button>(join_row, "Paste", recompui::ButtonStyle::Tertiary);
            paste_button->add_pressed_callback([this]() {
                char* pasted = SDL_GetClipboardText();
                if (pasted != nullptr) {
                    // One line, without the spaces around it (the code reads dashes and spaces anyway).
                    std::string text = pasted;
                    SDL_free(pasted);
                    text = text.substr(0, text.find_first_of("\r\n"));
                    while (!text.empty() && text.back() == ' ') text.pop_back();
                    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
                    join_text = text.substr(0, 64);
                    join_input->set_text(join_text);
                    queue_update();     // which applies the field's new value
                }
            });
            join_button = context.create_element<recompui::Button>(join, "Join", recompui::ButtonStyle::Primary);
            join_button->add_pressed_callback([this]() { on_join(); });
            join_hint = text(join, "", Typography::LabelSM, color::TextDim);

            // The room server: with one, a host gets a short code to give out and a joiner can type one.
            recompui::Element* rooms = column(this, 12.0f);
            rooms_section = rooms;
            text(rooms, "Room server (optional)", Typography::LabelLG);
            recompui::Element* rooms_row = row(rooms);
            text(rooms_row, "Address", Typography::LabelMD);
            room_input = input(rooms_row, room_server_text, 520.0f);
            room_input->add_text_changed_callback([this](const std::string& value) {
                room_server_text = value;
                save_settings();
            });
            text(rooms, "A room server turns the long join code into a short one such as K7Q-F2M, and tries the host's "
                "Tailscale, home-network and public addresses for you. Type the address of one that someone runs "
                "(it starts with https:// or http://), the same on every computer. Leave it empty to use the long "
                "join codes.", Typography::Body, color::TextDim);

            // What this window shows of the split screen.
            recompui::Element* display = column(this, 12.0f);
            display_section = display;
            text(display, "Your screen", Typography::LabelLG);
            recompui::Radio* display_radio = context.create_element<recompui::Radio>(display);
            display_radio->add_option("Split screen");
            display_radio->add_option("My view only");
            display_radio->set_index(conker::netplay_view::is_mine_only() ? 1 : 0);
            display_radio->add_index_changed_callback([](uint32_t index) { conker::netplay_view::set_mine_only(index == 1); });
            text(display, "Every player's view is still drawn on every machine (that's what keeps the games the same): "
                "\"My view only\" shows just yours, across the whole window. When the host's rule for the match is "
                "that everyone sees only their own view (Screens), that's what you get, whatever you pick here.",
                Typography::Body, color::TextDim);

            // The lobby, in place of the settings above while connecting.
            lobby_panel = context.create_element<conker::netplay_lobby::LobbyPanel>(this);

            // How it goes.
            recompui::Element* status = row(this);
            status_label = text(status, "", Typography::LabelLG);
            cancel_button = context.create_element<recompui::Button>(status, "Cancel", recompui::ButtonStyle::Tertiary);
            cancel_button->add_pressed_callback([this]() { on_cancel(); });
            diag_button = context.create_element<recompui::Button>(status, "Copy diagnostics", recompui::ButtonStyle::Tertiary);
            diag_button->add_pressed_callback([this]() { copy_diagnostics(); });
            set_state(State::idle, "");
            show_code();
            refresh_clipboard();
            queue_update();
        }

        // A join code on the clipboard fills the join box when the page opens, so joining is one press.
        void refresh_clipboard() {
            if (state != State::idle) {
                return;
            }
            char* pasted = SDL_GetClipboardText();
            if (pasted == nullptr) {
                return;
            }
            std::string code = pasted;
            SDL_free(pasted);
            code = code.substr(0, code.find_first_of("\r\n"));
            while (!code.empty() && code.back() == ' ') code.pop_back();
            while (!code.empty() && code.front() == ' ') code.erase(code.begin());
            if (code.empty() || code == join_text || !conker::netplay::decode_join_code(code)) {
                return;
            }
            join_text = code;
            join_input->set_text(code);
            join_hint->set_text("Filled in from your clipboard.");
            queue_update();
        }

        bool connecting() const { return state == State::connecting; }
        void cancel_if_connecting() {
            if (state == State::connecting) {
                on_cancel();
            }
        }
    };

    NetplayPage* page = nullptr;
}

void conker::netplay_menu::init(const std::u8string& id, const std::filesystem::path& rom) {
    game_id = id;
    stored_rom = rom;
}

void conker::netplay_menu::open() {
    if (modal == nullptr) {
        // The launcher's option calls this with the launcher's context open, and only one can be: it's
        // closed while the menu's is made.
        recompui::ContextId launcher = recompui::get_current_context();
        const bool launcher_open = launcher.slot_id != recompui::ContextId::null().slot_id;
        if (launcher_open) {
            launcher.close();
        }
        modal = recompui::Modal::create_modal(recompui::ModalType::Fullscreen);
        recompui::ContextId context = modal->modal_root_context;
        context.open();
        page = context.create_element<NetplayPage>(modal->get_body());
        context.close();
        if (launcher_open) {
            launcher.open();
        }
        // Closing it while connecting gives up.
        modal->set_on_close_callback([]() {
            if (page != nullptr) {
                page->cancel_if_connecting();
            }
        });
    }
    if (page != nullptr) {
        page->refresh_clipboard();
    }
    modal->open();
}

void conker::netplay_menu::update() {
    std::string why;
    {
        std::lock_guard lock(over_mutex);
        if (!over_pending) {
            return;
        }
        over_pending = false;
        why = over_text;
    }
    if (!why.empty()) {
        why[0] = (char)std::toupper((unsigned char)why[0]);
    }
    recompui::open_info_prompt("The match is over",
        why + ". The game goes on without anyone's input from here: quit when you like.", "Quit",
        []() { ultramodern::quit(); });
}
