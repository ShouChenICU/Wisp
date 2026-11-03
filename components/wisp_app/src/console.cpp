#include "wisp/app/console.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>

#include "app_impl.hpp"
#include "console_task.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/format.hpp"
#include "wisp/core/log.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "console";
// How long one read_line call may wait, which bounds how long App::stop()
// waits for the console task.
constexpr uint32_t kReadPollMs = 200;

constexpr const char* kHelp =
    "help                         this list\n"
    "wifi status                  state, saved networks and the setup hotspot\n"
    "wifi add <ssid> [password]   save a network (no password: open) and connect\n"
    "wifi forget <ssid>           delete a saved network\n"
    "wifi ap on|off               open / close the setup hotspot\n"
    "  Quote an SSID or password with spaces: wifi add \"My Home\" \"pass word\".\n"
    "  hex:<bytes> gives an SSID by its bytes, e.g. wifi add hex:c4e3bac3 password.";

bool is_builtin(std::string_view name) { return name == "help" || name == "wifi"; }

void append_hex_escape(std::string& out, uint8_t c) {
    char buf[5];
    std::snprintf(buf, sizeof(buf), "\\x%02x", c);
    out += buf;
}

// Escapes control bytes and invalid UTF-8 as \xNN (and, when `quoted`,
// " and \ as \" and \\), keeping valid UTF-8 text as is.
std::string escape(std::string_view s, bool quoted) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<uint8_t>(s[i]);
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) {
                append_hex_escape(out, c);
            } else if (quoted && (c == '"' || c == '\\')) {
                out += '\\';
                out += static_cast<char>(c);
            } else {
                out += static_cast<char>(c);
            }
            ++i;
            continue;
        }
        const size_t len = utf8_sequence(s, i);
        if (len == 0) {
            append_hex_escape(out, c);
            ++i;
        } else {
            out.append(s.substr(i, len));
            i += len;
        }
    }
    return out;
}

std::string error_line(std::string_view message) { return "error: " + escape(message, false); }

// "hex:<bytes>" -> those bytes; anything else as typed. nullopt for a
// malformed hex form.
std::optional<std::string> parse_ssid(const std::string& arg) {
    constexpr std::string_view kPrefix = "hex:";
    if (arg.compare(0, kPrefix.size(), kPrefix) != 0) return arg;
    const std::string_view hex = std::string_view(arg).substr(kPrefix.size());
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() % 2 != 0) return std::nullopt;
    std::string out;
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = digit(hex[i]);
        const int lo = digit(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

constexpr const char* kBadHex = "error: hex:<bytes> needs an even number of hex digits";

std::string wifi_status(net::WifiService& wifi) {
    const net::WifiStatus st = wifi.status();
    std::string out = std::string("state=") + net::to_string(st.state) +
                      " ssid=" + quote_ssid(st.ssid) + " ip=" + escape(st.ip, false) +
                      " rssi=" + std::to_string(st.rssi);
    if (!st.reason.empty()) out += " reason=" + escape(st.reason, false);
    for (const net::SavedNetwork& n : wifi.saved_networks()) {
        out += "\nsaved ssid=" + quote_ssid(n.ssid);
        if (n.auth_failed) out += " auth_failed";
    }
    // ap_status(), not ap_session(): it carries no password.
    const net::ApStatus ap = wifi.ap_status();
    if (ap.active) {
        out += "\nap ssid=" + quote_ssid(ap.ssid) + " clients=" + std::to_string(ap.clients);
    }
    return out;
}

std::string wifi_command(App& app, const std::vector<std::string>& args) {
    if (args.size() < 2) return "error: unknown command";
    const std::string& sub = args[1];
    if (sub != "status" && sub != "add" && sub != "forget" && sub != "ap") {
        return "error: unknown command";
    }
    net::WifiService* wifi = app.wifi();
    if (wifi == nullptr) return "error: Wi-Fi unavailable";

    if (sub == "status") {
        if (args.size() != 2) return "error: usage: wifi status";
        return wifi_status(*wifi);
    }
    if (sub == "add") {
        if (args.size() != 3 && args.size() != 4) {
            return "error: usage: wifi add <ssid> [password]";
        }
        std::optional<std::string> ssid = parse_ssid(args[2]);
        if (!ssid) return kBadHex;
        // The service's messages never carry the password (wifi_service.hpp).
        auto r = wifi->add_network(*ssid, args.size() == 4 ? args[3] : std::string(), os::now_ms());
        return r ? "ok" : error_line(r.error().message);
    }
    if (sub == "forget") {
        if (args.size() != 3) return "error: usage: wifi forget <ssid>";
        std::optional<std::string> ssid = parse_ssid(args[2]);
        if (!ssid) return kBadHex;
        auto r = wifi->forget_network(*ssid);
        return r ? "ok" : error_line(r.error().message);
    }
    // sub == "ap"
    if (args.size() == 3 && args[2] == "on") {
        auto r = wifi->start_provisioning(os::now_ms());
        if (!r) return error_line(r.error().message);
        // The session carries the password too; it stays on the screen.
        return "ap ssid=" + quote_ssid(r.value().ssid) + " ip=" + escape(r.value().ip, false);
    }
    if (args.size() == 3 && args[2] == "off") {
        auto r = wifi->stop_provisioning();
        return r ? "ok" : error_line(r.error().message);
    }
    return "error: usage: wifi ap on|off";
}

// Best effort: the input line may hold a password.
void wipe(std::string& s) {
    volatile char* p = s.data();
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

void run_console(App& app, const std::atomic<bool>& stopping) {
    const Platform& platform = app.platform();
    std::string line;
    while (!stopping.load()) {
        const LineRead got = platform.read_line(line, kReadPollMs);
        if (got == LineRead::Closed) {
            WLOG_I(kTag, "input closed");
            return;
        }
        if (got == LineRead::Timeout) continue;
        std::string out = run_command(app, line);
        wipe(line);
        size_t start = 0;
        while (start < out.size()) {
            size_t eol = out.find('\n', start);
            if (eol == std::string::npos) eol = out.size();
            platform.emit_line(out.substr(start, eol - start));
            start = eol + 1;
        }
    }
}

}  // namespace

Result<std::vector<std::string>> split_command(std::string_view line) {
    std::vector<std::string> words;
    std::string word;
    bool in_word = false;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quoted) {
            if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\')) {
                word += line[++i];
            } else if (c == '"') {
                quoted = false;
            } else {
                word += c;
            }
        } else if (c == ' ' || c == '\t') {
            if (in_word) {
                words.push_back(std::move(word));
                word.clear();
                in_word = false;
            }
        } else if (c == '"') {
            quoted = true;
            in_word = true;
        } else {
            word += c;
            in_word = true;
        }
    }
    if (quoted) {
        wipe(word);
        for (std::string& w : words) wipe(w);
        return Result<std::vector<std::string>>::err(ErrorCode::InvalidArgument,
                                                     "unterminated quote");
    }
    if (in_word) words.push_back(std::move(word));
    return Result<std::vector<std::string>>::ok(std::move(words));
}

std::string run_command(App& app, std::string_view line) {
    auto split = split_command(line);
    if (!split) return error_line(split.error().message);
    std::vector<std::string> args = std::move(split.value());
    std::string out;
    if (args.empty()) {
        out = "";
    } else if (args[0] == "help") {
        out = kHelp;
        for (const ConsoleCommand& cmd : app.platform().console_commands) {
            if (!is_builtin(cmd.name)) out += "\n" + cmd.usage;
        }
    } else if (args[0] == "wifi") {
        out = wifi_command(app, args);
    } else {
        out = "error: unknown command";
        for (const ConsoleCommand& cmd : app.platform().console_commands) {
            if (cmd.name == args[0] && !is_builtin(cmd.name) && cmd.run) {
                out = cmd.run(app, args);
                break;
            }
        }
    }
    for (std::string& a : args) wipe(a);
    return out;
}

std::string quote_ssid(std::string_view ssid) { return "\"" + escape(ssid, true) + "\""; }

void start_console(App& app) {
    App::Impl& impl = *app.impl_;
    if (!impl.platform.read_line || impl.console_task) return;
    impl.console_stopping.store(false);
    impl.console_task = os::Task::spawn({.name = "wisp.console", .stack_bytes = 6144},
                                        [&app, &impl] { run_console(app, impl.console_stopping); });
    if (!impl.console_task) WLOG_E(kTag, "could not start the console task");
}

void stop_console(App& app) {
    App::Impl& impl = *app.impl_;
    if (!impl.console_task) return;
    impl.console_stopping.store(true);
    impl.console_task->join();
    impl.console_task.reset();
}

}  // namespace wisp::app
