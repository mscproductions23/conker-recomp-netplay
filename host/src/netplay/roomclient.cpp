#include "roomclient.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace conker::netplay_rooms {

namespace {
    std::string trim(const std::string& s) {
        const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        size_t first = 0;
        size_t last = s.size();
        while (first < last && blank(s[first])) first++;
        while (last > first && blank(s[last - 1])) last--;
        return s.substr(first, last - first);
    }

    bool kv_find(const std::string& body, const std::string& key, std::string& value) {
        std::string::size_type start = 0;
        while (start <= body.size()) {
            std::string::size_type end = body.find('\n', start);
            if (end == std::string::npos) end = body.size();
            std::string line = trim(body.substr(start, end - start));
            if (!line.empty()) {
                auto eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string k = trim(line.substr(0, eq));
                    if (k == key) {
                        value = trim(line.substr(eq + 1));
                        return true;
                    }
                }
            }
            start = end + 1;
        }
        return false;
    }

    std::vector<std::string> kv_all(const std::string& body, const std::string& key) {
        std::vector<std::string> values;
        std::string::size_type start = 0;
        while (start <= body.size()) {
            std::string::size_type end = body.find('\n', start);
            if (end == std::string::npos) end = body.size();
            std::string line = trim(body.substr(start, end - start));
            if (!line.empty()) {
                auto eq = line.find('=');
                if (eq != std::string::npos) {
                    std::string k = trim(line.substr(0, eq));
                    if (k == key) {
                        values.push_back(trim(line.substr(eq + 1)));
                    }
                }
            }
            start = end + 1;
        }
        return values;
    }
}


namespace {

#ifdef _WIN32

static void append_quoted(std::wstring& cmd, const std::wstring& arg) {
    cmd.push_back(L' ');
    cmd.push_back(L'"');

    size_t len = arg.length();
    for (size_t i = 0; i < len; ) {
        size_t backslash_run = 0;

        // Count the run of backslashes
        while (i < len && arg[i] == L'\\') {
            backslash_run++;
            i++;
        }

        if (i < len) {
            if (arg[i] == L'"') {
                // Backslash run followed by a double quote
                for (size_t j = 0; j < 2 * backslash_run + 1; j++) {
                    cmd.push_back(L'\\');
                }
                cmd.push_back(L'"');
            } else {
                // Backslash run followed by any other character
                for (size_t j = 0; j < backslash_run; j++) {
                    cmd.push_back(L'\\');
                }
                cmd.push_back(arg[i]);
            }
        } else {
            // Backslash run at the end of the argument
            for (size_t j = 0; j < 2 * backslash_run; j++) {
                cmd.push_back(L'\\');
            }
        }
        i++;
    }

    cmd.push_back(L'"');
}




int run_program(const std::vector<std::string>& argv_in, const std::string& input, std::string& out) {
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    std::wstring exe = std::wstring(sysdir) + L"\\curl.exe";
    std::wstring cmd = L"\"" + exe + L"\"";

    for (size_t i = 1; i < argv_in.size(); ++i) {
        std::wstring warg;
        for (char ch : argv_in[i]) {
            warg.push_back(static_cast<wchar_t>(static_cast<unsigned char>(ch)));
        }
        append_quoted(cmd, warg);
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE in_read = nullptr, in_write = nullptr, out_read = nullptr, out_write = nullptr, nul = nullptr;
    PPROC_THREAD_ATTRIBUTE_LIST attribute_list = nullptr;

    if (!CreatePipe(&in_read, &in_write, &sa, 0)) { return 1; }
    if (!CreatePipe(&out_read, &out_write, &sa, 0)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        return 1;
    }
    SetHandleInformation(in_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read, HANDLE_FLAG_INHERIT, 0);
    nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    if (nul == INVALID_HANDLE_VALUE) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        CloseHandle(out_read);
        CloseHandle(out_write);
        return 1;
    }

    STARTUPINFOEXW siex{};
    siex.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    SIZE_T attr_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
    std::vector<unsigned char> buffer(attr_size);
    if (!InitializeProcThreadAttributeList(reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(buffer.data()), 1, 0, &attr_size)) {
        CloseHandle(in_read);
        CloseHandle(in_write);
        CloseHandle(out_read);
        CloseHandle(out_write);
        CloseHandle(nul);
        return 1;
    }
    attribute_list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());

    HANDLE child_handles[3] = {in_read, out_write, nul};
    if (!UpdateProcThreadAttribute(attribute_list, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST, child_handles, sizeof(child_handles), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attribute_list);
        CloseHandle(in_read);
        CloseHandle(in_write);
        CloseHandle(out_read);
        CloseHandle(out_write);
        CloseHandle(nul);
        return 1;
    }

    siex.lpAttributeList = attribute_list;
    siex.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    siex.StartupInfo.hStdInput = in_read;
    siex.StartupInfo.hStdOutput = out_write;
    siex.StartupInfo.hStdError = nul;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                        &siex.StartupInfo, &pi)) {
        DeleteProcThreadAttributeList(attribute_list);
        CloseHandle(in_read);
        CloseHandle(in_write);
        CloseHandle(out_read);
        CloseHandle(out_write);
        CloseHandle(nul);
        return 1;
    }

    DeleteProcThreadAttributeList(attribute_list);
    CloseHandle(in_read);
    CloseHandle(out_write);
    CloseHandle(nul);

    DWORD written = 0;
    const char* data = input.c_str();
    size_t remaining = input.size();
    while (remaining > 0) {
        DWORD n = 0;
        if (!WriteFile(in_write, data + written, static_cast<DWORD>(std::min(remaining, static_cast<size_t>(65536))), &n, nullptr)) break;
        written += n;
        remaining -= n;
    }
    CloseHandle(in_write);

    const size_t max_output = 8192;
    bool too_big = false;
    char buf[1024];
    out.clear();
    while (true) {
        DWORD n = 0;
        if (!ReadFile(out_read, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        if (out.size() + n > max_output) { too_big = true; n = static_cast<DWORD>(max_output - out.size()); }
        out.append(buf, n);
    }
    CloseHandle(out_read);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    if (too_big) return 3;
    return code == 0 ? 0 : 2;
}

#else
int run_program(const std::vector<std::string>& argv_in, const std::string& input, std::string& out) {
    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) == -1 || pipe(out_pipe) == -1) return 1;
    fcntl(in_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(out_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(in_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, in_pipe[0], 0);
    posix_spawn_file_actions_adddup2(&actions, out_pipe[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    std::vector<char*> argv_ptrs(argv_in.size() + 1);
    for (size_t i = 0; i < argv_in.size(); ++i) argv_ptrs[i] = const_cast<char*>(argv_in[i].c_str());
    argv_ptrs.back() = nullptr;
    pid_t pid;
    int result = posix_spawnp(&pid, argv_in[0].c_str(), &actions, nullptr, argv_ptrs.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (result != 0) { close(in_pipe[1]); close(out_pipe[0]); return 1; }
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, &old);
    size_t written = 0;
    while (written < input.size()) {
        ssize_t w = write(in_pipe[1], input.data() + written, input.size() - written);
        if (w == -1) { if (errno == EINTR) continue; break; }
        written += static_cast<size_t>(w);
    }
    close(in_pipe[1]);
    sigset_t pending;
    sigpending(&pending);
    if (sigismember(&pending, SIGPIPE)) { int sig; sigwait(&set, &sig); }
    pthread_sigmask(SIG_SETMASK, &old, nullptr);
    bool too_big = false;
    char buffer[8192];
    ssize_t total_read = 0;
    while (true) {
        ssize_t r = read(out_pipe[0], buffer, sizeof(buffer));
        if (r == -1) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        if (total_read + r > 8192) { too_big = true; continue; }
        out.append(buffer, r);
        total_read += r;
    }
    close(out_pipe[0]);
    int status;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR);
    if (too_big) return 3;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
    return 2;
}

#endif

}  // namespace

Http post(const std::string& url, const std::string& body, int timeout_seconds) {
    if (!url.starts_with("http://") && !url.starts_with("https://")) {
        return Http{0, "", "That server address isn't valid."};
    }
    for (char c : url) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x21 || uc > 0x7E || c == '"' || c == '\'' || c == '\\') {
            return Http{0, "", "That server address isn't valid."};
        }
    }

    std::vector<std::string> argv = {
        "curl", "-q", "-sS", "--proto", "=http,https", "--max-time",
        std::to_string(timeout_seconds), "-X", "POST", "-H",
        "Content-Type: text/plain", "--data-binary", "@-", "-w",
        "\\n%{http_code}", url
    };

    std::string out;
    int rc = run_program(argv, body, out);

    if (rc == 1) {
        return Http{0, "", "curl isn't installed or couldn't be started."};
    } else if (rc == 3) {
        return Http{0, "", "The room server sent too much data."};
    } else if (rc == 2) {
        return Http{0, "", "Couldn't reach the room server."};
    }

    std::string::size_type pos = out.rfind('\n');
    if (pos == std::string::npos) {
        return Http{0, "", "Couldn't reach the room server."};
    }

    std::string reply_body = out.substr(0, pos);
    std::string status_text = trim(out.substr(pos + 1));
    if (status_text.length() != 3 || !std::isdigit(status_text[0]) || !std::isdigit(status_text[1]) || !std::isdigit(status_text[2])) {
        return Http{0, "", "Couldn't reach the room server."};
    }

    int status = (status_text[0] - '0') * 100 + (status_text[1] - '0') * 10 + (status_text[2] - '0');
    Http h;
    h.status = status;
    h.body = reply_body;

    if (status >= 200 && status < 300) {
        h.error = "";
    } else if (status == 404) {
        h.error = "No room has that code (it may have closed).";
    } else if (status == 429) {
        h.error = "Too many tries: wait a minute.";
    } else if (status == 403) {
        h.error = "The room server refused the room's token.";
    } else if (status >= 500 && status < 600) {
        h.error = "The room server had a problem.";
    } else if (status >= 400 && status < 500) {
        h.error = "The room server rejected the request.";
    } else {
        h.error = "Couldn't reach the room server.";
    }

if ((status >= 500 && status < 600) || (status >= 400 && status < 500 && status != 403 && status != 404 && status != 429)) {
    std::string msg;
    if (kv_find(h.body, "error", msg) && !msg.empty()) {
        h.error += " " + msg;
    }
}

    return h;
}


Lookup lookup(const std::string& server, const std::string& code) {
    std::string base = server;
    if (!base.empty() && base.back() == '/') {
        base.pop_back();
    }

    Http h = post(base + "/v1/join", "code=" + code + "\n", 6);
    if (!h.error.empty()) {
        return Lookup{false, h.error, "", 0, {}};
    }

    std::string key;
    if (!kv_find(h.body, "key", key) || key.size() != 16 || !std::all_of(key.begin(), key.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c));
    })) {
        return Lookup{false, "The room server sent an unexpected reply.", "", 0, {}};
    }
    std::transform(key.begin(), key.end(), key.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });

    std::string port_text;
    if (!kv_find(h.body, "port", port_text) || port_text.empty() || !std::all_of(port_text.begin(), port_text.end(), ::isdigit)) {
        return Lookup{false, "The room server sent an unexpected reply.", "", 0, {}};
    }

    int port = 0;
    for (char c : port_text) {
        port = port * 10 + (c - '0');
    }

    if (port <= 0 || port > 65535) {
        return Lookup{false, "The room server sent an unexpected reply.", "", 0, {}};
    }

    std::vector<std::string> addrs = kv_all(h.body, "addr");

    return Lookup{true, "", key, port, addrs};
}





Room::Room() {}

Room::~Room() {
    close();
}

void Room::open(const std::string& server, const std::string& key_hex, int port, const std::vector<std::string>& addrs) {
    close();

    std::lock_guard<std::mutex> lk(mu_);
    server_ = server;
    if (!server_.empty() && server_.back() == '/') {
        server_.pop_back();
    }
    key_hex_ = key_hex;
    port_ = port;
    addrs_ = addrs;
    state_ = State::opening;
    error_.clear();
    code_.clear();
    token_.clear();
    public_.clear();
    stop_ = false;
    failures_ = 0;

    thread_ = std::thread(&Room::run, this);
}

void Room::close() {
    { std::lock_guard<std::mutex> g(mu_); stop_ = true; }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::string server_copy, code_copy, token_copy, kept_error;
    bool registered = false;
    bool was_failed = false;
    { std::lock_guard<std::mutex> g(mu_); server_copy = server_; code_copy = code_; token_copy = token_; registered = !code_copy.empty() && !token_copy.empty(); was_failed = (state_ == State::failed); if (was_failed) kept_error = error_; }
    if (registered) { post(server_copy + "/v1/close", "code=" + code_copy + "\ntoken=" + token_copy + "\n", 3); }
    { std::lock_guard<std::mutex> g(mu_); code_.clear(); token_.clear(); public_.clear(); state_ = State::idle; error_ = kept_error; stop_ = false; }
}

Room::State Room::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

std::string Room::code() const {
    std::lock_guard<std::mutex> lk(mu_);
    return code_;
}

std::string Room::public_address() const {
    std::lock_guard<std::mutex> lk(mu_);
    return public_;
}

std::string Room::error() const {
    std::lock_guard<std::mutex> lk(mu_);
    return error_;
}

void Room::run() {
    std::string server;
    std::string body;
    {
        std::lock_guard<std::mutex> lk(mu_);
        server = server_;
        body = "key=" + key_hex_ + "\nport=" + std::to_string(port_) + "\n";
        for (const auto& addr : addrs_) {
            body += "addr=" + addr + "\n";
        }
    }

    Http h = post(server + "/v1/host", body);

    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!h.error.empty()) {
            state_ = State::failed;
            error_ = h.error;
            return;
        }
    }

    std::string c, t, p;
    if (kv_find(h.body, "code", c) && kv_find(h.body, "token", t) && kv_find(h.body, "public", p)) {
        if (c.size() != 6 || t.empty()) {
            std::lock_guard<std::mutex> lk(mu_);
            state_ = State::failed;
            error_ = "The room server sent an unexpected reply.";
            return;
        }
        std::lock_guard<std::mutex> lk(mu_);
        code_ = c;
        token_ = t;
        public_ = p;
        state_ = State::open;
    } else {
        std::lock_guard<std::mutex> lk(mu_);
        state_ = State::failed;
        error_ = "The room server sent an unexpected reply.";
        return;
    }

    std::unique_lock<std::mutex> lk(mu_);
    while (!stop_) {
        if (cv_.wait_for(lk, std::chrono::seconds(30), [this]{ return stop_; })) {
            break;
        }

        std::string server = server_;
        std::string code = code_;
        std::string token = token_;
        lk.unlock();

        Http b = post(server + "/v1/beat", "code=" + code + "\ntoken=" + token + "\n");

        lk.lock();
        if (stop_) {
            break;
        }

        if (b.error.empty()) {
            failures_ = 0;
        } else {
            ++failures_;
            error_ = b.error;
            if (failures_ >= 3) {
                state_ = State::failed;
                return;
            }
        }
    }
}




}  // namespace conker::netplay_rooms
