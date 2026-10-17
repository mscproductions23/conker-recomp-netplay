#include <SDL.h>
#include <cstdio>
#include "recompinput/recompinput.h"
#include "conker.hpp"

namespace conker {
namespace controller_glyphs {

static _SDL_GameController* last = nullptr;

static const char* family_name(Family family) {
    switch (family) {
        case Family::Xbox: return "Xbox";
        case Family::PlayStation: return "PlayStation";
        case Family::Nintendo: return "Nintendo";
        default: return "Unknown";
    }
}

Info info_of(_SDL_GameController* controller) {
    if (controller == nullptr) {
        return {Family::Xbox, false, "Controller"};
    }

    switch (SDL_GameControllerGetType(controller)) {
        case SDL_CONTROLLER_TYPE_PS3: return {Family::PlayStation, false, "PlayStation 3"};
        case SDL_CONTROLLER_TYPE_PS4: return {Family::PlayStation, false, "PlayStation 4"};
        case SDL_CONTROLLER_TYPE_PS5: return {Family::PlayStation, true, "PlayStation 5 (DualSense)"};
        case SDL_CONTROLLER_TYPE_XBOX360: return {Family::Xbox, false, "Xbox 360"};
        case SDL_CONTROLLER_TYPE_XBOXONE: return {Family::Xbox, false, "Xbox One"};
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return {Family::Nintendo, false, "Nintendo Switch Pro"};
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
        case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return {Family::Nintendo, false, "Joy-Con"};
        default: return {Family::Xbox, false, "Controller"};
    }
}

void update() {
    auto* c = conker::frontend::primary_controller();
    if (c == nullptr || c == last) return;
    last = c;

    const Info info = info_of(c);

    recompinput::ControllerGlyphs g = recompinput::ControllerGlyphs::Xbox;
    switch (info.family) {
        case Family::Xbox: g = recompinput::ControllerGlyphs::Xbox; break;
        case Family::PlayStation: g = recompinput::ControllerGlyphs::PlayStation; break;
        case Family::Nintendo: g = recompinput::ControllerGlyphs::Nintendo; break;
    }

    recompinput::set_controller_glyphs(g, info.dualsense);
    std::printf("[controller] menus use %s glyphs for %s\n", family_name(info.family), info.name);
}

} // namespace controller_glyphs
} // namespace conker
