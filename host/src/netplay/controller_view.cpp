#include "controller_view.hpp"

#include <algorithm>

#include "elements/ui_svg.h"

namespace conker::netplay_view_pad {
    namespace {
        using recompui::Color;

        // The drawing is made on a 400 x 300 canvas and scaled to the width given.
        constexpr float canvas_width = 400.0f;

        enum class Shape { circle, round_rect };

        struct PartDef {
            uint16_t mask;
            Shape shape;
            float cx, cy, w, h;     // the centre and size (a circle's w is its diameter), in canvas units
            Color dark, lit;
            const char* label;
        };

        // Where each button is on the real controller: the D-pad on the left prong, the stick and Start in the
        // middle, B and A and the four yellow C buttons on the right, L and R on the shoulders and Z under the
        // middle prong.
        const Color dpad_dark{70, 64, 82, 255};
        const Color dpad_lit{255, 244, 232, 255};
        const PartDef defs[] = {
            { 0x0800, Shape::round_rect,  82.0f,  96.0f, 24.0f, 28.0f, dpad_dark, dpad_lit, nullptr },   // D up
            { 0x0400, Shape::round_rect,  82.0f, 146.0f, 24.0f, 28.0f, dpad_dark, dpad_lit, nullptr },   // D down
            { 0x0200, Shape::round_rect,  54.0f, 121.0f, 28.0f, 24.0f, dpad_dark, dpad_lit, nullptr },   // D left
            { 0x0100, Shape::round_rect, 110.0f, 121.0f, 28.0f, 24.0f, dpad_dark, dpad_lit, nullptr },   // D right
            { 0x1000, Shape::circle,     200.0f,  96.0f, 24.0f, 24.0f, Color{116, 36, 44, 255}, Color{255, 90, 96, 255}, nullptr },   // Start
            { 0x4000, Shape::circle,     276.0f, 150.0f, 34.0f, 34.0f, Color{36, 102, 62, 255}, Color{96, 232, 140, 255}, "B" },
            { 0x8000, Shape::circle,     322.0f, 168.0f, 40.0f, 40.0f, Color{40, 76, 150, 255}, Color{104, 160, 255, 255}, "A" },
            { 0x0008, Shape::circle,     332.0f,  84.0f, 22.0f, 22.0f, Color{118, 100, 30, 255}, Color{255, 218, 70, 255}, nullptr },   // C up
            { 0x0004, Shape::circle,     332.0f, 130.0f, 22.0f, 22.0f, Color{118, 100, 30, 255}, Color{255, 218, 70, 255}, nullptr },   // C down
            { 0x0002, Shape::circle,     309.0f, 107.0f, 22.0f, 22.0f, Color{118, 100, 30, 255}, Color{255, 218, 70, 255}, nullptr },   // C left
            { 0x0001, Shape::circle,     355.0f, 107.0f, 22.0f, 22.0f, Color{118, 100, 30, 255}, Color{255, 218, 70, 255}, nullptr },   // C right
            { 0x0020, Shape::round_rect,  70.0f,  22.0f, 92.0f, 16.0f, dpad_dark, dpad_lit, "L" },       // L shoulder
            { 0x0010, Shape::round_rect, 330.0f,  22.0f, 92.0f, 16.0f, dpad_dark, dpad_lit, "R" },       // R shoulder
            { 0x2000, Shape::round_rect, 200.0f, 292.0f, 54.0f, 14.0f, dpad_dark, dpad_lit, "Z" },       // Z trigger
        };
        static_assert(sizeof(defs) / sizeof(defs[0]) == 14, "one part for each button");

        constexpr float stick_cx = 200.0f;
        constexpr float stick_cy = 176.0f;
        constexpr float stick_ring = 66.0f;
        constexpr float nub_size = 34.0f;
    }

    ControllerView::ControllerView(recompui::ResourceId rid, recompui::Element* parent, float width, Color accent_color)
        : Element(rid, parent), scale(width / canvas_width), accent(accent_color) {
        recompui::ContextId context = recompui::get_current_context();
        set_display(recompui::Display::Flex);
        set_position(recompui::Position::Relative);
        set_width(width);
        set_height(width * 0.75f);
        set_min_width(width);
        set_flex_shrink(0.0f);

        // The outline in the player's colour, then the body on top of it, a little smaller.
        const auto layer = [&](float inset, const Color& tint) {
            recompui::Svg* svg = context.create_element<recompui::Svg>(this, "icons/N64Pad.svg");
            svg->set_position(recompui::Position::Absolute);
            svg->set_left(inset * scale);
            svg->set_top(inset * scale);
            svg->set_width((canvas_width - 2.0f * inset) * scale);
            svg->set_height((300.0f - 2.0f * inset) * scale);
            svg->set_image_color(tint);
        };
        layer(0.0f, Color{accent.r, accent.g, accent.b, 255});
        layer(5.0f, Color{58, 48, 70, 255});

        // The stick's well and its nub.
        const float ring = stick_ring * scale;
        recompui::Element* well = context.create_element<recompui::Element>(this);
        well->set_position(recompui::Position::Absolute);
        well->set_left((stick_cx - stick_ring / 2.0f) * scale);
        well->set_top((stick_cy - stick_ring / 2.0f) * scale);
        well->set_width(ring);
        well->set_height(ring);
        well->set_border_radius(ring / 2.0f);
        well->set_background_color(Color{20, 15, 26, 255});
        well->set_border_width(2.0f * scale);
        well->set_border_color(Color{70, 62, 84, 255});

        nub = context.create_element<recompui::Element>(this);
        nub->set_position(recompui::Position::Absolute);
        const float nub_px = nub_size * scale;
        nub_middle = stick_cx * scale - nub_px / 2.0f;
        nub_reach = (stick_ring - nub_size) / 2.0f * scale;
        nub->set_left(nub_middle);
        nub->set_top(stick_cy * scale - nub_px / 2.0f);
        nub->set_width(nub_px);
        nub->set_height(nub_px);
        nub->set_border_radius(nub_px / 2.0f);
        nub->set_background_color(Color{accent.r, accent.g, accent.b, 255});
        nub->set_border_width(2.0f * scale);
        nub->set_border_color(Color{255, 255, 255, 90});

        // The buttons.
        for (int i = 0; i < button_count; i++) {
            const PartDef& def = defs[i];
            Part& part = parts[i];
            part.dark = def.dark;
            part.lit = def.lit;
            recompui::Element* element = context.create_element<recompui::Element>(this);
            element->set_position(recompui::Position::Absolute);
            element->set_left((def.cx - def.w / 2.0f) * scale);
            element->set_top((def.cy - def.h / 2.0f) * scale);
            element->set_width(def.w * scale);
            element->set_height(def.h * scale);
            element->set_border_radius(def.shape == Shape::circle ? def.w * scale / 2.0f : 5.0f * scale);
            element->set_background_color(part.dark);
            element->set_border_width(2.0f * scale);
            element->set_border_color(Color{255, 255, 255, 0});
            if (def.label != nullptr) {
                element->set_display(recompui::Display::Flex);
                element->set_align_items(recompui::AlignItems::Center);
                element->set_justify_content(recompui::JustifyContent::Center);
                recompui::Label* label = context.create_element<recompui::Label>(element, def.label, recompui::theme::Typography::LabelSM);
                label->set_font_size(std::max(9.0f, 16.0f * scale));
                label->set_color(Color{255, 255, 255, 190});
            }
            part.element = element;
        }
        show_idle();
    }

    void ControllerView::set_lit(Part& part, bool lit) {
        if (lit == part.is_lit) {
            return;
        }
        part.is_lit = lit;
        part.element->set_background_color(lit ? part.lit : part.dark);
        part.element->set_border_color(Color{255, 255, 255, static_cast<uint8_t>(lit ? 230 : 0)});
    }

    void ControllerView::show(const netplay::Pad& pad) {
        for (int i = 0; i < button_count; i++) {
            set_lit(parts[i], (pad.buttons & defs[i].mask) != 0);
        }
        const float x = std::clamp(pad.stick_x / 80.0f, -1.0f, 1.0f);
        const float y = std::clamp(pad.stick_y / 80.0f, -1.0f, 1.0f);
        nub->set_left(nub_middle + x * nub_reach);
        nub->set_top(stick_cy * scale - nub_size * scale / 2.0f - y * nub_reach);    // the stick's up is positive
    }

    void ControllerView::show_idle() {
        show(netplay::Pad{});
    }
}
