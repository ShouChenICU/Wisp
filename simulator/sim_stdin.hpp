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
//
// Job control: a process that reads its controlling terminal from a
// background process group (`wisp_sim &`) is stopped by SIGTTIN. While
// the terminal's foreground group is not ours the reader leaves it alone
// and resumes once the job is brought to the foreground (`fg`). The
// simulator also ignores SIGTTIN, which turns the remaining race (sent to
// the background between that check and read()) into an EIO the reader
// retries instead of a stopped process. An EIO while not in the background
// (a hung-up terminal) ends the input like end of file.

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

// True when the line's first word (up to a space or tab) is "tap" or "swipe".
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
    static constexpr int kPollMs = 50;

    void run();
    void route(std::string line);
    // True while fd_ is our controlling terminal and another process
    // group is in its foreground.
    bool in_background() const;

    const int fd_;
    const bool is_tty_;
    std::function<void(const std::string&)> on_touch_;
    ConsoleLines& console_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
