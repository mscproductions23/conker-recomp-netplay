#include "matchscript.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Usage: %s <mode> <players> <variant> <pause 0|1> <recording.pads>\n", argv[0]);
        return 2;
    }

    const char* mode_str = argv[1];
    char* end;
    int players = static_cast<int>(std::strtol(argv[2], &end, 10));
    int variant = static_cast<int>(std::strtol(argv[3], &end, 10));
    int pause = static_cast<int>(std::strtol(argv[4], &end, 10));
    const char* file_path = argv[5];

    if (pause != 0 && pause != 1) {
        fprintf(stderr, "Usage: %s <mode> <players> <variant> <pause 0|1> <recording.pads>\n", argv[0]);
        return 2;
    }
    if (players < 2 || players > 4) {
        fprintf(stderr, "Usage: %s <mode> <players> <variant> <pause 0|1> <recording.pads>\n", argv[0]);
        return 2;
    }
    if (*end != '\0' || variant < 0 || variant > 255) {
        fprintf(stderr, "Usage: %s <mode> <players> <variant> <pause 0|1> <recording.pads>\n", argv[0]);
        return 2;
    }

    conker::netplay::GameMode mode;
    if (strcmp(mode_str, "deathmatch") == 0) mode = conker::netplay::GameMode::deathmatch;
    else if (strcmp(mode_str, "war") == 0) mode = conker::netplay::GameMode::war;
    else if (strcmp(mode_str, "tank") == 0) mode = conker::netplay::GameMode::tank;
    else if (strcmp(mode_str, "race") == 0) mode = conker::netplay::GameMode::race;
    else if (strcmp(mode_str, "beach") == 0) mode = conker::netplay::GameMode::beach;
    else if (strcmp(mode_str, "raptor") == 0) mode = conker::netplay::GameMode::raptor;
    else if (strcmp(mode_str, "heist") == 0) mode = conker::netplay::GameMode::heist;
    else {
        fprintf(stderr, "Usage: %s <mode> <players> <variant> <pause 0|1> <recording.pads>\n", argv[0]);
        return 2;
    }

    try {
        conker::netplay::MatchScript script(mode, players, variant, pause != 0);

        std::ifstream file(file_path, std::ios::binary);
        if (!file) {
            fprintf(stderr, "cannot open %s\n", file_path);
            return 1;
        }

        std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        if (data.size() < 32 || (data.size() - 32) % 21 != 0) {
            fprintf(stderr, "size is not 32 + 21*N\n");
            return 1;
        }

        const unsigned char magic[8] = {'C', 'B', 'F', 'D', 'P', 'A', 'D', 'S'};
        if (std::memcmp(data.data(), magic, 8) != 0) {
            fprintf(stderr, "bad magic\n");
            return 1;
        }

        uint32_t file_players = data[12] | (data[13] << 8) | (data[14] << 16) | (data[15] << 24);
        if (file_players != static_cast<uint32_t>(players)) {
            fprintf(stderr, "players in file %u != argument %d\n", file_players, players);
            return 1;
        }

        size_t N = (data.size() - 32) / 21;

        if (script.length() != N) {
            fprintf(stderr, "script length %lu != file reads %zu\n", static_cast<unsigned long>(script.length()), N);
            return 1;
        }

        for (size_t r = 0; r < N; ++r) {
            size_t base = 32 + 21 * r;
            if (data[base] != 4) {
                fprintf(stderr, "read %zu: marker byte is %d, expected 4\n", r, data[base]);
                return 1;
            }

            for (int p = 0; p < 4; ++p) {
                size_t off = base + 1 + 5 * p;
                uint16_t buttons = data[off] | (data[off + 1] << 8);
                int8_t stick_x = static_cast<int8_t>(data[off + 2]);
                int8_t stick_y = static_cast<int8_t>(data[off + 3]);
                uint8_t err_no = data[off + 4];

                conker::netplay::ScriptPad expected{buttons, stick_x, stick_y, err_no};
                conker::netplay::ScriptPad actual = script.pad(r, p);

                if (expected != actual) {
                    printf("mismatch at read %zu port %d: file buttons=%d stick_x=%d stick_y=%d err_no=%d script buttons=%d stick_x=%d stick_y=%d err_no=%d\n",
                           r, p,
                           static_cast<int>(expected.buttons), static_cast<int>(expected.stick_x), static_cast<int>(expected.stick_y), static_cast<int>(expected.err_no),
                           static_cast<int>(actual.buttons), static_cast<int>(actual.stick_x), static_cast<int>(actual.stick_y), static_cast<int>(actual.err_no));
                    return 1;
                }
            }
        }

        printf("matchscript ok: %zu reads\n", N);
        return 0;
    } catch (std::exception&) {
        fprintf(stderr, "Usage: %s <mode> <players> <variant> <pause 0|1> <recording.pads>\n", argv[0]);
        return 2;
    }
}
