#include "lobby.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include <SDL.h>

#include "librecomp/game.hpp"

#include "../conker.hpp"
#include "controller_view.hpp"
#include "lockstep.hpp"
#include "settings.hpp"

namespace {
    using recompui::theme::Typography;
    using recompui::theme::color;
    using recompui::Color;

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

    // The players' colours as plain colours, for the drawings.
    recompui::Color player_rgb(int port) {
        static const recompui::Color colors[] = {
            { 64, 176, 255, 255 }, { 255, 112, 56, 255 }, { 96, 214, 128, 255 }, { 232, 196, 64, 255 },
        };
        return colors[std::clamp(port, 0, 3)];
    }

    // A small rounded tag with a label in it: a slot, a role, a state. Its label is returned so the text can change.
    conker::netplay_lobby::Pill pill(recompui::Element* parent, const std::string& words, const recompui::Color& fill, const recompui::Color& ink) {
        recompui::Element* tag = recompui::get_current_context().create_element<recompui::Element>(parent);
        tag->set_display(recompui::Display::Flex);
        tag->set_align_items(recompui::AlignItems::Center);
        tag->set_padding_left(14.0f);
        tag->set_padding_right(14.0f);
        tag->set_padding_top(5.0f);
        tag->set_padding_bottom(5.0f);
        tag->set_border_radius(999.0f);
        tag->set_background_color(fill);
        tag->set_flex_shrink(0.0f);
        recompui::Label* label = recompui::get_current_context().create_element<recompui::Label>(tag, words, Typography::LabelSM);
        label->set_color(ink);
        label->set_letter_spacing(1.5f);
        label->set_white_space(recompui::WhiteSpace::Nowrap);
        return { tag, label };
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
        set_flex_shrink(0.0f);
        set_gap(18.0f);
        set_width(100.0f, recompui::Unit::Percent);

        recompui::Element* heading = box(this, recompui::FlexDirection::Row, 18.0f);
        heading->set_width(100.0f, recompui::Unit::Percent);
        heading->set_justify_content(recompui::JustifyContent::SpaceBetween);
        text(heading, "Lobby", Typography::Header2)->set_color(Color{255, 150, 64, 255});
        count_label = text(heading, "", Typography::LabelMD, color::TextDim);

        // The code to send the others (the host's): the short room code when there is one, big, and the long join
        // code under it; and how reaching the host from outside is going (the router's port, the room).
        share_card = box(this, recompui::FlexDirection::Row, 28.0f);
        share_card->set_width(100.0f, recompui::Unit::Percent);
        share_card->set_justify_content(recompui::JustifyContent::SpaceBetween);
        share_card->set_padding(24.0f);
        share_card->set_border_radius(20.0f);
        share_card->set_background_color(color::Background2);
        share_card->set_border_width(2.0f);
        share_card->set_border_color(Color{255, 255, 255, 22});
        share_card->set_border_top_width(5.0f);
        share_card->set_border_top_color(Color{255, 150, 64, 255});
        recompui::Element* share_text = box(share_card, recompui::FlexDirection::Column, 8.0f);
        share_text->set_align_items(recompui::AlignItems::FlexStart);
        code_caption = text(share_text, "", Typography::LabelSM, color::TextDim);
        code_caption->set_letter_spacing(2.0f);
        code_big = text(share_text, "", Typography::Header1);
        code_big->set_letter_spacing(6.0f);
        code_small = text(share_text, "", Typography::LabelSM, color::TextDim);
        room_status = text(share_text, "", Typography::LabelSM, color::TextDim);
        recompui::Element* share_buttons = box(share_card, recompui::FlexDirection::Column, 10.0f);
        share_buttons->set_align_items(recompui::AlignItems::Stretch);
        code_copy = context.create_element<recompui::Button>(share_buttons, "Copy code", recompui::ButtonStyle::Primary);
        code_copy->add_pressed_callback([this]() {
            const std::string& big = room_code.empty() ? join_code : room_code;
            if (!big.empty() && SDL_SetClipboardText(big.c_str()) == 0) {
                code_copy->set_text("Copied");
            }
        });
        long_copy = context.create_element<recompui::Button>(share_buttons, "Copy long code", recompui::ButtonStyle::Tertiary);
        long_copy->add_pressed_callback([this]() {
            if (!join_code.empty() && SDL_SetClipboardText(join_code.c_str()) == 0) {
                long_copy->set_text("Copied");
            }
        });
        share_card->set_display(recompui::Display::None);

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

        // Who is in: a card for each of the game's four ports, with the player's controller drawn live beside
        // their name, so what they press is what lights up.
        recompui::Element* list = box(this, recompui::FlexDirection::Row, 18.0f);
        list->set_width(100.0f, recompui::Unit::Percent);
        list->set_align_items(recompui::AlignItems::Stretch);
        list->set_flex_wrap(recompui::FlexWrap::Wrap);
        rows.resize(netplay::max_players);
        for (int i = 0; i < netplay::max_players; i++) {
            Row& row = rows[i];
            row.box = box(list, recompui::FlexDirection::Row, 22.0f);
            row.box->set_width(48.5f, recompui::Unit::Percent);
            row.box->set_padding(16.0f);
            row.box->set_border_radius(18.0f);
            row.box->set_background_color(color::Background2);
            row.box->set_border_width(2.0f);
            row.box->set_border_color(color::BorderSoft);

            const Color accent = player_rgb(i);
            row.view = context.create_element<conker::netplay_view_pad::ControllerView>(row.box, 240.0f, accent);

            recompui::Element* info = box(row.box, recompui::FlexDirection::Column, 10.0f);
            info->set_align_items(recompui::AlignItems::FlexStart);
            info->set_flex_grow(1.0f);
            recompui::Element* head = box(info, recompui::FlexDirection::Row, 14.0f);
            row.slot = pill(head, "P" + std::to_string(i + 1), accent, Color{16, 12, 20, 255});
            row.name = text(head, "", Typography::Header3);
            row.role = pill(head, "", Color{255, 255, 255, 30}, Color{246, 241, 235, 255});
            recompui::Element* details = box(info, recompui::FlexDirection::Row, 22.0f);
            row.device = text(details, "", Typography::LabelMD, color::TextDim);
            row.ping = text(details, "", Typography::LabelMD, color::TextDim);
            row.status = pill(info, "", Color{255, 255, 255, 30}, Color{246, 241, 235, 255});
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

        // The game to play: the host's to choose. A mode starts the match in it by itself (the lobby presses the
        // menus for everyone, on every machine alike); "In game" leaves the menus to the players.
        mode_box = box(this, recompui::FlexDirection::Column, 6.0f);
        mode_box->set_align_items(recompui::AlignItems::FlexStart);
        recompui::Element* mode_row = box(mode_box, recompui::FlexDirection::Row, 16.0f);
        text(mode_row, "Game", Typography::LabelMD);
        mode_radio = context.create_element<recompui::Radio>(mode_row);
        mode_radio->add_option("Choose in game");
        for (const char* name : { "Deathmatch", "War", "Tank", "Race", "Beach", "Raptor", "Heist" }) {
            mode_radio->add_option(name);
        }
        mode_radio->set_index(0);
        mode_radio->add_index_changed_callback([this](uint32_t index) { on_mode_chosen(index); });
        variant_row = box(mode_box, recompui::FlexDirection::Row, 16.0f);
        text(variant_row, "List entry", Typography::LabelMD);
        variant_radio = context.create_element<recompui::Radio>(variant_row);
        for (const char* name : { "1st", "2nd", "3rd", "4th" }) {
            variant_radio->add_option(name);
        }
        variant_radio->set_index(0);
        variant_radio->add_index_changed_callback([this](uint32_t index) { on_mode_chosen(mode_radio->get_index()); });
        text(variant_row, "which map, set or race the mode's own list starts with", Typography::LabelSM, color::TextDim);
        variant_row->set_display(recompui::Display::None);

        start_button = context.create_element<recompui::Button>(this, "Start game", recompui::ButtonStyle::Primary);
        start_button->add_pressed_callback([this]() { on_start_pressed(); });
        hint_label = text(this, "", Typography::Body, color::TextDim);
    }

    void LobbyPanel::open(bool as_host) {
        hosting = as_host;
        refresh_codes();
        ready = false;
        own_name = conker::netplay_helpers::load_settings(recomp::get_config_path()).name;
        name_input->set_text(own_name);
        ready_button->set_display(hosting ? recompui::Display::None : recompui::Display::Flex);
        ready_button->set_text("I'm ready");
        start_button->set_display(hosting ? recompui::Display::Flex : recompui::Display::None);
        delay_radio->set_display(hosting ? recompui::Display::Flex : recompui::Display::None);
        delay_radio->set_index(0);
        mode_box->set_display(hosting ? recompui::Display::Flex : recompui::Display::None);
        mode_radio->set_index(0);
        variant_radio->set_index(0);
        variant_row->set_display(recompui::Display::None);
        conker::lockstep::set_game_mode(0, 0);
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
        join_code = code;
        refresh_codes();
    }

    void LobbyPanel::set_room(const std::string& short_code, const std::string& status) {
        room_code = short_code;
        room_status_text = status;
        refresh_codes();
    }

    // What the share card shows: the room code big if there is one, else the long join code; and the router's news.
    void LobbyPanel::refresh_codes() {
        const std::string& big = room_code.empty() ? join_code : room_code;
        if (big != shown_big) {
            code_copy->set_text("Copy code");
            shown_big = big;
        }
        if (join_code != shown_long) {
            long_copy->set_text("Copy long code");
            shown_long = join_code;
        }
        code_caption->set_text(room_code.empty() ? "JOIN CODE" : "ROOM CODE");
        code_big->set_text(big);
        code_big->set_font_size(room_code.empty() ? 30.0f : 54.0f);
        const std::string small = !room_code.empty() && !join_code.empty() ? "Or the long join code: " + join_code : std::string();
        code_small->set_text(small);
        code_small->set_display(small.empty() ? recompui::Display::None : recompui::Display::Block);
        room_status->set_text(room_status_text);
        long_copy->set_display(!room_code.empty() && !join_code.empty() ? recompui::Display::Flex : recompui::Display::None);
        share_card->set_display(hosting && (!big.empty() || !room_status_text.empty()) ? recompui::Display::Flex : recompui::Display::None);
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

    void LobbyPanel::on_mode_chosen(uint32_t index) {
        // 0 is "Choose in game"; 1 to 7 are the scripted modes (lockstep's numbering).
        variant_row->set_display(index == 0 ? recompui::Display::None : recompui::Display::Flex);
        conker::lockstep::set_game_mode((int)index, index == 0 ? 0 : (int)variant_radio->get_index());
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
            const Color dim_fill{255, 255, 255, 30};
            const Color ink{246, 241, 235, 255};
            if (!entry.present) {
                row.name->set_text("Waiting for a player...");
                row.name->set_color(color::TextDim);
                row.role.tag->set_display(recompui::Display::None);
                row.device->set_text(hosting && i > 0 ? "Send them your join code" : "");
                row.ping->set_text("");
                row.status.tag->set_display(recompui::Display::None);
                row.view->show_idle();
                row.view->set_opacity(0.35f);
                row.box->set_opacity(0.8f);
                continue;
            }
            in++;
            // A host is ready by starting the game, so it counts as ready.
            const bool is_ready = entry.ready || i == 0;
            ready_count += is_ready ? 1 : 0;
            row.box->set_opacity(1.0f);
            row.view->set_opacity(1.0f);
            row.name->set_text(entry.name);
            row.name->set_color(color::Text);
            const bool own_row = i == own_port;
            const std::string role = i == 0 ? (own_row ? "HOST - YOU" : "HOST") : (own_row ? "YOU" : "");
            row.role.tag->set_display(role.empty() ? recompui::Display::None : recompui::Display::Flex);
            row.role.label->set_text(role);
            if (entry.controller == 0) {
                row.device->set_text(own_row ? "No controller (keyboard only)" : "No controller");
                if (own_row) {
                    row.device->set_color(Color{255, 196, 96, 255});
                }
                else {
                    row.device->set_color(color::TextDim);
                }
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
            row.status.tag->set_display(recompui::Display::Flex);
            row.status.label->set_text(is_ready ? "READY" : "NOT READY");
            row.status.label->set_color(is_ready ? Color{16, 12, 20, 255} : ink);
            row.status.tag->set_background_color(is_ready ? Color{96, 214, 128, 255} : dim_fill);
            row.box->set_border_color(is_ready ? Color{96, 214, 128, 110} : Color{255, 255, 255, 26});
            if (entry.pad_seen) {
                row.view->show(entry.pad);
            }
            else {
                row.view->show_idle();
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
