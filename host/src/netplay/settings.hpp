#pragma once

#include <filesystem>
#include <string>

namespace conker::netplay_helpers {
    struct Settings {
        int port = 27015;
        int players = 2;
        std::string address;
        bool own_view_for_everyone = true;
        std::string name;
    };
    Settings load_settings(const std::filesystem::path& folder);
    void save_settings(const std::filesystem::path& folder, const Settings& settings);
}
