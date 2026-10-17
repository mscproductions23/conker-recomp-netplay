// The launcher and menus' look: a warm, dark palette with an orange accent, in place of
// RecompFrontend's default blue, and softer corners. Colours are set through recompui's theme
// (every element reads them as it's made), so this runs before any UI is built.

#include <algorithm>
#include <cstdint>

#include "recompui/recompui.h"

#include "conker.hpp"

namespace {
    using recompui::Color;
    using recompui::theme::color;
    using recompui::theme::set_theme_color;

    // The strengths RecompFrontend names its translucent shades by (A5 is 5%, A80 80%).
    uint8_t percent(int p) {
        return static_cast<uint8_t>((p * 255 + 50) / 100);
    }

    // A colour's full set: the colour, a lighter and a darker one, and its translucent shades.
    void set_family(color base, color light, color dark, color a5, color a20, color a30, color a50, color a80,
                    Color value) {
        const auto mix = [&](int toward, int amount) {
            // Mixes the colour toward white (255) or black (0) by amount percent.
            const auto channel = [&](uint8_t c) {
                return static_cast<uint8_t>(c + ((toward - c) * amount) / 100);
            };
            return Color{channel(value.r), channel(value.g), channel(value.b), 255};
        };
        const auto with_alpha = [&](int p) { return Color{value.r, value.g, value.b, percent(p)}; };
        set_theme_color(base, value);
        set_theme_color(light, mix(255, 55));
        set_theme_color(dark, mix(0, 45));
        set_theme_color(a5, with_alpha(5));
        set_theme_color(a20, with_alpha(20));
        set_theme_color(a30, with_alpha(30));
        set_theme_color(a50, with_alpha(50));
        set_theme_color(a80, with_alpha(80));
    }
}

void conker::launcher_theme::apply() {
    // Backgrounds: a deep plum-black, a little lighter for each layer on top.
    set_theme_color(color::Background1, Color{16, 12, 20, 255});
    set_theme_color(color::Background2, Color{26, 20, 32, 255});
    set_theme_color(color::Background3, Color{38, 29, 46, 255});
    set_theme_color(color::BGOverlay, Color{236, 208, 184, 26});
    set_theme_color(color::ModalOverlay, Color{16, 12, 20, 232});
    set_theme_color(color::BGShadow, Color{0, 0, 0, 96});
    set_theme_color(color::BGShadow2, Color{16, 12, 20, 190});

    // Text: warm white.
    set_theme_color(color::Text, Color{246, 241, 235, 255});
    set_theme_color(color::TextActive, Color{255, 250, 244, 255});
    set_theme_color(color::TextDim, Color{208, 198, 188, 255});
    set_theme_color(color::TextInactive, Color{246, 241, 235, 140});
    set_theme_color(color::TextA5, Color{246, 241, 235, percent(5)});
    set_theme_color(color::TextA20, Color{246, 241, 235, percent(20)});
    set_theme_color(color::TextA30, Color{246, 241, 235, percent(30)});
    set_theme_color(color::TextA50, Color{246, 241, 235, percent(50)});
    set_theme_color(color::TextA80, Color{246, 241, 235, percent(80)});

    // The accent (selection, buttons, the title): Conker's orange. The second colour, which the
    // focused launcher option pulses in, is a soft gold.
    set_family(color::Primary, color::PrimaryL, color::PrimaryD, color::PrimaryA5, color::PrimaryA20,
               color::PrimaryA30, color::PrimaryA50, color::PrimaryA80, Color{241, 122, 36, 255});
    set_family(color::Secondary, color::SecondaryL, color::SecondaryD, color::SecondaryA5, color::SecondaryA20,
               color::SecondaryA30, color::SecondaryA50, color::SecondaryA80, Color{255, 200, 92, 255});

    // Raised panels and outlines, tinted to match instead of the default cool blue-grey.
    set_theme_color(color::Elevated, Color{236, 208, 184, 26});
    set_theme_color(color::ElevatedSoft, Color{236, 208, 184, 13});
    set_theme_color(color::ElevatedBorder, Color{241, 160, 96, 204});
    set_theme_color(color::ElevatedBorderHard, Color{241, 160, 96, 255});
    set_theme_color(color::Border, Color{255, 236, 220, 51});
    set_theme_color(color::BorderSoft, Color{255, 236, 220, 26});
    set_theme_color(color::BorderHard, Color{255, 236, 220, 77});
    set_theme_color(color::BorderSolid, Color{255, 236, 220, 153});

    // Softer, rounder corners.
    recompui::theme::set_border_radius_sm(6.0f);
    recompui::theme::set_border_radius_md(12.0f);
    recompui::theme::set_border_radius_lg(18.0f);
    recompui::theme::set_border_width(1.5f);
}
