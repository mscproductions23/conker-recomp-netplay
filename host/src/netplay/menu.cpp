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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include <algorithm>
#include <cctype>
#include <cmath>
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
#include "controller_view.hpp"
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
        recompui::Label* hero_subtitle = nullptr;
        recompui::Element* idle_area = nullptr;         // everything but the lobby and the status line
        recompui::Element* options_box = nullptr;
        recompui::Button* more_button = nullptr;
        bool options_open = false;
        bool visible = false;                 // the page is on screen
        conker::netplay_view_pad::ControllerView* own_pad = nullptr;   // the player's own controller, live
        recompui::Label* own_pad_label = nullptr;
        std::string own_pad_name;
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

        // A panel: a rounded, slightly lifted box with a stripe of colour along its top edge.
        recompui::Element* card(recompui::Element* parent, const recompui::Color& stripe) {
            recompui::Element* box = recompui::get_current_context().create_element<recompui::Element>(parent);
            box->set_display(recompui::Display::Flex);
            box->set_flex_direction(recompui::FlexDirection::Column);
            box->set_align_items(recompui::AlignItems::FlexStart);
            box->set_gap(16.0f);
            box->set_width(100.0f, recompui::Unit::Percent);
            box->set_padding(26.0f);
            box->set_border_radius(20.0f);
            box->set_background_color(recompui::theme::color::Background2);
            box->set_border_width(2.0f);
            box->set_border_color(recompui::Color{255, 255, 255, 22});
            box->set_border_top_width(5.0f);
            box->set_border_top_color(stripe);
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
            hero_subtitle->set_display(lobby ? recompui::Display::None : recompui::Display::Block);
            if (lobby) {
                // Focus goes to Copy diagnostics, which a held key or pad button can press without harm (Cancel
                // would leave the lobby).
                cancel_button->set_display(recompui::Display::Flex);
                diag_button->set_display(recompui::Display::Flex);
                diag_button->focus();
            }
            else {
                idle_area->set_display(settings);
                host_button->set_enabled(true);
                join_button->set_enabled(true);
                host_button->focus();
                cancel_button->set_display(recompui::Display::None);
                diag_button->set_display(recompui::Display::None);
            }
            if (lobby) {
                host_button->set_enabled(false);
                join_button->set_enabled(false);
                idle_area->set_display(settings);
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
            if (visible && state == State::idle && own_pad != nullptr) {
                show_own_controller();
                queue_update();     // again next frame while the page is on screen
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

            using recompui::Color;
            const Color orange{255, 150, 64, 255};

            // What this player picked last time.
            const conker::netplay_helpers::Settings saved = conker::netplay_helpers::load_settings(recomp::get_config_path());
            players = saved.players;
            port_text = std::to_string(saved.port);
            address_text = saved.address;
            own_view_for_everyone = saved.own_view_for_everyone;
            room_server_text = saved.room_server;

            // The header: what this page is for, and the build (everyone in a match needs the same one).
            recompui::Element* hero = row(this);
            hero->set_width(100.0f, recompui::Unit::Percent);
            hero->set_justify_content(recompui::JustifyContent::SpaceBetween);
            recompui::Element* title_box = column(hero, 6.0f);
            title_box->set_width_auto();
            recompui::Label* title = text(title_box, "Play online", Typography::Header1);
            title->set_color(orange);
            hero_subtitle = text(title_box, "Host a room, send the code, play. Everyone needs this same build and their own US ROM.",
                Typography::Body, color::TextDim);
            recompui::Element* build_tag = row(hero);
            build_tag->set_padding_left(16.0f);
            build_tag->set_padding_right(16.0f);
            build_tag->set_padding_top(8.0f);
            build_tag->set_padding_bottom(8.0f);
            build_tag->set_border_radius(999.0f);
            build_tag->set_background_color(Color{255, 255, 255, 22});
            text(build_tag, "BUILD " + short_build(), Typography::LabelSM, color::TextDim)->set_letter_spacing(1.5f);

            // Everything that isn't the lobby, hidden while connecting.
            recompui::Element* idle = column(this, 24.0f);
            idle_area = idle;
            recompui::Element* columns = row(idle);
            columns->set_width(100.0f, recompui::Unit::Percent);
            columns->set_align_items(recompui::AlignItems::FlexStart);
            columns->set_gap(28.0f);
            recompui::Element* left = column(columns, 22.0f);
            left->set_width(1000.0f);
            left->set_min_width(1000.0f);
            recompui::Element* right = column(columns, 22.0f);
            right->set_width(620.0f);
            right->set_min_width(620.0f);

            // Hosting.
            recompui::Element* host = card(left, orange);
            text(host, "Host a room", Typography::Header3);
            text(host, "Friends join with a code. You pick the game in the lobby.", Typography::Body, color::TextDim);
            recompui::Element* count_row = row(host);
            text(count_row, "Players", Typography::LabelMD);
            recompui::Radio* count_radio = context.create_element<recompui::Radio>(count_row);
            for (int n = 2; n <= conker::netplay::max_players; n++) {
                count_radio->add_option(std::to_string(n));
            }
            count_radio->set_index((uint32_t)(players - 2));
            count_radio->add_index_changed_callback([this](uint32_t index) {
                players = 2 + (int)index;
                save_settings();
            });
            // Where friends reach you: this computer's own addresses (Tailscale first: its friends need no router
            // setup), or one typed in.
            recompui::Element* address_row = row(host);
            text(address_row, "Reach me at", Typography::LabelMD);
            address_input = input(address_row, address_text, 280.0f);
            address_input->add_text_changed_callback([this](const std::string& value) {
                address_text = value;
                save_settings();
                show_code();
            });
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
                address_radio->add_option("Other");
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
            host_button = context.create_element<recompui::Button>(host, "Host a room", recompui::ButtonStyle::Primary);
            host_button->add_pressed_callback([this]() { on_host(); });
            // The code row is kept (show_code fills it) but the lobby is where the code is shown.
            recompui::Element* code_row = row(host);
            code_row->set_display(recompui::Display::None);
            code_label = text(code_row, "", Typography::LabelMD);
            copy_button = context.create_element<recompui::Button>(code_row, "Copy", recompui::ButtonStyle::Tertiary);
            copy_button->add_pressed_callback([this]() {
                if (!shown_code.empty() && SDL_SetClipboardText(shown_code.c_str()) == 0) {
                    copy_button->set_text("Copied");
                    queue_update();
                }
            });

            // Joining.
            recompui::Element* join = card(left, Color{64, 176, 255, 255});
            text(join, "Join a room", Typography::Header3);
            text(join, "Paste the code the host sent you: the long join code, or a short room code.", Typography::Body, color::TextDim);
            recompui::Element* join_row = row(join);
            join_input = input(join_row, "", 560.0f);
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
            join_button = context.create_element<recompui::Button>(join_row, "Join", recompui::ButtonStyle::Primary);
            join_button->add_pressed_callback([this]() { on_join(); });
            join_hint = text(join, "", Typography::LabelSM, color::TextDim);

            // The player's own controller, live: press a button and it lights, so a pad that doesn't answer shows
            // before anyone is waiting.
            recompui::Element* mine = card(right, Color{96, 214, 128, 255});
            text(mine, "Your controller", Typography::Header3);
            own_pad_label = text(mine, "", Typography::Body, color::TextDim);
            own_pad = context.create_element<conker::netplay_view_pad::ControllerView>(mine, 460.0f, Color{96, 214, 128, 255});
            text(mine, "Press buttons to test them. Players use their first controller.", Typography::LabelSM, color::TextDim);

            // The rest of the settings, out of the way.
            recompui::Element* more_row = row(idle);
            more_button = context.create_element<recompui::Button>(more_row, "More options", recompui::ButtonStyle::Tertiary);
            more_button->add_pressed_callback([this]() {
                options_open = !options_open;
                options_box->set_display(options_open ? recompui::Display::Flex : recompui::Display::None);
                more_button->set_text(options_open ? "Fewer options" : "More options");
            });
            options_box = card(idle, Color{255, 255, 255, 60});
            options_box->set_display(recompui::Display::None);
            text(options_box, "More options", Typography::Header3);
            recompui::Element* port_row = row(options_box);
            text(port_row, "UDP port", Typography::LabelMD);
            port_input = input(port_row, port_text, 120.0f);
            port_input->add_text_changed_callback([this](const std::string& value) {
                port_text = value;
                save_settings();
            });
            text(port_row, "The port friends connect to; forward it (UDP) on your router if you can't use Tailscale.",
                Typography::LabelSM, color::TextDim);
            recompui::Element* screens_row = row(options_box);
            text(screens_row, "Screens", Typography::LabelMD);
            recompui::Radio* screens_radio = context.create_element<recompui::Radio>(screens_row);
            screens_radio->add_option("Everyone sees only their own view");
            screens_radio->add_option("Players choose");
            screens_radio->set_index(own_view_for_everyone ? 0 : 1);
            screens_radio->add_index_changed_callback([this](uint32_t index) {
                own_view_for_everyone = index == 0;
                save_settings();
            });
            recompui::Element* display_row = row(options_box);
            text(display_row, "Your screen", Typography::LabelMD);
            recompui::Radio* display_radio = context.create_element<recompui::Radio>(display_row);
            display_radio->add_option("Split screen");
            display_radio->add_option("My view only");
            display_radio->set_index(conker::netplay_view::is_mine_only() ? 1 : 0);
            display_radio->add_index_changed_callback([](uint32_t index) { conker::netplay_view::set_mine_only(index == 1); });
            recompui::Element* server_row = row(options_box);
            text(server_row, "Room server", Typography::LabelMD);
            room_input = input(server_row, room_server_text, 520.0f);
            room_input->add_text_changed_callback([this](const std::string& value) {
                room_server_text = value;
                save_settings();
            });
            text(options_box, "A room server turns the long join code into a short one such as K7Q-F2M, and tries the host's "
                "Tailscale, home-network and public addresses for you. Type the address of one that someone runs (it starts "
                "with https:// or http://), the same on every computer; empty means the long join codes only.",
                Typography::LabelSM, color::TextDim);

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

        // The player's own controller, read as the game would: the same drawing the others see in the lobby.
        void show_own_controller() {
            uint16_t buttons = 0;
            float x = 0.0f;
            float y = 0.0f;
            if (!conker::frontend::read_local_pad(&buttons, &x, &y)) {
                return;
            }
            conker::netplay::Pad pad;
            pad.buttons = buttons & (uint16_t)~conker::netplay::reserved_buttons;
            pad.stick_x = (int8_t)std::lround(std::clamp(x, -1.0f, 1.0f) * 80.0f);
            pad.stick_y = (int8_t)std::lround(std::clamp(y, -1.0f, 1.0f) * 80.0f);
            own_pad->show(pad);
            _SDL_GameController* controller = conker::frontend::primary_controller();
            std::string name = "No controller found: the keyboard is used (Space is A, Return is Start, WASD is the stick).";
            if (controller != nullptr) {
                const char* sdl_name = SDL_GameControllerName(controller);
                name = std::string(sdl_name != nullptr ? sdl_name : "Controller") + " (" +
                    conker::controller_glyphs::info_of(controller).name + ")";
            }
            if (name != own_pad_name) {
                own_pad_name = name;
                own_pad_label->set_text(name);
            }
        }

        // The page is on screen: its live controller is drawn only then, as reading the pad polls the inputs.
        void set_visible(bool shown) {
            visible = shown;
            if (shown) {
                queue_update();
            }
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
                page->set_visible(false);
                page->cancel_if_connecting();
            }
        });
    }
    if (page != nullptr) {
        page->refresh_clipboard();
        page->set_visible(true);
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
