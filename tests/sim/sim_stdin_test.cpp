// The simulator's single stdin reader (simulator/sim_stdin.hpp), fed
// through a pipe: touch lines go to the touch callback, everything else to
// the console queue, and shutdown never waits on an idle input.

#include "sim_stdin.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using wisp::app::LineRead;

namespace {

struct Pipe {
    int fds[2] = {-1, -1};
    Pipe() { EXPECT_EQ(::pipe(fds), 0); }
    ~Pipe() {
        close_write();
        if (fds[0] >= 0) ::close(fds[0]);
    }
    void write(const std::string& s) {
        ASSERT_EQ(::write(fds[1], s.data(), s.size()), static_cast<ssize_t>(s.size()));
    }
    void close_write() {
        if (fds[1] >= 0) ::close(fds[1]);
        fds[1] = -1;
    }
};

struct Touches {
    std::mutex mutex;
    std::vector<std::string> lines;
    void add(const std::string& l) {
        std::lock_guard<std::mutex> lock(mutex);
        lines.push_back(l);
    }
    std::vector<std::string> get() {
        std::lock_guard<std::mutex> lock(mutex);
        return lines;
    }
};

std::vector<std::string> drain(ConsoleLines& q) {
    std::vector<std::string> out;
    std::string line;
    while (true) {
        const LineRead r = q.read(line, 2000);
        if (r != LineRead::Line) {
            EXPECT_EQ(r, LineRead::Closed);
            return out;
        }
        out.push_back(line);
    }
}

}  // namespace

TEST(SimStdin, ClassifiesTouchLines) {
    EXPECT_TRUE(is_touch_line("tap 1,2"));
    EXPECT_TRUE(is_touch_line("swipe 1,2,3,4"));
    EXPECT_TRUE(is_touch_line("tap"));
    EXPECT_TRUE(is_touch_line("tap nonsense"));
    EXPECT_TRUE(is_touch_line("tap\t1,2"));
    EXPECT_TRUE(is_touch_line("swipe\t1,2,3,4"));
    EXPECT_FALSE(is_touch_line("tap\x01 1,2"));
    EXPECT_FALSE(is_touch_line("tapx 1,2"));
    EXPECT_FALSE(is_touch_line(" tap 1,2"));
    EXPECT_FALSE(is_touch_line("wifi status"));
    EXPECT_FALSE(is_touch_line("help"));
}

TEST(SimStdin, RoutesLinesAndClosesAtEof) {
    Pipe p;
    ConsoleLines console;
    Touches touches;
    StdinReader reader(p.fds[0], [&](const std::string& l) { touches.add(l); }, console);
    p.write("tap 188,298\nwifi status\r\n\nswipe 1,2,");
    p.write("3,4\nwifi add \"My Home\" pw\nhelp");  // last line has no newline
    p.close_write();
    EXPECT_EQ(drain(console),
              (std::vector<std::string>{"wifi status", "wifi add \"My Home\" pw", "help"}));
    EXPECT_EQ(touches.get(), (std::vector<std::string>{"tap 188,298", "swipe 1,2,3,4"}));
}

TEST(SimStdin, DropsOverlongLines) {
    Pipe p;
    ConsoleLines console;
    StdinReader reader(p.fds[0], nullptr, console);
    p.write(std::string(StdinReader::kMaxLine + 10, 'x') + "\nhelp\n");
    p.close_write();
    EXPECT_EQ(drain(console), (std::vector<std::string>{"help"}));
}

TEST(SimStdin, ReadTimesOutWhileInputIsIdle) {
    Pipe p;
    ConsoleLines console;
    std::string line;
    auto reader = std::make_unique<StdinReader>(p.fds[0], nullptr, console);
    EXPECT_EQ(console.read(line, 50), LineRead::Timeout);
    // Destroying the reader while the input stays open must not hang.
    const auto t0 = std::chrono::steady_clock::now();
    reader.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(1000));
    // Stopping the reader also ends the console's input.
    EXPECT_EQ(console.read(line, 50), LineRead::Closed);
}

// A terminal that is not this process's controlling terminal (job control
// only stops background readers of the controlling one) is read like a
// pipe.
TEST(SimStdin, ReadsATerminal) {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT_GE(master, 0);
    ASSERT_EQ(::grantpt(master), 0);
    ASSERT_EQ(::unlockpt(master), 0);
    const int slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
    ASSERT_GE(slave, 0);
    ASSERT_TRUE(::isatty(slave));
    {
        ConsoleLines console;
        StdinReader reader(slave, nullptr, console);
        const std::string input = "help\n";
        ASSERT_EQ(::write(master, input.data(), input.size()), static_cast<ssize_t>(input.size()));
        std::string line;
        ASSERT_EQ(console.read(line, 2000), LineRead::Line);
        EXPECT_EQ(line, "help");
    }
    ::close(slave);
    ::close(master);
}

// A terminal that hangs up (here: its master side closes) ends the input.
// Linux reports this hangup as end of file; an EIO from a terminal we are
// not in the background of is treated the same way.
TEST(SimStdin, TerminalHangupClosesTheInput) {
    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    ASSERT_GE(master, 0);
    ASSERT_EQ(::grantpt(master), 0);
    ASSERT_EQ(::unlockpt(master), 0);
    const int slave = ::open(::ptsname(master), O_RDWR | O_NOCTTY);
    ASSERT_GE(slave, 0);
    {
        ConsoleLines console;
        StdinReader reader(slave, nullptr, console);
        ::close(master);
        std::string line;
        EXPECT_EQ(console.read(line, 2000), LineRead::Closed);
    }
    ::close(slave);
}
