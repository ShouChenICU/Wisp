#include "sim_options.hpp"

#include <cstdlib>
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
            opts.exit_after_ms =
                static_cast<uint32_t>(std::strtoul(v.value().c_str(), nullptr, 10));
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
