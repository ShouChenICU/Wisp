#include "sim_options.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>
#include <utility>

#include "wisp/base/error.hpp"

namespace {

// Consumes argv[++i] as the value for flag `name`. Fails with
// InvalidArgument (mentioning `name`) if `i` is already the last
// argument.
wisp::Result<std::string> take_value(int argc, const char* const* argv, int& i, const char* name) {
    if (i + 1 >= argc) {
        return wisp::Result<std::string>::err(wisp::ErrorCode::InvalidArgument,
                                              std::string("missing value for ") + name);
    }
    return wisp::Result<std::string>::ok(argv[++i]);
}

// Parses `s` as a non-negative, base-10 uint32_t with no trailing
// garbage -- a bare std::strtoul() call silently accepts "-5" by
// wrapping it around, and silently accepts "5abc" by just ignoring the
// "abc". Returns false (leaving `out` untouched) for
// anything else, including an empty string or one that doesn't fit in
// 32 bits.
bool parse_uint32(const std::string& s, uint32_t& out) {
    if (s.empty() || s[0] == '-') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    unsigned long parsed = std::strtoul(s.c_str(), &end, 10);
    if (end != s.c_str() + s.size() || errno == ERANGE ||
        parsed > std::numeric_limits<uint32_t>::max()) {
        return false;
    }
    out = static_cast<uint32_t>(parsed);
    return true;
}

// Parses "<c1>,<c2>,...@<ms>" with exactly `coords` coordinates, each a
// pixel on the 480x480 simulated screen.
bool parse_touch(const std::string& s, size_t coords, int* xy, uint32_t& at_ms) {
    constexpr uint32_t kScreenSize = 480;
    size_t at = s.find('@');
    if (at == std::string::npos || !parse_uint32(s.substr(at + 1), at_ms)) {
        return false;
    }
    std::string list = s.substr(0, at);
    for (size_t i = 0; i < coords; ++i) {
        size_t comma = list.find(',');
        bool last = i + 1 == coords;
        if (last != (comma == std::string::npos)) {
            return false;
        }
        uint32_t v = 0;
        if (!parse_uint32(last ? list : list.substr(0, comma), v) || v >= kScreenSize) {
            return false;
        }
        xy[i] = static_cast<int>(v);
        if (!last) {
            list = list.substr(comma + 1);
        }
    }
    return true;
}

}  // namespace

wisp::Result<SimOptions> parse_sim_options(int argc, const char* const* argv) {
    SimOptions opts;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--no-sd") {
            opts.no_sd = true;
        } else if (arg == "--wisp-demo") {
            opts.wisp_demo = true;
        } else if (arg == "--screenshot") {
            auto v = take_value(argc, argv, i, "--screenshot");
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            opts.screenshot = std::move(v.value());
        } else if (arg == "--exit-after-ms") {
            auto v = take_value(argc, argv, i, "--exit-after-ms");
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            uint32_t parsed = 0;
            if (!parse_uint32(v.value(), parsed)) {
                return wisp::Result<SimOptions>::err(wisp::ErrorCode::InvalidArgument,
                                                     "invalid --exit-after-ms value: " + v.value());
            }
            opts.exit_after_ms = parsed;
        } else if (arg == "--tap" || arg == "--swipe") {
            auto v = take_value(argc, argv, i, arg.c_str());
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            SimTouchStep step;
            const bool tap = arg == "--tap";
            step.kind = tap ? SimTouchStep::Kind::Tap : SimTouchStep::Kind::Swipe;
            int xy[4] = {};
            if (!parse_touch(v.value(), tap ? 2 : 4, xy, step.at_ms)) {
                return wisp::Result<SimOptions>::err(wisp::ErrorCode::InvalidArgument,
                                                     "invalid " + arg + " value: " + v.value());
            }
            step.x1 = xy[0];
            step.y1 = xy[1];
            step.x2 = tap ? xy[0] : xy[2];
            step.y2 = tap ? xy[1] : xy[3];
            opts.touches.push_back(step);
        } else if (arg == "--wifi-fail") {
            opts.wifi_fail = true;
        } else if (arg == "--http-port") {
            auto v = take_value(argc, argv, i, "--http-port");
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            uint32_t parsed = 0;
            if (!parse_uint32(v.value(), parsed) || parsed > 65535) {
                return wisp::Result<SimOptions>::err(wisp::ErrorCode::InvalidArgument,
                                                     "invalid --http-port value: " + v.value());
            }
            opts.http_port = static_cast<uint16_t>(parsed);
        } else if (arg == "--web-dir") {
            auto v = take_value(argc, argv, i, "--web-dir");
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            opts.web_dir = std::move(v.value());
        } else if (arg == "--scenes-dir" || arg == "--open-scene") {
            auto v = take_value(argc, argv, i, arg.c_str());
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            (arg == "--scenes-dir" ? opts.scenes_dir : opts.open_scene) = std::move(v.value());
        } else if (arg == "--imu-script") {
            auto v = take_value(argc, argv, i, "--imu-script");
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            opts.imu_script = std::move(v.value());
        } else if (arg == "--font-image") {
            auto v = take_value(argc, argv, i, "--font-image");
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            opts.font_image = std::move(v.value());
        } else if (arg == "--disable") {
            auto v = take_value(argc, argv, i, "--disable");
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            opts.disabled.push_back(std::move(v.value()));
        } else if (arg == "--data-dir") {
            auto v = take_value(argc, argv, i, "--data-dir");
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            opts.data_dir = std::move(v.value());
        } else if (arg == "--shape") {
            auto v = take_value(argc, argv, i, "--shape");
            if (!v) {
                return wisp::Result<SimOptions>::err(v.error().code, v.error().message);
            }
            const std::string& shape = v.value();
            if (shape != "rect" && shape != "rounded_rect" && shape != "circle") {
                return wisp::Result<SimOptions>::err(wisp::ErrorCode::InvalidArgument,
                                                     "invalid --shape value: " + shape);
            }
            opts.shape = shape;
        } else {
            return wisp::Result<SimOptions>::err(wisp::ErrorCode::InvalidArgument,
                                                 "unknown argument: " + arg);
        }
    }

    return wisp::Result<SimOptions>::ok(std::move(opts));
}

wisp::Result<SimTouchStep> parse_touch_command(const std::string& line) {
    SimTouchStep step;
    std::string coords;
    const size_t word_end = line.find_first_of(kTouchSeparators);
    const std::string word = line.substr(0, word_end);
    if (word_end != std::string::npos && (word == "tap" || word == "swipe")) {
        step.kind = word == "tap" ? SimTouchStep::Kind::Tap : SimTouchStep::Kind::Swipe;
        const size_t start = line.find_first_not_of(kTouchSeparators, word_end);
        if (start != std::string::npos) coords = line.substr(start);
    }
    const bool tap = step.kind == SimTouchStep::Kind::Tap;
    int xy[4] = {};
    uint32_t at_ms = 0;
    if (coords.empty() || coords.find('@') != std::string::npos ||
        !parse_touch(coords + "@0", tap ? 2 : 4, xy, at_ms)) {
        return wisp::Result<SimTouchStep>::err(wisp::ErrorCode::InvalidArgument,
                                               "invalid touch command: " + line);
    }
    step.x1 = xy[0];
    step.y1 = xy[1];
    step.x2 = tap ? xy[0] : xy[2];
    step.y2 = tap ? xy[1] : xy[3];
    return wisp::Result<SimTouchStep>::ok(step);
}
