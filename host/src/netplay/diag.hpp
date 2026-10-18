#pragma once

#include <cstddef>
#include <string>

namespace conker::netplay_helpers {
    void note(const std::string& line);
    std::string recent(std::size_t max_lines = 60);
    void clear();
}
