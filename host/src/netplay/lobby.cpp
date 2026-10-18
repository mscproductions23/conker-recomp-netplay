#include "lobby.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <SDL.h>

#include "librecomp/game.hpp"

#include "../conker.hpp"
#include "lockstep.hpp"
#include "settings.hpp"

namespace {
    using recompui::theme::Typography;
    using recompui::theme::color;

    // The N64 pad's buttons, in the order of a player's pad view, with the bits the game reads them by.
    struct ChipDef {
        const char* label;
        uint16_t mask;
    };
    constexpr ChipDef chip_defs[] = {
        { "A", 0x8000 }, { "B", 0x4000 }, { "Z", 0x2000 }, { "St", 0x1000 },
        { "L", 0x0020 }, { "R", 0x0010 },
        { "C^", 0x0008 }, { "Cv", 0x0004 }, { "C<", 0x0002 }, { "C>", 0x0001 },
        { "D^", 0x0800 }, { "Dv", 0x0400 }, { "D<", 0x0200 }, { "D>", 0x0100 },
    };

    // The stick's square, and how far the dot may move from its middle, in dp.
    constexpr float stick_size = 44.0f;
    constexpr float stick_dot = 10.0f;
    constexpr float stick_reach = (stick_size - stick_dot) / 2.0f - 2.0f;

    // How often a player's own pad is sent: when it changes, at most this often; and at least this often.
    constexpr uint32_t pad_min_gap_ms = 66;
    constexpr uint32_t pad_keepalive_ms = 1500;
    constexpr uint32_t notice_ms = 12000;

    // The game makes 30 controller reads a second.
    constexpr float read_ms = 1000.0f / 30.0f;

    recompui::Label* text(recompui::Element* parent, const std::string& words, Typography typography,
        recompui::theme::color text_color = color::Text) {
        recompui::Label* label = recompui::get_current_context().create_element<recompui::Label>(parent, words, typography);
        label->set_color(text_color);
        label->set_flex_shrink(0.0f);
        return label;
    }

    recompui::Element* box(recompui::Element* parent, recompui::FlexDirection direction, float gap) {
        recompui::Element* element = recompui::get_current_context().create_element<recompui::Element>(parent);
        element->set_display(recompui::Display::Flex);
        element->set_flex_direction(direction);
        element->set_align_items(recompui::AlignItems::Center);
        element->set_gap(gap);
        element->set_flex_shrink(0.0f);
        return element;
    }

    recompui::theme::color player_color(int port) {
        return static_cast<recompui::theme::color>((int)color::Player1 + std::clamp(port, 0, 7));
    }

    const char* controller_name(uint8_t controller) {
        switch (controller) {
        case 1: return "Xbox controller";
        case 2: return "PlayStation controller";
        case 3: return "Nintendo controller";
        default: return "No controller";
        }
    }

    // A connection's colour: good, so-so, poor.
    recompui::theme::color ping_color(int ping_ms) {
        return ping_ms <= 60 ? color::Success : (ping_ms <= 120 ? color::Warning : color::Danger);
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

        // Something that happened: a joiner was refused...
        notice_label = text(this, "", Typography::LabelMD, color::Warning);
        notice_label->set_display(recompui::Display::None);

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

        // Who is in: one row for each of the game's four ports, each with a live pad under it.
        recompui::Element* list = box(this, recompui::FlexDirection::Column, 10.0f);
        list->set_width(100.0f, recompui::Unit::Percent);
        list->set_align_items(recompui::AlignItems::Stretch);
        rows.resize(netplay::max_players);
        for (int i = 0; i < netplay::max_players; i++) {
            Row& row = rows[i];
            row.box = box(list, recompui::FlexDirection::Column, 8.0f);
            row.box->set_align_items(recompui::AlignItems::Stretch);
            row.box->set_padding(14.0f);
            row.box->set_border_radius(recompui::theme::border::radius_md);
            row.box->set_background_color(color::ElevatedSoft);

            recompui::Element* line = box(row.box, recompui::FlexDirection::Row, 18.0f);
            row.bar = context.create_element<recompui::Element>(line);
            row.bar->set_width(6.0f);
            row.bar->set_min_width(6.0f);
            row.bar->set_height(34.0f);
            row.bar->set_border_radius(3.0f);
            row.bar->set_background_color(player_color(i));
            row.slot = text(line, "Player " + std::to_string(i + 1), Typography::LabelMD, color::TextDim);
            row.slot->set_min_width(90.0f);
            row.name = text(line, "", Typography::LabelLG);
            row.name->set_min_width(260.0f);
            row.role = text(line, "", Typography::LabelSM, color::Secondary);
            row.role->set_min_width(90.0f);
            row.device = text(line, "", Typography::LabelSM, color::TextDim);
            row.device->set_min_width(190.0f);
            row.ping = text(line, "", Typography::LabelSM, color::TextDim);
            row.ping->set_min_width(130.0f);
            row.status = text(line, "", Typography::LabelMD, color::TextDim);

            // The pad: a chip for each button and the stick as a dot, lit as the player presses them.
            PadView& pad = row.pad;
            pad.box = box(row.box, recompui::FlexDirection::Row, 6.0f);
            pad.box->set_display(recompui::Display::None);
            for (int k = 0; k < pad_chips; k++) {
                pad.chip_boxes[k] = box(pad.box, recompui::FlexDirection::Column, 0.0f);
                pad.chip_boxes[k]->set_width(34.0f);
                pad.chip_boxes[k]->set_min_width(34.0f);
                pad.chip_boxes[k]->set_height(26.0f);
                pad.chip_boxes[k]->set_justify_content(recompui::JustifyContent::Center);
                pad.chip_boxes[k]->set_border_radius(recompui::theme::border::radius_sm);
                pad.chip_boxes[k]->set_background_color(color::ElevatedSoft);
                pad.chip_labels[k] = text(pad.chip_boxes[k], chip_defs[k].label, Typography::LabelSM, color::TextDim);
            }
            recompui::Element* stick = context.create_element<recompui::Element>(pad.box);
            stick->set_position(recompui::Position::Relative);
            stick->set_width(stick_size);
            stick->set_min_width(stick_size);
            stick->set_height(stick_size);
            stick->set_margin_left(12.0f);
            stick->set_border_radius(8.0f);
            stick->set_border_width(1.5f);
            stick->set_border_color(color::Border);
            pad.dot = context.create_element<recompui::Element>(stick);
            pad.dot->set_position(recompui::Position::Absolute);
            pad.dot->set_width(stick_dot);
            pad.dot->set_height(stick_dot);
            pad.dot->set_border_radius(stick_dot / 2.0f);
            pad.dot->set_background_color(player_color(i));
            pad.dot->set_left((stick_size - stick_dot) / 2.0f - 1.5f);
            pad.dot->set_top((stick_size - stick_dot) / 2.0f - 1.5f);
        }

        // The input delay: the host's to choose (by default from everyone's ping).
        recompui::Element* delay_row = box(this, recompui::FlexDirection::Row, 16.0f);
        text(delay_row, "Input delay", Typography::LabelMD);
        delay_radio = context.create_element<recompui::Radio>(delay_row);
        delay_radio->add_option("Auto");
        for (int reads = 2; reads <= 6; reads++) {
            delay_radio->add_option(std::to_string(reads));
        }
        delay_radio->set_index(0);
        delay_radio->add_index_changed_callback([this](uint32_t index) { on_delay_chosen(index); });
        delay_label = text(delay_row, "", Typography::LabelSM, color::TextDim);

        start_button = context.create_element<recompui::Button>(this, "Start game", recompui::ButtonStyle::Primary);
        start_button->add_pressed_callback([this]() { on_start_pressed(); });
        hint_label = text(this, "", Typography::Body, color::TextDim);
    }

    void LobbyPanel::open(bool as_host) {
        hosting = as_host;
        ready = false;
        own_name = conker::netplay_helpers::load_settings(recomp::get_config_path()).name;
        name_input->set_text(own_name);
        ready_button->set_display(hosting ? recompui::Display::None : recompui::Display::Flex);
        ready_button->set_text("I'm ready");
        start_button->set_display(hosting ? recompui::Display::Flex : recompui::Display::None);
        delay_radio->set_display(hosting ? recompui::Display::Flex : recompui::Display::None);
        delay_radio->set_index(0);
        conker::lockstep::set_delay_override(0);
        notice_label->set_display(recompui::Display::None);
        notice_until = 0;
        hint_label->set_text(hosting
            ? "Press Start game when everyone is in and ready. You're ready when you press it. The input delay "
              "is how many controller reads ahead everyone's presses are sent; a longer one hides a slow "
              "connection but makes the game feel less immediate."
            : "The host starts the game once everyone is in and ready. Press buttons on your controller: your "
              "pad above lights up, and the others see it too.");
        set_display(recompui::Display::Flex);
        last_sent = 0;
        last_pad_ticks = 0;
        last_pad_sent = netplay::Pad{};
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

    void LobbyPanel::show_notice(const std::string& text_to_show) {
        notice_label->set_text(text_to_show);
        notice_label->set_display(recompui::Display::Block);
        notice_until = SDL_GetTicks() + notice_ms;
    }

    uint8_t LobbyPanel::own_controller() const {
        _SDL_GameController* controller = conker::frontend::primary_controller();
        if (controller == nullptr) {
            return 0;
        }
        switch (conker::controller_glyphs::info_of(controller).family) {
        case conker::controller_glyphs::Family::PlayStation: return 2;
        case conker::controller_glyphs::Family::Nintendo: return 3;
        default: return 1;
        }
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
        sent_controller = own_controller();
        // The host is ready by pressing Start game.
        conker::lockstep::set_lobby(own_name, hosting || ready, sent_controller);
    }

    void LobbyPanel::update_own_pad() {
        uint16_t buttons = 0;
        float x = 0.0f;
        float y = 0.0f;
        if (!conker::frontend::read_local_pad(&buttons, &x, &y)) {
            return;
        }
        netplay::Pad pad;
        pad.buttons = buttons & (uint16_t)~netplay::reserved_buttons;
        pad.stick_x = (int8_t)std::lround(std::clamp(x, -1.0f, 1.0f) * 80.0f);
        pad.stick_y = (int8_t)std::lround(std::clamp(y, -1.0f, 1.0f) * 80.0f);
        const uint32_t now = SDL_GetTicks();
        const bool changed = !(pad == last_pad_sent);
        if ((changed && now - last_pad_ticks >= pad_min_gap_ms) || now - last_pad_ticks >= pad_keepalive_ms) {
            conker::lockstep::send_lobby_pad(pad);
            last_pad_sent = pad;
            last_pad_ticks = now;
        }
    }

    void LobbyPanel::show_pad(PadView& view, const netplay::Pad& pad) {
        for (int k = 0; k < pad_chips; k++) {
            const bool down = (pad.buttons & chip_defs[k].mask) != 0;
            view.chip_boxes[k]->set_background_color(down ? color::Primary : color::ElevatedSoft);
            view.chip_labels[k]->set_color(down ? color::White : color::TextDim);
        }
        const float x = std::clamp(pad.stick_x / 80.0f, -1.0f, 1.0f);
        const float y = std::clamp(pad.stick_y / 80.0f, -1.0f, 1.0f);
        const float middle = (stick_size - stick_dot) / 2.0f - 1.5f;
        view.dot->set_left(middle + x * stick_reach);
        view.dot->set_top(middle - y * stick_reach);     // the stick's up is positive
    }

    void LobbyPanel::on_name_changed(const std::string& value) {
        own_name = value.substr(0, netplay::name_size);
        const std::filesystem::path folder = recomp::get_config_path();
        conker::netplay_helpers::Settings settings = conker::netplay_helpers::load_settings(folder);
        settings.name = own_name;
        conker::netplay_helpers::save_settings(folder, settings);
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

    void LobbyPanel::on_delay_chosen(uint32_t index) {
        // 0 is Auto (from everyone's ping); the others are 2 to 6 reads.
        conker::lockstep::set_delay_override(index == 0 ? 0 : (int)index + 1);
    }

    void LobbyPanel::refresh() {
        // A controller plugged in or out is a change to send.
        if (own_controller() != sent_controller) {
            dirty = true;
        }
        flush_own();
        update_own_pad();

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
            if (!entry.present) {
                row.name->set_text("Waiting for a player...");
                row.name->set_color(color::TextDim);
                row.role->set_text("");
                row.device->set_text("");
                row.ping->set_text("");
                row.status->set_text("");
                row.pad.box->set_display(recompui::Display::None);
                continue;
            }
            in++;
            // A host is ready by starting the game, so it counts as ready.
            const bool is_ready = entry.ready || i == 0;
            ready_count += is_ready ? 1 : 0;
            row.name->set_text(entry.name);
            row.name->set_color(color::Text);
            const bool own_row = i == own_port;
            row.role->set_text(i == 0 ? (own_row ? "Host, you" : "Host") : (own_row ? "You" : ""));
            if (entry.controller == 0) {
                row.device->set_text(own_row ? "No controller (keyboard only)" : "No controller");
                row.device->set_color(own_row ? color::Warning : color::TextDim);
            }
            else {
                row.device->set_text(controller_name(entry.controller));
                row.device->set_color(color::TextDim);
            }
            if (i == 0) {
                row.ping->set_text("");
            }
            else if (entry.ping_ms == 0) {
                row.ping->set_text("measuring...");
                row.ping->set_color(color::TextDim);
            }
            else {
                std::string ping = std::to_string(entry.ping_ms) + " ms";
                if (entry.jitter_ms >= 5) {
                    ping += " +/-" + std::to_string(entry.jitter_ms);
                }
                row.ping->set_text(ping);
                row.ping->set_color(ping_color(entry.ping_ms));
            }
            row.status->set_text(is_ready ? "Ready" : "Not ready");
            row.status->set_color(is_ready ? color::Success : color::TextDim);
            if (entry.pad_seen) {
                row.pad.box->set_display(recompui::Display::Flex);
                show_pad(row.pad, entry.pad);
            }
            else {
                row.pad.box->set_display(recompui::Display::None);
            }
        }
        count_label->set_text(std::to_string(in) + " of " + std::to_string((int)roster.size()) + " players in, " +
            std::to_string(ready_count) + " ready.");
        if (hosting) {
            start_button->set_enabled(conker::lockstep::lobby_can_start());
            const int reads = conker::lockstep::chosen_delay();
            delay_label->set_text(std::to_string(reads) + " reads, about " + std::to_string((int)std::lround(reads * read_ms)) + " ms");
        }
        else {
            delay_label->set_text("the host chooses it");
        }
        if (notice_until != 0 && SDL_GetTicks() > notice_until) {
            notice_label->set_display(recompui::Display::None);
            notice_until = 0;
        }
    }
}
