// The Netplay menu's lobby: once a player has joined (or the host is waiting), everyone sees who is
// in, with their names and whether they're ready, and the host starts the game when it wants to.
// Each player types a name (kept between runs) and, unless they're the host, presses Ready; the host's
// Start game button is enabled once every slot is taken and every joiner is ready. The roster and the
// start come from lockstep (its lobby functions); this is only how they look.
#pragma once

#include <string>
#include <vector>

#include "recompui/recompui.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "elements/ui_text_input.h"

namespace conker::netplay_lobby {
    class LobbyPanel : public recompui::Element {
    public:
        LobbyPanel(recompui::ResourceId rid, recompui::Element* parent);

        // Shows the lobby from a fresh start (not ready, the saved name), for a host or a joiner.
        void open(bool as_host);
        // Hides it.
        void close();
        // The host's join code to show (and copy), or empty for none: only a host's lobby shows it.
        void set_join_code(const std::string& code);
        // Redraws it from lockstep's roster; called every update while connecting.
        void refresh();

    protected:
        std::string_view get_type_name() override { return "LobbyPanel"; }

    private:
        struct Row {
            recompui::Element* box = nullptr;
            recompui::Label* slot = nullptr;
            recompui::Label* name = nullptr;
            recompui::Label* role = nullptr;
            recompui::Label* status = nullptr;
        };

        // This player's name and Ready flag are sent to lockstep a moment after they change, and at most a few
        // times a second: the host drops a joiner that sends its lobby more than 8 times in a second, which a
        // name typed quickly would otherwise do.
        void send_own();
        void flush_own();
        void on_name_changed(const std::string& value);
        void on_ready_pressed();
        void on_start_pressed();

        bool hosting = false;
        bool ready = false;
        bool dirty = false;             // name or Ready changed since it was last sent
        uint32_t last_sent = 0;         // SDL ticks of the last send
        std::string own_name;
        recompui::TextInput* name_input = nullptr;
        recompui::Button* ready_button = nullptr;
        recompui::Button* start_button = nullptr;
        recompui::Label* count_label = nullptr;
        recompui::Label* code_label = nullptr;
        recompui::Button* code_copy = nullptr;
        std::string join_code;
        recompui::Label* hint_label = nullptr;
        std::vector<Row> rows;
    };
}
