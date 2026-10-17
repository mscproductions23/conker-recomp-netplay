// The launcher's Controller Test page: each connected controller, with its name and family
// (Xbox, PlayStation or Nintendo), every button as a glyph that lights up while it's held, the two
// sticks as dots, and the triggers as bars. It reads the controllers itself, so nothing here
// reaches the game, and the menus' own use of the buttons is switched off while it's open: the
// Back/Toggle button doesn't close it and no button can press anything. To leave, hold Start
// (Menu, Options or Plus) or Esc for a second and a half, or click Close with the mouse.

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <SDL.h>

#include "recompui/recompui.h"
#include "recompinput/recompinput.h"
#include "elements/ui_button.h"
#include "elements/ui_label.h"
#include "elements/ui_modal.h"

#include "conker.hpp"

namespace {
    using conker::controller_glyphs::Family;
    using recompui::theme::Typography;
    using recompui::theme::color;

    constexpr int max_cards = 4;
    constexpr uint32_t hold_to_leave_ms = 1500;

    recompui::Modal* modal = nullptr;

    // One button's chip: its glyph and caption, lit while it's held.
    struct Chip {
        SDL_GameControllerButton button;
        recompui::Element* box = nullptr;
        recompui::Label* glyph = nullptr;
        recompui::Label* caption = nullptr;
    };

    struct StickView {
        recompui::Element* dot = nullptr;
        recompui::Label* value = nullptr;
    };

    struct TriggerView {
        recompui::Element* fill = nullptr;
        recompui::Label* value = nullptr;
    };

    struct Card {
        recompui::Element* box = nullptr;
        recompui::Label* name = nullptr;
        recompui::Label* family = nullptr;
        std::vector<Chip> chips;
        StickView left_stick, right_stick;
        TriggerView left_trigger, right_trigger;
        _SDL_GameController* shown = nullptr;   // the controller the card was last filled for
    };

    // The caption under a button's glyph, in its family's own words.
    const char* caption_for(SDL_GameControllerButton button, Family family) {
        const bool ps = family == Family::PlayStation;
        const bool nin = family == Family::Nintendo;
        switch (button) {
        case SDL_CONTROLLER_BUTTON_A: return ps ? "Cross" : "A";
        case SDL_CONTROLLER_BUTTON_B: return ps ? "Circle" : "B";
        case SDL_CONTROLLER_BUTTON_X: return ps ? "Square" : "X";
        case SDL_CONTROLLER_BUTTON_Y: return ps ? "Triangle" : "Y";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ps ? "L1" : (nin ? "L" : "LB");
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ps ? "R1" : (nin ? "R" : "RB");
        case SDL_CONTROLLER_BUTTON_BACK: return ps ? "Share" : (nin ? "Minus" : "View");
        case SDL_CONTROLLER_BUTTON_START: return ps ? "Options" : (nin ? "Plus" : "Menu");
        case SDL_CONTROLLER_BUTTON_GUIDE: return ps ? "PS" : (nin ? "Home" : "Guide");
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return ps ? "L3" : "Left stick";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return ps ? "R3" : "Right stick";
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "Right";
        case SDL_CONTROLLER_BUTTON_TOUCHPAD: return "Touchpad";
        default: return "?";
        }
    }

    recompinput::ControllerGlyphs glyph_family(Family family) {
        switch (family) {
        case Family::PlayStation: return recompinput::ControllerGlyphs::PlayStation;
        case Family::Nintendo: return recompinput::ControllerGlyphs::Nintendo;
        default: return recompinput::ControllerGlyphs::Xbox;
        }
    }

    const char* family_name(Family family) {
        switch (family) {
        case Family::PlayStation: return "PlayStation";
        case Family::Nintendo: return "Nintendo";
        default: return "Xbox";
        }
    }

    std::string two_decimals(float value) {
        char text[16];
        std::snprintf(text, sizeof(text), "%+.2f", value);
        return text;
    }

    class ControllerTestPage : public recompui::Element {
        std::vector<Card> cards;
        recompui::Label* empty_label = nullptr;
        recompui::Label* leave_label = nullptr;
        recompui::Element* leave_fill = nullptr;
        uint32_t held_since = 0;     // when Start (or Esc) went down, 0 while it isn't

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

        // A bar: a track with a fill whose width follows the value.
        recompui::Element* bar(recompui::Element* parent, float width, recompui::theme::color fill_color) {
            recompui::ContextId context = recompui::get_current_context();
            recompui::Element* track = context.create_element<recompui::Element>(parent);
            track->set_width(width);
            track->set_height(14.0f);
            track->set_border_radius(7.0f);
            track->set_background_color(color::ElevatedSoft);
            track->set_overflow(recompui::Overflow::Hidden);
            recompui::Element* fill = context.create_element<recompui::Element>(track);
            fill->set_width(0.0f, recompui::Unit::Percent);
            fill->set_height(100.0f, recompui::Unit::Percent);
            fill->set_background_color(fill_color);
            return fill;
        }

        // A stick's square, with a dot that follows it.
        StickView stick(recompui::Element* parent, const char* title) {
            recompui::ContextId context = recompui::get_current_context();
            recompui::Element* column = box(parent, recompui::FlexDirection::Column, 6.0f);
            text(column, title, Typography::LabelSM, color::TextDim);
            recompui::Element* pad = context.create_element<recompui::Element>(column);
            pad->set_position(recompui::Position::Relative);
            pad->set_width(120.0f);
            pad->set_height(120.0f);
            pad->set_border_radius(16.0f);
            pad->set_border_width(2.0f);
            pad->set_border_color(color::Border);
            pad->set_background_color(color::ElevatedSoft);
            StickView view;
            view.dot = context.create_element<recompui::Element>(pad);
            view.dot->set_position(recompui::Position::Absolute);
            view.dot->set_width(16.0f);
            view.dot->set_height(16.0f);
            view.dot->set_border_radius(8.0f);
            view.dot->set_background_color(color::Primary);
            view.dot->set_left(52.0f);
            view.dot->set_top(52.0f);
            view.value = text(column, two_decimals(0) + "  " + two_decimals(0), Typography::LabelSM, color::TextDim);
            return view;
        }

        TriggerView trigger(recompui::Element* parent, const char* title) {
            recompui::Element* column = box(parent, recompui::FlexDirection::Column, 6.0f);
            column->set_align_items(recompui::AlignItems::FlexStart);
            text(column, title, Typography::LabelSM, color::TextDim);
            TriggerView view;
            view.fill = bar(column, 200.0f, color::Secondary);
            view.value = text(column, "0%", Typography::LabelSM, color::TextDim);
            return view;
        }

        void build_card(Card& card, recompui::Element* parent) {
            recompui::ContextId context = recompui::get_current_context();
            card.box = box(parent, recompui::FlexDirection::Column, 16.0f);
            card.box->set_align_items(recompui::AlignItems::FlexStart);
            card.box->set_width(100.0f, recompui::Unit::Percent);
            card.box->set_padding(24.0f);
            card.box->set_border_radius(recompui::theme::border::radius_lg);
            card.box->set_background_color(color::Background2);
            card.box->set_display(recompui::Display::None);

            recompui::Element* title = box(card.box, recompui::FlexDirection::Row, 16.0f);
            card.name = text(title, "", Typography::LabelLG);
            card.family = text(title, "", Typography::LabelMD, color::Primary);

            static const SDL_GameControllerButton order[] = {
                SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
                SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_GUIDE,
                SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
                SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
                SDL_CONTROLLER_BUTTON_TOUCHPAD,
            };
            recompui::Element* chips = box(card.box, recompui::FlexDirection::Row, 10.0f);
            chips->set_flex_wrap(recompui::FlexWrap::Wrap);
            chips->set_width(100.0f, recompui::Unit::Percent);
            for (SDL_GameControllerButton button : order) {
                Chip chip;
                chip.button = button;
                chip.box = box(chips, recompui::FlexDirection::Column, 4.0f);
                chip.box->set_width(104.0f);
                chip.box->set_padding(10.0f);
                chip.box->set_border_radius(recompui::theme::border::radius_md);
                chip.box->set_background_color(color::ElevatedSoft);
                chip.glyph = text(chip.box, "", Typography::Header3);
                chip.glyph->set_font_family("promptfont");
                chip.caption = text(chip.box, "", Typography::LabelSM, color::TextDim);
                card.chips.push_back(chip);
            }

            recompui::Element* analog = box(card.box, recompui::FlexDirection::Row, 40.0f);
            analog->set_align_items(recompui::AlignItems::FlexStart);
            card.left_stick = stick(analog, "Left stick");
            card.right_stick = stick(analog, "Right stick");
            recompui::Element* triggers = box(analog, recompui::FlexDirection::Column, 18.0f);
            triggers->set_align_items(recompui::AlignItems::FlexStart);
            card.left_trigger = trigger(triggers, "Left trigger");
            card.right_trigger = trigger(triggers, "Right trigger");
        }

        // Fills a card for a controller: its name, family, and the glyphs of that family.
        void fill_card(Card& card, _SDL_GameController* controller) {
            const conker::controller_glyphs::Info info = conker::controller_glyphs::info_of(controller);
            const char* name = SDL_GameControllerName(controller);
            card.name->set_text(name != nullptr ? name : "Controller");
            card.family->set_text(std::string(family_name(info.family)) + " glyphs, " + info.name);
            for (Chip& chip : card.chips) {
                const bool has = chip.button != SDL_CONTROLLER_BUTTON_TOUCHPAD ||
                    SDL_GameControllerGetNumTouchpads(controller) > 0;
                chip.box->set_display(has ? recompui::Display::Flex : recompui::Display::None);
                chip.glyph->set_text(recompinput::controller_button_glyph(chip.button, glyph_family(info.family), info.dualsense));
                chip.caption->set_text(caption_for(chip.button, info.family));
            }
            card.shown = controller;
        }

        static float axis(_SDL_GameController* controller, SDL_GameControllerAxis which) {
            return std::clamp(SDL_GameControllerGetAxis(controller, which) / 32767.0f, -1.0f, 1.0f);
        }

        void show_stick(StickView& view, float x, float y) {
            view.dot->set_left(52.0f + x * 52.0f);
            view.dot->set_top(52.0f + y * 52.0f);
            view.value->set_text(two_decimals(x) + "  " + two_decimals(y));
        }

        void show_trigger(TriggerView& view, float value) {
            const float amount = std::clamp(value, 0.0f, 1.0f);
            view.fill->set_width(amount * 100.0f, recompui::Unit::Percent);
            view.value->set_text(std::to_string((int)(amount * 100.0f + 0.5f)) + "%");
        }

        void refresh() {
            const std::vector<_SDL_GameController*> controllers = conker::frontend::connected_controllers();
            const int shown = std::min((int)controllers.size(), max_cards);
            empty_label->set_display(shown == 0 ? recompui::Display::Block : recompui::Display::None);
            bool start_held = false;
            for (int i = 0; i < max_cards; i++) {
                Card& card = cards[i];
                if (i >= shown) {
                    card.box->set_display(recompui::Display::None);
                    card.shown = nullptr;
                    continue;
                }
                _SDL_GameController* controller = controllers[i];
                card.box->set_display(recompui::Display::Flex);
                if (card.shown != controller) {
                    fill_card(card, controller);
                }
                for (Chip& chip : card.chips) {
                    const bool down = SDL_GameControllerGetButton(controller, chip.button) != 0;
                    chip.box->set_background_color(down ? color::Primary : color::ElevatedSoft);
                    chip.glyph->set_color(down ? color::White : color::Text);
                    chip.caption->set_color(down ? color::White : color::TextDim);
                    if (down && chip.button == SDL_CONTROLLER_BUTTON_START) {
                        start_held = true;
                    }
                }
                show_stick(card.left_stick, axis(controller, SDL_CONTROLLER_AXIS_LEFTX), axis(controller, SDL_CONTROLLER_AXIS_LEFTY));
                show_stick(card.right_stick, axis(controller, SDL_CONTROLLER_AXIS_RIGHTX), axis(controller, SDL_CONTROLLER_AXIS_RIGHTY));
                show_trigger(card.left_trigger, axis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
                show_trigger(card.right_trigger, axis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
            }
            // Hold Start (or Esc) to leave.
            if (SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_ESCAPE]) {
                start_held = true;
            }
            if (!start_held) {
                held_since = 0;
                leave_fill->set_width(0.0f, recompui::Unit::Percent);
                return;
            }
            const uint32_t now = SDL_GetTicks();
            if (held_since == 0) {
                held_since = now;
            }
            const float progress = std::min(1.0f, (float)(now - held_since) / (float)hold_to_leave_ms);
            leave_fill->set_width(progress * 100.0f, recompui::Unit::Percent);
            if (progress >= 1.0f) {
                held_since = 0;
                leave_fill->set_width(0.0f, recompui::Unit::Percent);
                if (modal != nullptr) {
                    modal->close();
                }
            }
        }

    protected:
        std::string_view get_type_name() override { return "ControllerTestPage"; }

        void process_event(const recompui::Event& e) override {
            if (e.type == recompui::EventType::Update) {
                refresh();
                queue_update();
            }
        }

    public:
        ControllerTestPage(recompui::ResourceId rid, recompui::Element* parent)
            : Element(rid, parent, recompui::Events(recompui::EventType::Update)) {
            recompui::ContextId context = recompui::get_current_context();
            set_display(recompui::Display::Flex);
            set_flex_direction(recompui::FlexDirection::Column);
            set_gap(24.0f);
            set_padding(32.0f);
            set_width(100.0f, recompui::Unit::Percent);
            set_background_color(color::Background1);

            recompui::Element* head = box(this, recompui::FlexDirection::Row, 24.0f);
            head->set_width(100.0f, recompui::Unit::Percent);
            head->set_justify_content(recompui::JustifyContent::SpaceBetween);
            recompui::Element* titles = box(head, recompui::FlexDirection::Column, 8.0f);
            titles->set_align_items(recompui::AlignItems::FlexStart);
            text(titles, "Controller Test", Typography::Header3);
            text(titles, "Press buttons, move the sticks and pull the triggers on any controller. Nothing here "
                "reaches the game, and no button can press anything in the menus while this is open.",
                Typography::Body, color::TextDim);
            // Mouse only: it can't be reached with a controller, so no button can close it by accident.
            recompui::Button* close = context.create_element<recompui::Button>(head, "Close", recompui::ButtonStyle::Secondary);
            close->set_focusable(false);
            close->add_pressed_callback([]() {
                if (modal != nullptr) {
                    modal->close();
                }
            });

            empty_label = text(this, "No controller found. Connect one with USB or Bluetooth and press a button.",
                Typography::LabelMD, color::TextDim);
            empty_label->set_display(recompui::Display::None);

            cards.resize(max_cards);
            for (Card& card : cards) {
                build_card(card, this);
            }

            recompui::Element* leave = box(this, recompui::FlexDirection::Row, 16.0f);
            leave_label = text(leave, "To leave, hold Start (Menu, Options or Plus) or Esc for a second and a half.",
                Typography::LabelSM, color::TextDim);
            leave_fill = bar(leave, 200.0f, color::Primary);
            queue_update();
        }
    };

    ControllerTestPage* page = nullptr;
}

void conker::controller_test::open() {
    if (modal == nullptr) {
        // The launcher's option calls this with the launcher's context open, and only one can be: it's
        // closed while the page's is made.
        recompui::ContextId launcher = recompui::get_current_context();
        const bool launcher_open = launcher.slot_id != recompui::ContextId::null().slot_id;
        if (launcher_open) {
            launcher.close();
        }
        modal = recompui::Modal::create_modal(recompui::ModalType::Fullscreen);
        recompui::ContextId context = modal->modal_root_context;
        context.open();
        page = context.create_element<ControllerTestPage>(modal->get_body());
        // The modal's Toggle button (Esc, Start) would close it, and its header's close button could be
        // pressed with a controller: switch the first off and hide the second. Leaving is the page's own.
        // (Both change elements, so the page's context has to be open still.)
        modal->set_menu_action_callback(recompui::MenuAction::Toggle, []() {}, "");
        modal->get_header()->set_display(recompui::Display::None);
        context.close();
        if (launcher_open) {
            launcher.open();
        }
    }
    modal->open();
}
