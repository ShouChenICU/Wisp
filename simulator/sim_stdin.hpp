#pragma once

// The simulator's stdin (spec 8.1; connectivity spec 8.2). One thread --
// StdinReader's -- owns the file descriptor and splits what arrives into
// lines, then routes each:
//   - a touch line ("tap ..." / "swipe ...", is_touch_line()) goes to a
//     callback that only queues it (TouchScript plays it on its own
//     thread), so a slow swipe never holds up reading;
//   - every other non-empty line goes to a ConsoleLines queue, which
//     Platform::read_line pops for the serial console.
// The console therefore never sees a touch line, and no second reader
// competes for the same bytes. At end of input the queue is closed, so
// read_line reports Closed once it is drained.
//
// The reader polls with a short timeout instead of blocking in read(), so
// its destructor returns promptly even while stdin stays open with no
// data (a terminal, or a pipe whose writer is idle).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "wisp/app/app.hpp"

// True when the line's first space-separated word is "tap" or "swipe".
// Such a line is always a touch line, even a malformed one, so a typo in
// a touch command is reported as one rather than reaching the console.
bool is_touch_line(std::string_view line);

// Lines for Platform::read_line. Thread-safe.
class ConsoleLines {
public:
    void push(std::string line);
    // No more lines will come; read() reports Closed once drained.
    void close();
    // Waits up to timeout_ms for a line (see Platform::read_line).
    wisp::app::LineRead read(std::string& line, uint32_t timeout_ms);

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
    bool closed_ = false;
};

class StdinReader {
public:
    // Starts reading `fd` at once. `on_touch` runs on the reader thread
    // and must not block. `console` must outlive this object.
    StdinReader(int fd, std::function<void(const std::string&)> on_touch, ConsoleLines& console);
    // Stops reading and joins; does not close `fd`.
    ~StdinReader();

    StdinReader(const StdinReader&) = delete;
    StdinReader& operator=(const StdinReader&) = delete;

    // A line longer than this is dropped whole (up to its newline).
    static constexpr size_t kMaxLine = 4096;

private:
    void run();
    void route(std::string line);

    const int fd_;
    std::function<void(const std::string&)> on_touch_;
    ConsoleLines& console_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
