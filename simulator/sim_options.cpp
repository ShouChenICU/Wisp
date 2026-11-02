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

}  // namespace

wisp::Result<SimOptions> parse_sim_options(int argc, const char* const* argv) {
    SimOptions opts;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];

        if (arg == "--headless") {
            opts.headless = true;
        } else if (arg == "--no-sd") {
            opts.no_sd = true;
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
