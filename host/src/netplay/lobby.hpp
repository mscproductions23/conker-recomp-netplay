// The Netplay menu's lobby: once a player has joined (or the host is waiting), everyone sees who is in,
// in the player's colour, with their name, what controller they have, how good their connection is,
// whether they're ready and, live, what their controller is doing (the N64 pad the game would get), so
// a bad controller or a bad connection shows before the game starts. Each player types a name (kept
// between runs) and, unless they're the host, presses Ready; the host sets the input delay (by default
// from everyone's ping) and presses Start game once every place is taken and every joiner is ready.
// The roster, pads, pings and the start come from lockstep (its lobby functions); this is how they look.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "recompui/recompui.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "elements/ui_radio.h"
#include "elements/ui_text_input.h"

#include "controller_view.hpp"
#include "net/packets.hpp"

namespace conker::netplay_lobby {
    // A small rounded tag with a label in it, which can be hidden or recoloured.
    struct Pill {
        recompui::Element* tag = nullptr;
        recompui::Label* label = nullptr;
    };

    class LobbyPanel : public recompui::Element {
    public:
        LobbyPanel(recompui::ResourceId rid, recompui::Element* parent);

        // Shows the lobby from a fresh start (not ready, the saved name), for a host or a joiner.
        void open(bool as_host);
        // Hides it.
        void close();
        // Redraws it from lockstep's roster, sends this player's pad; called every update while connecting.
        void refresh();
        // The host's join code to show (and copy), or empty for none: only a host's lobby shows it.
        void set_join_code(const std::string& code);
        // The room server's short code for this match (empty for none) and a line about reaching the host from
        // outside (the router's port, the room); shown under the join code, hidden when both are empty.
        void set_room(const std::string& short_code, const std::string& status);
        // A line for the player (a joiner was refused...), shown for a while.
        void show_notice(const std::string& text);

    protected:
        std::string_view get_type_name() override { return "LobbyPanel"; }

    private:
        struct Row {
            recompui::Element* box = nullptr;
            conker::netplay_view_pad::ControllerView* view = nullptr;
            Pill slot;
            recompui::Label* name = nullptr;
            Pill role;
            recompui::Label* device = nullptr;
            recompui::Label* ping = nullptr;
            Pill status;
        };

        // This player's name, Ready flag and controller go to lockstep a moment after they change, and at most a
        // few times a second: the host drops a joiner that sends its lobby more than 8 times in a second, which
        // a name typed quickly would otherwise do.
        void refresh_codes();
        void send_own();
        void flush_own();
        // This player's own pad, read as the game would, sent to the others a few times a second.
        void update_own_pad();
        uint8_t own_controller() const;
        void on_name_changed(const std::string& value);
        void on_ready_pressed();
        void on_start_pressed();
        void on_delay_chosen(uint32_t index);
        void on_mode_chosen(uint32_t index);

        bool hosting = false;
        bool ready = false;
        bool dirty = false;             // name, Ready or controller changed since it was last sent
        uint32_t last_sent = 0;         // SDL ticks of the last send
        uint8_t sent_controller = 0;
        netplay::Pad last_pad_sent{};
        uint32_t last_pad_ticks = 0;
        uint32_t notice_until = 0;
        std::string own_name;
        recompui::TextInput* name_input = nullptr;
        recompui::Button* ready_button = nullptr;
        recompui::Button* start_button = nullptr;
        recompui::Label* count_label = nullptr;
        recompui::Element* share_card = nullptr;
        recompui::Label* code_caption = nullptr;
        recompui::Label* code_big = nullptr;
        recompui::Label* code_small = nullptr;
        recompui::Label* room_status = nullptr;
        recompui::Button* code_copy = nullptr;
        recompui::Button* long_copy = nullptr;
        std::string room_status_text;
        std::string shown_big;
        std::string shown_long;
        recompui::Label* notice_label = nullptr;
        recompui::Label* delay_label = nullptr;
        recompui::Radio* delay_radio = nullptr;
        recompui::Element* mode_box = nullptr;
        recompui::Element* variant_row = nullptr;
        recompui::Radio* mode_radio = nullptr;
        recompui::Radio* variant_radio = nullptr;
        recompui::Label* hint_label = nullptr;
        std::string join_code;
        std::string room_code;
        std::vector<Row> rows;
    };
}
