#include "diag.hpp"

#include <algorithm>
#include <ctime>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>

namespace conker::netplay_helpers {

namespace {

constexpr std::size_t MAX_LINES = 200;
constexpr std::size_t LINE_LENGTH = 200;
constexpr std::size_t TIMESTAMP_LENGTH = 9;

std::string clean_line(const std::string& line) {
    std::string cleaned;
    cleaned.reserve(LINE_LENGTH);

    // First, truncate to at most LINE_LENGTH characters
    std::size_t length = std::min(line.length(), LINE_LENGTH);

    // Then, map each character
    for (std::size_t i = 0; i < length; ++i) {
        unsigned char c = static_cast<unsigned char>(line[i]);
        if (c >= 0x20 && c <= 0x7E) {
            cleaned.push_back(c);
        } else {
            cleaned.push_back('?');
        }
    }

    return cleaned;
}

std::string format_time(time_t t) {
    char buffer[TIMESTAMP_LENGTH];
    std::tm tm = {};

    #if defined(_WIN32)
    if (localtime_s(&tm, &t) != 0) {
        std::snprintf(buffer, sizeof(buffer), "??:??:??");
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d",
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    #else
    std::tm* tm_ptr = localtime_r(&t, &tm);
    if (tm_ptr == nullptr) {
        std::snprintf(buffer, sizeof(buffer), "??:??:??");
    } else {
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d",
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
    #endif

    return std::string(buffer);
}

std::mutex log_mutex;
std::deque<std::string> entries;

} // namespace

void note(const std::string& line) {
    std::lock_guard<std::mutex> lock(log_mutex);

    try {
        std::string cleaned_line = clean_line(line);
        std::string timestamp = format_time(std::time(nullptr));
        std::string entry = timestamp + " " + cleaned_line;

        if (entries.size() == MAX_LINES) {
            entries.pop_front();
        }

        entries.push_back(entry);
    } catch (...) {
        // Ignore any exceptions
    }
}

std::string recent(std::size_t max_lines) {
    std::lock_guard<std::mutex> lock(log_mutex);

    std::string result;

    try {
        std::size_t size = entries.size();
        std::size_t lines_to_copy = std::min(max_lines, size);

        if (lines_to_copy > 0) {
            for (std::size_t i = 0; i < lines_to_copy; ++i) {
                if (!result.empty()) {
                    result += '\n';
                }
                result += entries[size - lines_to_copy + i];
            }
        }
    } catch (...) {
        // Ignore any exceptions
    }

    return result;
}

void clear() {
    std::lock_guard<std::mutex> lock(log_mutex);

    try {
        entries.clear();
    } catch (...) {
        // Ignore any exceptions
    }
}

} // namespace conker::netplay_helpers
