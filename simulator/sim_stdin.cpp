#include "sim_stdin.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <utility>

#include "sim_options.hpp"

bool is_touch_line(std::string_view line) {
    const std::string_view word = line.substr(0, line.find_first_of(kTouchSeparators));
    return word == "tap" || word == "swipe";
}

void ConsoleLines::push(std::string line) {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(std::move(line));
    cv_.notify_all();
}

void ConsoleLines::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.notify_all();
}

wisp::app::LineRead ConsoleLines::read(std::string& line, uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                 [this] { return closed_ || !lines_.empty(); });
    if (!lines_.empty()) {
        line = std::move(lines_.front());
        lines_.pop_front();
        return wisp::app::LineRead::Line;
    }
    return closed_ ? wisp::app::LineRead::Closed : wisp::app::LineRead::Timeout;
}

StdinReader::StdinReader(int fd, std::function<void(const std::string&)> on_touch,
                         ConsoleLines& console)
    : fd_(fd), is_tty_(::isatty(fd) == 1), on_touch_(std::move(on_touch)), console_(console) {
    thread_ = std::thread([this] { run(); });
}

StdinReader::~StdinReader() {
    stop_.store(true);
    if (thread_.joinable()) thread_.join();
}

void StdinReader::route(std::string line) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) return;
    if (is_touch_line(line)) {
        if (on_touch_) on_touch_(line);
    } else {
        console_.push(std::move(line));
    }
}

bool StdinReader::in_background() const {
    if (!is_tty_) return false;
    // -1 (ENOTTY) when fd is a terminal but not our controlling one: job
    // control does not apply to it.
    const pid_t foreground = ::tcgetpgrp(fd_);
    return foreground != -1 && foreground != ::getpgrp();
}

void StdinReader::run() {
    std::string pending;
    bool overlong = false;  // dropping the rest of a line past kMaxLine
    while (!stop_.load()) {
        if (in_background()) {
            ::poll(nullptr, 0, kPollMs);
            continue;
        }
        pollfd pfd{fd_, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, kPollMs);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            if (ready < 0) break;
            continue;
        }
        char buf[512];
        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        // EIO while we are in the background: moved there between the check
        // above and the read (SIGTTIN is ignored, see the header); the next
        // turn waits in the background branch instead of reading again. Any
        // other EIO (a hung-up terminal) falls through to end of input.
        if (n < 0 && errno == EIO && in_background()) continue;
        if (n <= 0) {
            // End of input: a last line without a newline still counts.
            if (!overlong) route(std::move(pending));
            break;
        }
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                if (!overlong) route(std::move(pending));
                pending.clear();
                overlong = false;
            } else if (!overlong) {
                pending += buf[i];
                if (pending.size() > kMaxLine) {
                    pending.clear();
                    overlong = true;
                }
            }
        }
    }
    console_.close();
}
