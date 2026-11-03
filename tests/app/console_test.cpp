// Serial console (connectivity spec 8.2): command parsing, the Wi-Fi
// commands against a FakeWifi on a headless App, and the console task fed
// through Platform::read_line.

#include "wisp/app/console.hpp"

#include <gtest/gtest.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_wifi.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/core/log.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

using wisp::app::App;
using wisp::app::LineRead;
using wisp::app::run_command;
using wisp::app::split_command;
using wisp::test::FakeStorage;
using wisp::test::FakeWifi;
using wisp::test::wait_until;

namespace {

class ConsoleBoard : public wisp::app::Board {
public:
    ConsoleBoard(std::string root, std::shared_ptr<FakeWifi> wifi)
        : root_(std::move(root)), wifi_(std::move(wifi)) {}

    std::string name() const override { return "fake"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport&) override {
        registry.add(std::make_shared<FakeStorage>("storage.int", root_, /*removable=*/false));
        if (wifi_) registry.add(wifi_);
    }

private:
    std::string root_;
    std::shared_ptr<FakeWifi> wifi_;
};

// Lines a test feeds to Platform::read_line; close() ends the input.
class LineFeed {
public:
    void push(std::string line) {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(std::move(line));
        cv_.notify_all();
    }
    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }
    LineRead read(std::string& line, uint32_t timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                     [this] { return closed_ || !lines_.empty(); });
        if (!lines_.empty()) {
            line = std::move(lines_.front());
            lines_.pop_front();
            return LineRead::Line;
        }
        return closed_ ? LineRead::Closed : LineRead::Timeout;
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::string> lines_;
    bool closed_ = false;
};

struct ConsoleOptions {
    bool with_wifi = true;
    bool with_reader = false;
    std::vector<wisp::app::ConsoleCommand> extra;
};

struct ConsoleHarness {
    std::string root = wisp::test::temp_dir().string();
    std::shared_ptr<FakeWifi> wifi;
    std::shared_ptr<LineFeed> feed = std::make_shared<LineFeed>();
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    std::unique_ptr<App> app;

    explicit ConsoleHarness(ConsoleOptions opts = {}) {
        if (opts.with_wifi) {
            wifi = std::make_shared<FakeWifi>("net.wifi");
            wifi->networks = {{"Home", -50, true}, {"My Home", -60, true}};
            wifi->passwords = {{"Home", "password1"}};
        }
        wisp::app::Platform platform;
        platform.board = std::make_unique<ConsoleBoard>(root, wifi);
        platform.settings =
            std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(root + "/settings.json");
        platform.emit_line = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(lines_mutex);
            lines.push_back(line);
        };
        platform.random = [](uint8_t* out, size_t len) {
            for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(i * 37 + 11);
        };
        if (opts.with_reader) {
            platform.read_line = [feed = feed](std::string& line, uint32_t timeout_ms) {
                return feed->read(line, timeout_ms);
            };
        }
        platform.console_commands = std::move(opts.extra);
        app = std::make_unique<App>(std::move(platform));
    }

    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lock(lines_mutex);
        return lines;
    }

    bool emitted(const std::string& want) {
        for (const auto& l : snapshot()) {
            if (l == want) return true;
        }
        return false;
    }
};

std::vector<std::string> words(std::string_view line) { return split_command(line).value(); }

}  // namespace

TEST(Console, SplitsQuotedSsid) {
    EXPECT_EQ(words(R"(wifi add "My Home" "p@ss word")"),
              (std::vector<std::string>{"wifi", "add", "My Home", "p@ss word"}));
    EXPECT_EQ(words("  wifi   status  "), (std::vector<std::string>{"wifi", "status"}));
    EXPECT_EQ(words(R"(wifi add "say \"hi\"" a\b)"),
              (std::vector<std::string>{"wifi", "add", "say \"hi\"", "a\\b"}));
    EXPECT_EQ(words(R"(x "a\\")"), (std::vector<std::string>{"x", "a\\"}));
    EXPECT_EQ(words(R"(x "")"), (std::vector<std::string>{"x", ""}));
    EXPECT_EQ(words(R"(x ab"c d"e)"), (std::vector<std::string>{"x", "abc de"}));
    EXPECT_TRUE(words("   ").empty());
}

TEST(Console, UnterminatedQuoteIsError) {
    auto r = split_command(R"(wifi add "My Home secret)");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message.find("secret"), std::string::npos);

    ConsoleHarness h;
    ASSERT_TRUE(h.app->start());
    const std::string out = run_command(*h.app, R"(wifi add Home "secret pass)");
    EXPECT_EQ(out.rfind("error: ", 0), 0u) << out;
    EXPECT_EQ(out.find("secret"), std::string::npos);
}

TEST(Console, AddNeverEchoesPassword) {
    wisp::log::init();
    ConsoleHarness h;
    ASSERT_TRUE(h.app->start());
    const std::string out = run_command(*h.app, R"(wifi add "My Home" "p@ss word 9")");
    EXPECT_EQ(out, "ok");
    ASSERT_EQ(h.app->wifi()->saved_ssids(), (std::vector<std::string>{"My Home"}));

    // A password the service rejects (too short) must not leak through
    // the error either.
    const std::string bad = run_command(*h.app, "wifi add Home s3cr");
    EXPECT_EQ(bad.rfind("error: ", 0), 0u) << bad;
    EXPECT_EQ(bad.find("s3cr"), std::string::npos);

    const std::string status = run_command(*h.app, "wifi status");
    const std::string ring = wisp::log::ring_snapshot();
    for (const std::string& secret : {std::string("p@ss word 9"), std::string("s3cr")}) {
        EXPECT_EQ(out.find(secret), std::string::npos);
        EXPECT_EQ(status.find(secret), std::string::npos);
        EXPECT_EQ(ring.find(secret), std::string::npos) << ring;
    }
}

TEST(Console, AddAcceptsHexSsidAndOpenNetwork) {
    ConsoleHarness h;
    ASSERT_TRUE(h.app->start());
    EXPECT_EQ(run_command(*h.app, "wifi add hex:c4e3bac3 password1"), "ok");
    EXPECT_EQ(run_command(*h.app, "wifi add Cafe"), "ok");  // open network
    EXPECT_EQ(h.app->wifi()->saved_ssids(),
              (std::vector<std::string>{std::string("\xc4\xe3\xba\xc3"), "Cafe"}));
    EXPECT_EQ(run_command(*h.app, "wifi add hex:c4e").rfind("error: ", 0), 0u);
    EXPECT_EQ(run_command(*h.app, "wifi add hex:zz").rfind("error: ", 0), 0u);
    EXPECT_EQ(run_command(*h.app, "wifi add").rfind("error: ", 0), 0u);
    EXPECT_EQ(run_command(*h.app, "wifi forget hex:c4e3bac3"), "ok");
    EXPECT_EQ(h.app->wifi()->saved_ssids(), (std::vector<std::string>{"Cafe"}));
}

TEST(Console, StatusEscapesSsidAndListsSaved) {
    ConsoleHarness h;
    ASSERT_TRUE(h.app->start());
    EXPECT_EQ(run_command(*h.app, "wifi status"), "state=off ssid=\"\" ip= rssi=0");

    // GBK bytes (not UTF-8), a control byte, a quote and a backslash.
    ASSERT_TRUE(
        h.app->wifi()->add_network(std::string("\xc4\xe3\x01q\"b\\"), "", wisp::os::now_ms()));
    ASSERT_TRUE(h.app->wifi()->add_network("\xe4\xbd\xa0\xe5\xa5\xbd", "", wisp::os::now_ms()));
    const std::string out = run_command(*h.app, "wifi status");
    EXPECT_NE(out.find(R"(saved ssid="\xc4\xe3\x01q\"b\\")"), std::string::npos) << out;
    // Valid UTF-8 stays as typed.
    EXPECT_NE(out.find("saved ssid=\"\xe4\xbd\xa0\xe5\xa5\xbd\""), std::string::npos) << out;
    for (char c : out) {
        const auto u = static_cast<unsigned char>(c);
        EXPECT_TRUE(c == '\n' || (u >= 0x20 && u != 0x7f)) << static_cast<int>(u);
    }
}

TEST(Console, StatusShowsReasonAndAuthFailed) {
    ConsoleHarness h;
    ASSERT_TRUE(h.app->start());
    ASSERT_EQ(run_command(*h.app, "wifi add Home wrongpass"), "ok");
    ASSERT_TRUE(wait_until(
        [&] { return h.app->wifi()->status().state == wisp::net::WifiState::Backoff; }, 3000));
    const std::string out = run_command(*h.app, "wifi status");
    EXPECT_EQ(out.rfind("state=backoff ssid=\"Home\" ip= rssi=0 reason=auth_failed\n", 0), 0u)
        << out;
    EXPECT_NE(out.find("saved ssid=\"Home\" auth_failed"), std::string::npos) << out;
    EXPECT_EQ(out.find("wrongpass"), std::string::npos);

    ASSERT_EQ(run_command(*h.app, "wifi add Home password1"), "ok");
    ASSERT_TRUE(wait_until(
        [&] { return h.app->wifi()->status().state == wisp::net::WifiState::Connected; }, 3000));
    EXPECT_EQ(run_command(*h.app, "wifi status"),
              "state=connected ssid=\"Home\" ip=192.168.1.50 rssi=-50\nsaved ssid=\"Home\"");
}

TEST(Console, ForgetUnknownIsError) {
    ConsoleHarness h;
    ASSERT_TRUE(h.app->start());
    EXPECT_EQ(run_command(*h.app, "wifi forget Nowhere"), "error: network is not saved");
    EXPECT_EQ(run_command(*h.app, "wifi forget").rfind("error: ", 0), 0u);
}

TEST(Console, ApOnDoesNotPrintPassword) {
    ConsoleHarness h;
    ASSERT_TRUE(h.app->start());
    const std::string out = run_command(*h.app, "wifi ap on");
    auto session = h.app->wifi()->ap_session();
    ASSERT_TRUE(session.has_value());
    ASSERT_FALSE(session->password.empty());
    EXPECT_EQ(out, "ap ssid=\"" + session->ssid + "\" ip=192.168.4.1");
    EXPECT_EQ(out.find(session->password), std::string::npos);

    const std::string status = run_command(*h.app, "wifi status");
    EXPECT_NE(status.find("ap ssid=\"" + session->ssid + "\" clients=0"), std::string::npos)
        << status;
    EXPECT_EQ(status.find(session->password), std::string::npos);

    EXPECT_EQ(run_command(*h.app, "wifi ap off"), "ok");
    EXPECT_FALSE(h.app->wifi()->ap_session().has_value());
    EXPECT_EQ(run_command(*h.app, "wifi ap sideways").rfind("error: ", 0), 0u);
}

TEST(Console, UnknownCommand) {
    ConsoleHarness h;
    ASSERT_TRUE(h.app->start());
    EXPECT_EQ(run_command(*h.app, "hunter2"), "error: unknown command");
    EXPECT_EQ(run_command(*h.app, "wifi dance"), "error: unknown command");
    EXPECT_EQ(run_command(*h.app, "tap 10,10"), "error: unknown command");
    EXPECT_EQ(run_command(*h.app, ""), "");
}

TEST(Console, WithoutWifiReportsUnavailable) {
    ConsoleHarness h({.with_wifi = false});
    ASSERT_TRUE(h.app->start());
    EXPECT_EQ(run_command(*h.app, "wifi status"), "error: Wi-Fi unavailable");
}

TEST(Console, HelpListsCommandsAndPlatformExtras) {
    ConsoleHarness h({.extra = {{"debug", "debug ping            reply pong",
                                 [](App&, const std::vector<std::string>& args) {
                                     return args.size() == 2 && args[1] == "ping"
                                                ? std::string("pong")
                                                : std::string("error: usage: debug ping");
                                 }},
                                // Built-in names cannot be taken over.
                                {"wifi", "wifi hijack", [](App&, const std::vector<std::string>&) {
                                     return std::string("hijacked");
                                 }}}});
    ASSERT_TRUE(h.app->start());
    const std::string help = run_command(*h.app, "help");
    for (const char* want : {"wifi status", "wifi add <ssid> [password]", "wifi forget <ssid>",
                             "wifi ap on", "hex:", "debug ping"}) {
        EXPECT_NE(help.find(want), std::string::npos) << want;
    }
    EXPECT_EQ(help.find("hijack"), std::string::npos);
    EXPECT_EQ(run_command(*h.app, "debug ping"), "pong");
    EXPECT_EQ(run_command(*h.app, "wifi status").find("hijacked"), std::string::npos);
}

TEST(Console, TaskRunsLinesFromReadLine) {
    wisp::log::init();
    ConsoleHarness h({.with_reader = true});
    ASSERT_TRUE(h.app->start());
    h.feed->push("wifi add Home password9");
    h.feed->push("bogus");
    ASSERT_TRUE(wait_until([&] { return h.emitted("error: unknown command"); }, 3000));
    EXPECT_TRUE(h.emitted("ok"));
    EXPECT_EQ(h.app->wifi()->saved_ssids(), (std::vector<std::string>{"Home"}));
    h.feed->push("wifi status");
    ASSERT_TRUE(wait_until([&] { return h.emitted("saved ssid=\"Home\""); }, 3000));
    for (const auto& l : h.snapshot()) EXPECT_EQ(l.find("password9"), std::string::npos) << l;
    EXPECT_EQ(wisp::log::ring_snapshot().find("password9"), std::string::npos);
}

TEST(Console, StopDoesNotWaitForInput) {
    ConsoleHarness h({.with_reader = true});
    ASSERT_TRUE(h.app->start());
    const uint64_t t0 = wisp::os::now_ms();
    h.app->stop();  // read_line keeps timing out; stop() must not hang
    EXPECT_LT(wisp::os::now_ms() - t0, 2000u);
    // And the console comes back after a restart.
    ASSERT_TRUE(h.app->start());
    h.feed->push("wifi ap off");
    EXPECT_TRUE(wait_until([&] { return h.emitted("ok"); }, 3000));
}

TEST(Console, ClosedInputEndsTheTask) {
    ConsoleHarness h({.with_reader = true});
    ASSERT_TRUE(h.app->start());
    h.feed->close();
    wisp::os::sleep_ms(100);
    h.app->stop();
    SUCCEED();
}
