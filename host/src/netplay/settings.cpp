#include "settings.hpp"

#include <fstream>
#include <charconv>
#include <system_error>
#include <algorithm>
#include <cstddef>
#include <string_view>
#include <cctype>
#include <string>
#include <filesystem>

namespace conker::netplay_helpers {

namespace {

std::string clean_text(std::string_view text, std::size_t max_length) {
    std::string cleaned;

    // Keep only characters in the range 0x20..0x7E
    for (char c : text) {
        if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) <= 0x7E) {
            cleaned.push_back(c);
        }
    }

    // Trim leading and trailing spaces
    auto first = cleaned.find_first_not_of(' ');
    auto last = cleaned.find_last_not_of(' ');
    if (first != std::string::npos && last != std::string::npos) {
        cleaned = cleaned.substr(first, last - first + 1);
    } else if (first != std::string::npos) {
        cleaned = cleaned.substr(first);
    } else {
        cleaned.clear();
    }

    // Cut to max length
    if (cleaned.size() > max_length) {
        cleaned.resize(max_length);
    }

    // Trim leading and trailing spaces again
    first = cleaned.find_first_not_of(' ');
    last = cleaned.find_last_not_of(' ');
    if (first != std::string::npos && last != std::string::npos) {
        cleaned = cleaned.substr(first, last - first + 1);
    } else if (first != std::string::npos) {
        cleaned = cleaned.substr(first);
    } else {
        cleaned.clear();
    }

    return cleaned;
}

bool parse_integer(std::string_view text, long long& result) {
    // Handle leading '+' or '-'
    bool negative = false;
    size_t start = 0;

    if (!text.empty()) {
        if (text[0] == '+') {
            start = 1;
        } else if (text[0] == '-') {
            negative = true;
            start = 1;
        }
    }

    // Ensure there's at least one digit
    if (start < text.size() && (text[start] < '0' || text[start] > '9')) {
        return false;
    }

    std::from_chars_result conv_result = std::from_chars(
        text.data() + start,
        text.data() + text.size(),
        result
    );

    if (conv_result.ec == std::errc{} && conv_result.ptr == text.data() + text.size()) {
        if (negative) {
            result = -result;
        }
        return true;
    }
    return false;
}

std::string read_old_name_file(const std::filesystem::path& folder) {
    std::filesystem::path old_name_path = folder / "netplay_name.txt";
    std::ifstream old_name_file(old_name_path, std::ios::binary);
    if (!old_name_file) {
        return "";
    }

    std::string name_line;
    std::getline(old_name_file, name_line);
    if (!name_line.empty() && name_line.back() == '\r') {
        name_line.pop_back();
    }

    return clean_text(name_line, 16);
}

} // namespace

Settings load_settings(const std::filesystem::path& folder) {
    Settings settings;

    std::error_code ec;
    std::filesystem::path settings_path = folder / "netplay_settings.txt";

    std::ifstream settings_file(settings_path, std::ios::binary);
    if (!settings_file) {
        settings.name = read_old_name_file(folder);
        return settings;
    }

    std::string line;
    while (std::getline(settings_file, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::size_t sep_pos = line.find('=');
        if (sep_pos == std::string::npos) {
            continue;
        }

        std::string_view line_view(line);
        std::string_view key = line_view.substr(0, sep_pos);
        std::string_view value = line_view.substr(sep_pos + 1);

        if (key == "port") {
            long long port_value;
            if (parse_integer(value, port_value)) {
                settings.port = std::clamp(port_value, 1LL, 65535LL);
            }
        } else if (key == "players") {
            long long players_value;
            if (parse_integer(value, players_value)) {
                settings.players = std::clamp(players_value, 2LL, 4LL);
            }
        } else if (key == "address") {
            settings.address = clean_text(value, 64);
        } else if (key == "own_view") {
            settings.own_view_for_everyone = (value != "0");
        } else if (key == "name") {
            settings.name = clean_text(value, 16);
        }
    }

    if (settings.name.empty()) {
        settings.name = read_old_name_file(folder);
    }

    return settings;
}

void save_settings(const std::filesystem::path& folder, const Settings& settings) {
    try {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);

        std::filesystem::path tmp_path = folder / "netplay_settings.txt.tmp";
        std::ofstream tmp_file(tmp_path, std::ios::binary);

        if (!tmp_file) {
            std::filesystem::remove(tmp_path, ec);
            return;
        }

        tmp_file << "port=" << std::clamp(settings.port, 1, 65535) << "\n";
        tmp_file << "players=" << std::clamp(settings.players, 2, 4) << "\n";
        tmp_file << "address=" << clean_text(settings.address, 64) << "\n";
        tmp_file << "own_view=" << (settings.own_view_for_everyone ? "1" : "0") << "\n";
        tmp_file << "name=" << clean_text(settings.name, 16) << "\n";

        tmp_file.close();

        if (tmp_file.fail()) {
            std::filesystem::remove(tmp_path, ec);
            return;
        }

        std::filesystem::rename(tmp_path, folder / "netplay_settings.txt", ec);
        if (ec) {
            std::filesystem::remove(tmp_path, ec);
        }
    } catch (...) {
        // Ignore errors
    }
}

} // namespace conker::netplay_helpers
