// A drawing of an N64 controller that shows a pad live: each button lights when it's down, and the stick's
// nub moves with the stick. It's used for every player in the lobby (what they're pressing, as the game
// would read it) and, large, for the player's own controller on the Netplay page, so a pad that doesn't
// answer shows before a match. The drawing is the N64's own trident shape with the buttons where they are on
// the real one, and its size is whatever width it's given.
#pragma once

#include <array>

#include "recompui/recompui.h"
#include "elements/ui_label.h"

#include "net/packets.hpp"

namespace conker::netplay_view_pad {
    class ControllerView : public recompui::Element {
    public:
        // `width` in dp (the height is 3/4 of it); `accent` is the player's colour, for the outline and the nub.
        ControllerView(recompui::ResourceId rid, recompui::Element* parent, float width, recompui::Color accent);

        // Lights what `pad` has down and moves the stick's nub.
        void show(const netplay::Pad& pad);
        // Nothing to show (the player hasn't sent a pad yet): the drawing stays dark.
        void show_idle();

    protected:
        std::string_view get_type_name() override { return "ControllerView"; }

    private:
        static constexpr int button_count = 14;

        struct Part {
            recompui::Element* element = nullptr;
            recompui::Color dark{};
            recompui::Color lit{};
            bool is_lit = false;
        };

        void set_lit(Part& part, bool lit);

        float scale = 1.0f;
        recompui::Color accent{};
        std::array<Part, button_count> parts;
        recompui::Element* nub = nullptr;
        float nub_middle = 0.0f;
        float nub_reach = 0.0f;
    };
}
