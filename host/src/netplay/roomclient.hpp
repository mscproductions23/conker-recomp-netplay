#pragma once
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
namespace conker::netplay_rooms {
struct Http { int status = 0; std::string body; std::string error; };
Http post(const std::string& url, const std::string& body, int timeout_seconds = 6);
struct Lookup { bool ok = false; std::string error; std::string key_hex; int port = 0; std::vector<std::string> addrs; };
Lookup lookup(const std::string& server, const std::string& code);
class Room {
public:
    enum class State { idle, opening, open, failed };
    Room();
    ~Room();                 // calls close()
    Room(const Room&) = delete;
    Room& operator=(const Room&) = delete;
    void open(const std::string& server, const std::string& key_hex, int port, const std::vector<std::string>& addrs);
    void close();
    State state() const;
    std::string code() const;
    std::string public_address() const;
    std::string error() const;
private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::thread thread_;
    State state_ = State::idle;
    std::string server_, key_hex_, code_, token_, public_, error_;
    int port_ = 0;
    std::vector<std::string> addrs_;
    bool stop_ = false;
    int failures_ = 0;

    void run();
};

}  // namespace conker::netplay_rooms
