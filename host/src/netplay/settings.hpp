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
        std::string room_server;    // the room server's address (http:// or https://), or empty for none
    };
    Settings load_settings(const std::filesystem::path& folder);
    void save_settings(const std::filesystem::path& folder, const Settings& settings);
}
