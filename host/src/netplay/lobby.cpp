#include "lobby.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>

#include <SDL.h>

#include "librecomp/game.hpp"
#include "lockstep.hpp"
#include "net/packets.hpp"

namespace {
    using recompui::theme::Typography;
    using recompui::theme::color;

    // Where a player's name is kept between runs.
    std::filesystem::path name_file() {
        return recomp::get_config_path() / "netplay_name.txt";
    }

    std::string load_name() {
        std::ifstream file(name_file());
        std::string name;
        std::getline(file, name);
        return name.substr(0, conker::netplay::name_size);
    }

    void save_name(const std::string& name) {
        std::ofstream file(name_file(), std::ios::trunc);
        file << name << '\n';
    }

    recompui::Label* text(recompui::Element* parent, const std::string& words, Typography typography,
        recompui::theme::color text_color = color::Text) {
        recompui::Label* label = recompui::get_current_context().create_element<recompui::Label>(parent, words, typography);
        label->set_color(text_color);
        return label;
    }

    recompui::Element* box(recompui::Element* parent, recompui::FlexDirection direction, float gap) {
        recompui::Element* element = recompui::get_current_context().create_element<recompui::Element>(parent);
        element->set_display(recompui::Display::Flex);
        element->set_flex_direction(direction);
        element->set_align_items(recompui::AlignItems::Center);
        element->set_gap(gap);
        return element;
    }
}

namespace conker::netplay_lobby {
    LobbyPanel::LobbyPanel(recompui::ResourceId rid, recompui::Element* parent) : Element(rid, parent) {
        recompui::ContextId context = recompui::get_current_context();
        set_display(recompui::Display::None);
        set_flex_direction(recompui::FlexDirection::Column);
        set_gap(18.0f);
        set_width(100.0f, recompui::Unit::Percent);

        text(this, "Lobby", Typography::LabelLG);
        count_label = text(this, "", Typography::LabelMD, color::TextDim);

        // The host's join code, to send the others.
        recompui::Element* code_row = box(this, recompui::FlexDirection::Row, 16.0f);
        code_label = text(code_row, "", Typography::LabelMD);
        code_copy = context.create_element<recompui::Button>(code_row, "Copy", recompui::ButtonStyle::Tertiary);
        code_copy->add_pressed_callback([this]() {
            if (!join_code.empty() && SDL_SetClipboardText(join_code.c_str()) == 0) {
                code_copy->set_text("Copied");
            }
        });
        code_copy->set_display(recompui::Display::None);

        // The player's own name.
        recompui::Element* name_row = box(this, recompui::FlexDirection::Row, 16.0f);
        text(name_row, "Your name", Typography::LabelMD);
        name_input = context.create_element<recompui::TextInput>(name_row);
        name_input->set_width(260.0f);
        name_input->set_min_width(260.0f);
        name_input->set_flex_shrink(0.0f);
        name_input->add_text_changed_callback([this](const std::string& value) { on_name_changed(value); });
        ready_button = context.create_element<recompui::Button>(name_row, "I'm ready", recompui::ButtonStyle::Primary);
        ready_button->add_pressed_callback([this]() { on_ready_pressed(); });

        // Who is in: one row for each of the game's four ports.
        recompui::Element* list = box(this, recompui::FlexDirection::Column, 10.0f);
        list->set_width(100.0f, recompui::Unit::Percent);
        list->set_align_items(recompui::AlignItems::Stretch);
        rows.resize(conker::netplay::max_players);
        for (int i = 0; i < conker::netplay::max_players; i++) {
            Row& row = rows[i];
            row.box = box(list, recompui::FlexDirection::Row, 20.0f);
            row.box->set_padding(14.0f);
            row.box->set_border_radius(recompui::theme::border::radius_md);
            row.box->set_background_color(color::ElevatedSoft);
            row.slot = text(row.box, "Player " + std::to_string(i + 1), Typography::LabelMD, color::TextDim);
            row.slot->set_min_width(110.0f);
            row.name = text(row.box, "", Typography::LabelLG);
            row.name->set_min_width(320.0f);
            row.role = text(row.box, "", Typography::LabelSM, color::Secondary);
            row.role->set_min_width(80.0f);
            row.status = text(row.box, "", Typography::LabelMD, color::TextDim);
        }

        start_button = context.create_element<recompui::Button>(this, "Start game", recompui::ButtonStyle::Primary);
        start_button->add_pressed_callback([this]() { on_start_pressed(); });
        hint_label = text(this, "", Typography::Body, color::TextDim);
    }

    void LobbyPanel::open(bool as_host) {
        hosting = as_host;
        ready = false;
        own_name = load_name();
        name_input->set_text(own_name);
        ready_button->set_display(hosting ? recompui::Display::None : recompui::Display::Flex);
        ready_button->set_text("I'm ready");
        start_button->set_display(hosting ? recompui::Display::Flex : recompui::Display::None);
        hint_label->set_text(hosting
            ? "Press Start game when everyone is in and ready. You're ready when you press it."
            : "The host starts the game once everyone is in and ready.");
        set_display(recompui::Display::Flex);
        last_sent = 0;
        send_own();
        refresh();
    }

    void LobbyPanel::close() {
        set_display(recompui::Display::None);
    }

    void LobbyPanel::set_join_code(const std::string& code) {
        if (code != join_code) {
            code_copy->set_text("Copy");
        }
        join_code = code;
        code_label->set_text(code.empty() ? std::string() : "Join code: " + code);
        code_copy->set_display(code.empty() ? recompui::Display::None : recompui::Display::Flex);
    }

    void LobbyPanel::send_own() {
        dirty = true;
        flush_own();
    }

    void LobbyPanel::flush_own() {
        // At most about three sends a second; a change that has to wait goes out at a later refresh.
        constexpr uint32_t min_gap_ms = 350;
        const uint32_t now = SDL_GetTicks();
        if (!dirty || (last_sent != 0 && now - last_sent < min_gap_ms)) {
            return;
        }
        dirty = false;
        last_sent = now;
        // The host is ready by pressing Start game.
        conker::lockstep::set_lobby(own_name, hosting || ready);
    }

    void LobbyPanel::on_name_changed(const std::string& value) {
        own_name = value.substr(0, conker::netplay::name_size);
        save_name(own_name);
        send_own();
    }

    void LobbyPanel::on_ready_pressed() {
        ready = !ready;
        ready_button->set_text(ready ? "Not ready" : "I'm ready");
        send_own();
        refresh();
    }

    void LobbyPanel::on_start_pressed() {
        if (!conker::lockstep::lobby_start()) {
            hint_label->set_text("Not everyone is in and ready yet.");
        }
    }

    void LobbyPanel::refresh() {
        flush_own();
        const std::vector<conker::lockstep::LobbyEntry> roster = conker::lockstep::lobby();
        const int own_port = conker::lockstep::port();
        int in = 0;
        int ready_count = 0;
        for (int i = 0; i < (int)rows.size(); i++) {
            Row& row = rows[i];
            const bool slot_used = i < (int)roster.size();
            row.box->set_display(slot_used ? recompui::Display::Flex : recompui::Display::None);
            if (!slot_used) {
                continue;
            }
            const conker::lockstep::LobbyEntry& entry = roster[i];
            if (entry.present) {
                in++;
                // A host is ready by starting the game, so it counts as ready.
                const bool is_ready = entry.ready || i == 0;
                ready_count += is_ready ? 1 : 0;
                row.name->set_text(entry.name);
                row.name->set_color(color::Text);
                row.role->set_text(i == 0 ? "Host" : (i == own_port ? "You" : ""));
                if (i == own_port && i != 0) {
                    row.role->set_text("You");
                }
                else if (i == 0 && own_port == 0) {
                    row.role->set_text("Host, you");
                }
                row.status->set_text(is_ready ? "Ready" : "Not ready");
                row.status->set_color(is_ready ? color::Success : color::TextDim);
            }
            else {
                row.name->set_text("Waiting for a player...");
                row.name->set_color(color::TextDim);
                row.role->set_text("");
                row.status->set_text("");
            }
        }
        count_label->set_text(std::to_string(in) + " of " + std::to_string((int)roster.size()) + " players in, " +
            std::to_string(ready_count) + " ready.");
        if (hosting) {
            start_button->set_enabled(conker::lockstep::lobby_can_start());
        }
    }
}
