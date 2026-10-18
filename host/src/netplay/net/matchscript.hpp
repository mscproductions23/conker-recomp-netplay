#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <utility>

namespace conker::netplay {

    enum class GameMode : uint8_t { deathmatch, war, tank, race, beach, raptor, heist };

    struct ScriptPad {
        uint16_t buttons = 0;
        int8_t stick_x = 0;
        int8_t stick_y = 0;
        uint8_t err_no = 0;
        bool operator==(const ScriptPad&) const = default;
    };

    class MatchScript {
    public:
        MatchScript(GameMode mode, int players, int variant = 0, bool pause = false);
        uint64_t length() const;
        ScriptPad pad(uint64_t read, int port) const;
    private:
        int players_;
        std::vector<std::pair<uint8_t, ScriptPad>> reads_;
    };

    const char* mode_name(GameMode mode);

}
