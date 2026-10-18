#include "matchscript.hpp"
#include <stdexcept>
#include <array>
#include <vector>

namespace conker::netplay {

    namespace {

        const uint16_t START = 0x1000;
        const uint16_t A = 0x8000;

        const std::array<int, 7> MODES = {0, 1, 2, 3, 5, 6, 7};

        void append_wait(std::vector<std::pair<uint8_t, ScriptPad>>& reads, long long n) {
            if (n > 0) {
                reads.insert(reads.end(), n, std::make_pair<uint8_t, ScriptPad>(0xFF, ScriptPad{}));
            }
        }

        void append_wait_until(std::vector<std::pair<uint8_t, ScriptPad>>& reads, long long target) {
            if (target > static_cast<long long>(reads.size())) {
                append_wait(reads, target - reads.size());
            }
        }

        void append_press(std::vector<std::pair<uint8_t, ScriptPad>>& reads, int port, uint16_t buttons, int stick_x, int stick_y, int count) {
            if (count > 0) {
                for (int i = 0; i < count; ++i) {
                    reads.push_back(std::make_pair(static_cast<uint8_t>(port), ScriptPad{buttons, static_cast<int8_t>(stick_x), static_cast<int8_t>(stick_y), 0}));
                }
            }
        }

    } // namespace

    MatchScript::MatchScript(GameMode mode, int players, int variant, bool pause)
        : players_(players)
    {
        if (players < 2 || players > 4) {
            throw std::invalid_argument("Invalid number of players");
        }
        if (static_cast<uint8_t>(mode) > 6) {
            throw std::invalid_argument("Invalid game mode");
        }

        reads_.clear();

        append_wait(reads_, 1140);
        append_press(reads_, 0, START, 0, 0, 4);
        append_wait_until(reads_, 1522);
        append_press(reads_, 0, START, 0, 0, 5);
        append_wait_until(reads_, 1700);
        append_press(reads_, 0, 0, 82, 0, 15);
        append_wait(reads_, 200);
        append_press(reads_, 0, A, 0, 0, 5);
        append_wait(reads_, 160);

        for (int i = 0; i < MODES[static_cast<uint8_t>(mode)]; ++i) {
            append_press(reads_, 0, 0, 80, 0, 8);
            append_wait(reads_, 40);
        }

        append_press(reads_, 0, A, 0, 0, 5);
        append_wait(reads_, 100);

        for (int i = 0; i < variant; ++i) {
            append_press(reads_, 0, 0, 0, -80, 8);
            append_wait(reads_, 30);
        }

        append_press(reads_, 0, A, 0, 0, 5);
        append_wait(reads_, 100);

        for (int port = 1; port < players; ++port) {
            append_press(reads_, port, START, 0, 0, 5);
            append_wait(reads_, 40);
        }

        append_wait(reads_, 60);

        std::vector<uint16_t> starts;
        if (mode == GameMode::race) {
            starts = {START, A};
        } else {
            starts = {START, A, START, A, A};
        }
        for (auto b : starts) {
            append_press(reads_, 0, b, 0, 0, 5);
            append_wait(reads_, mode == GameMode::race ? 60 : 120);
        }

        append_wait(reads_, mode == GameMode::race ? 300 : 900);

        if (pause) {
            append_press(reads_, 0, START, 0, 0, 5);
            append_wait(reads_, 200);
        }
    }

    uint64_t MatchScript::length() const {
        return static_cast<uint64_t>(reads_.size());
    }

    ScriptPad MatchScript::pad(uint64_t read, int port) const {
        if (port < 0 || port > 3) {
            throw std::out_of_range("Invalid port number");
        }
        if (port >= players_) {
            return ScriptPad{0, 0, 0, 8};
        }
        if (read >= length()) {
            return ScriptPad{};
        }
        auto entry = reads_[read];
        if (entry.first == static_cast<uint8_t>(port)) {
            return entry.second;
        }
        return ScriptPad{};
    }

    const char* mode_name(GameMode mode) {
        switch (mode) {
            case GameMode::deathmatch: return "deathmatch";
            case GameMode::war: return "war";
            case GameMode::tank: return "tank";
            case GameMode::race: return "race";
            case GameMode::beach: return "beach";
            case GameMode::raptor: return "raptor";
            case GameMode::heist: return "heist";
            default: return "unknown";
        }
    }

} // namespace conker::netplay
