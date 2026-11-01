// PC simulator entry point (spec 6.1/8.1, task-13-brief.md): parses
// SimOptions, builds the wisp::app::Platform around a SimBoard, runs
// wisp::app::App::start() (which builds the same self-check UI the ESP
// firmware will), and then either loops headless or drives an SDL2
// window at ~60 Hz. This is the only translation unit in the project
// that creates an SDL window or pumps SDL events -- see
// wisp/port/sim/sim_board.hpp's file header on why SimDisplay itself
// stays SDL-free, and AGENTS.md's layering rule ("只有 wisp_port_sim 和
// simulator/ 可以 include SDL*").
//
// Fix round 1, item 1: the audio subsystem (and, for --headless, its
// dummy-driver hint) is initialized here, before app.start() runs --
// SimBoard::init() (called from inside it) needs SDL audio already up
// to probe for a capture device via SDL_GetNumAudioDevices(1), deciding
// whether to register audio_in.main at all (see sim_board.cpp). Video
// stays separate: only run_windowed() below ever initializes it, and
// only on this (the main) thread.

#include <SDL.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#include "sim_options.hpp"
#include "wisp/app/app.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/log_stdout.hpp"
#include "wisp/port/sim/png_writer.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/port/sim/sim_board.hpp"
#include "wisp/port/sim/sim_input.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace {

constexpr int kScreenSize = 480;
constexpr uint32_t kFrameMs = 16;  // ~60 Hz
constexpr const char* kLogTag = "sim";

wisp::port::sim::SimBoardConfig to_board_config(const SimOptions& opts) {
    wisp::port::sim::SimBoardConfig config;
    config.shape = opts.shape;
    config.disabled = opts.disabled;
    config.no_sd = opts.no_sd;
    config.data_dir = opts.data_dir;
    return config;
}

// Takes a screenshot of the self-check UI (app.ui()->snapshot_rgb888())
// and writes it to `path`, per --screenshot. Logs to stderr (rather than
// failing the whole process) if there's no UI or the write fails.
void take_screenshot(wisp::app::App& app, const std::string& path) {
    if (app.ui() == nullptr) {
        std::fprintf(stderr, "wisp_sim: --screenshot requested but no display/UI is running\n");
        return;
    }
    auto snap = app.ui()->snapshot_rgb888();
    if (!snap) {
        std::fprintf(stderr, "wisp_sim: snapshot failed: %s\n", snap.error().message.c_str());
        return;
    }
    auto written =
        wisp::port::sim::write_png_rgb888(path, snap.value().data(), kScreenSize, kScreenSize);
    if (!written) {
        std::fprintf(stderr, "wisp_sim: failed to write screenshot %s: %s\n", path.c_str(),
                     written.error().message.c_str());
    }
}

// Precomputes which of the 480x480 pixels are visible() for the display
// device's actual shape (spec 5.6): an invisible pixel is always
// rendered black by the SDL render loop below, regardless of what
// SimDisplay's framebuffer holds there. Returns an all-visible mask if
// there is no display device or its caps don't parse.
std::vector<uint8_t> build_visible_mask(wisp::app::App& app) {
    std::vector<uint8_t> mask(static_cast<size_t>(kScreenSize) * kScreenSize, 1);
    auto display = app.registry().get<wisp::hal::Display>("display.main");
    if (!display) {
        return mask;
    }
    auto shape_result = wisp::hal::screen_shape_from_caps(display->info().caps);
    if (!shape_result) {
        return mask;
    }
    const auto& shape = shape_result.value();
    for (int y = 0; y < kScreenSize; ++y) {
        for (int x = 0; x < kScreenSize; ++x) {
            mask[static_cast<size_t>(y) * kScreenSize + x] =
                wisp::hal::visible(shape, x, y) ? 1 : 0;
        }
    }
    return mask;
}

// Runs the headless loop: sleeps until exit_after_ms elapses or
// should_exit becomes true (e.g. the power-off callback fired), then
// optionally screenshots (only on the exit_after_ms path -- spec 8.1)
// and returns. The audio subsystem is already up (see main()); this
// function doesn't touch SDL init/teardown at all.
void run_headless(wisp::app::App& app, const SimOptions& opts,
                  const std::atomic<bool>& should_exit) {
    uint64_t t0 = wisp::os::now_ms();
    bool timed_out = false;
    while (!should_exit.load()) {
        if (opts.exit_after_ms != 0 && wisp::os::now_ms() - t0 >= opts.exit_after_ms) {
            timed_out = true;
            break;
        }
        wisp::os::sleep_ms(10);
    }

    if (timed_out && !opts.screenshot.empty()) {
        take_screenshot(app, opts.screenshot);
    }
}

// Translates one SDL event into SimInput state. Mouse motion only
// updates the touch point while a button is held (dragging); key repeat
// events are ignored (SimInput only cares about down/up transitions).
// Fix round 1, item 9: captures the mouse while the (simulated touch)
// button is held, so a drag that leaves the window still keeps
// delivering motion events instead of silently losing the touch.
void handle_event(const SDL_Event& ev, wisp::port::sim::SimInput& input, bool& running) {
    switch (ev.type) {
        case SDL_QUIT:
            running = false;
            break;
        case SDL_MOUSEBUTTONDOWN:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                input.mouse(ev.button.x, ev.button.y, true);
                SDL_CaptureMouse(SDL_TRUE);
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (ev.button.button == SDL_BUTTON_LEFT) {
                input.mouse(ev.button.x, ev.button.y, false);
                SDL_CaptureMouse(SDL_FALSE);
            }
            break;
        case SDL_MOUSEMOTION:
            if ((ev.motion.state & SDL_BUTTON_LMASK) != 0) {
                input.mouse(ev.motion.x, ev.motion.y, true);
            }
            break;
        case SDL_KEYDOWN:
            if (ev.key.repeat == 0) {
                switch (ev.key.keysym.sym) {
                    case SDLK_p:
                        input.key_power(true);
                        break;
                    case SDLK_PLUS:
                    case SDLK_EQUALS:
                    case SDLK_KP_PLUS:
                        input.key_vol_up(true);
                        break;
                    case SDLK_MINUS:
                    case SDLK_KP_MINUS:
                        input.key_vol_down(true);
                        break;
                    case SDLK_UP:
                        input.tilt(0, -1);
                        break;
                    case SDLK_DOWN:
                        input.tilt(0, 1);
                        break;
                    case SDLK_LEFT:
                        input.tilt(-1, 0);
                        break;
                    case SDLK_RIGHT:
                        input.tilt(1, 0);
                        break;
                    case SDLK_s:
                        input.toggle_sd();
                        break;
                    default:
                        break;
                }
            }
            break;
        case SDL_KEYUP:
            switch (ev.key.keysym.sym) {
                case SDLK_p:
                    input.key_power(false);
                    break;
                case SDLK_PLUS:
                case SDLK_EQUALS:
                case SDLK_KP_PLUS:
                    input.key_vol_up(false);
                    break;
                case SDLK_MINUS:
                case SDLK_KP_MINUS:
                    input.key_vol_down(false);
                    break;
                default:
                    break;
            }
            break;
        default:
            break;
    }
}

// Copies SimDisplay's framebuffer, blacks out whatever the shape mask
// (or a powered-off screen) hides, applies the display's software
// brightness, and presents it.
void render_frame(wisp::port::sim::SimDisplay& display, const std::vector<uint8_t>& mask,
                  std::vector<uint16_t>& framebuffer, SDL_Renderer* renderer,
                  SDL_Texture* texture) {
    display.copy_framebuffer(framebuffer.data());
    bool powered = display.powered();
    for (size_t i = 0; i < framebuffer.size(); ++i) {
        if (mask[i] == 0 || !powered) {
            framebuffer[i] = 0;
        }
    }

    void* pixels = nullptr;
    int pitch = 0;
    if (SDL_LockTexture(texture, nullptr, &pixels, &pitch) == 0) {
        auto* dst = static_cast<uint8_t*>(pixels);
        for (int y = 0; y < kScreenSize; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * static_cast<size_t>(pitch),
                        framebuffer.data() + static_cast<size_t>(y) * kScreenSize,
                        static_cast<size_t>(kScreenSize) * sizeof(uint16_t));
        }
        SDL_UnlockTexture(texture);
    }

    // Fix round 1, item 6: apply the display's software brightness by
    // modulating the texture's color channels at blit time (rather than
    // rescaling every RGB565 pixel on the CPU each frame).
    uint8_t brightness = display.brightness();
    SDL_SetTextureColorMod(texture, brightness, brightness, brightness);

    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}

// Runs the windowed loop: pumps SDL events into `input`, renders
// SimDisplay's framebuffer at ~60 Hz, and returns once the window is
// closed, exit_after_ms elapses, or should_exit becomes true. Returns
// false (having already reported the failure to stderr) if SDL's video
// subsystem or the window/renderer/texture couldn't be created (fix
// round 1, item 3) -- the audio subsystem is already up (see main()).
bool run_windowed(wisp::app::App& app, const SimOptions& opts, wisp::port::sim::SimBoard& board,
                  wisp::port::sim::SimInput& input, const std::atomic<bool>& should_exit) {
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "wisp_sim: SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_Window* window =
        SDL_CreateWindow("Wisp Simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         kScreenSize, kScreenSize, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = window != nullptr ? SDL_CreateRenderer(window, -1, 0) : nullptr;
    SDL_Texture* texture = renderer != nullptr ? SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                                                   SDL_TEXTUREACCESS_STREAMING,
                                                                   kScreenSize, kScreenSize)
                                               : nullptr;
    if (window == nullptr || renderer == nullptr || texture == nullptr) {
        std::fprintf(stderr, "wisp_sim: failed to create SDL window: %s\n", SDL_GetError());
        if (texture != nullptr) SDL_DestroyTexture(texture);
        if (renderer != nullptr) SDL_DestroyRenderer(renderer);
        if (window != nullptr) SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return false;
    }

    std::vector<uint8_t> mask = build_visible_mask(app);
    std::vector<uint16_t> framebuffer(static_cast<size_t>(kScreenSize) * kScreenSize, 0);
    auto display = board.display();

    uint64_t t0 = wisp::os::now_ms();
    bool running = true;
    bool timed_out = false;
    while (running && !should_exit.load()) {
        uint64_t frame_start = wisp::os::now_ms();

        SDL_Event ev;
        while (SDL_PollEvent(&ev) != 0) {
            handle_event(ev, input, running);
        }

        if (opts.exit_after_ms != 0 && wisp::os::now_ms() - t0 >= opts.exit_after_ms) {
            timed_out = true;
            running = false;
        }

        if (display) {
            render_frame(*display, mask, framebuffer, renderer, texture);
        }

        uint64_t elapsed = wisp::os::now_ms() - frame_start;
        if (elapsed < kFrameMs) {
            wisp::os::sleep_ms(static_cast<uint32_t>(kFrameMs - elapsed));
        }
    }

    if (timed_out && !opts.screenshot.empty()) {
        take_screenshot(app, opts.screenshot);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    auto parsed = parse_sim_options(argc, argv);
    if (!parsed) {
        std::fprintf(stderr, "wisp_sim: %s\n", parsed.error().message.c_str());
        return 2;
    }
    SimOptions opts = std::move(parsed.value());

    std::error_code ec;
    std::filesystem::create_directories(opts.data_dir, ec);

    // log::init() is called again (harmlessly -- see log.hpp) inside
    // App::start(); it's called here too, first, so the WLOG_W below
    // (fix round 1, item 5) is actually recorded/delivered instead of
    // being a silent no-op before the first init() (see log.hpp).
    static wisp::port::sim::StdoutLogSink log_sink;
    wisp::log::init();
    wisp::log::add_sink(&log_sink);

    if (!opts.screenshot.empty() && opts.exit_after_ms == 0) {
        // A screenshot is only ever taken on the exit_after_ms timeout
        // path (see take_screenshot()'s call sites above) -- without
        // one, --screenshot would silently never fire.
        WLOG_W(kLogTag, "--screenshot given without --exit-after-ms; no screenshot will be taken");
    }

    // Fix round 1, item 1: audio must be up before app.start() runs --
    // see this file's header comment.
    if (opts.headless) {
        SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
    }
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "wisp_sim: SDL_Init(AUDIO) failed: %s\n", SDL_GetError());
        return 1;
    }

    std::atomic<bool> should_exit{false};
    auto input = std::make_shared<wisp::port::sim::SimInput>(/*sd_present_initial=*/!opts.no_sd);
    auto on_power_off = [&should_exit] { should_exit.store(true); };

    // Fix round 1, item 7: keep the concrete SimBoard* from make_unique
    // directly, before it's moved into Platform::board (a
    // unique_ptr<Board>) -- a plain upcast on construction, no
    // static_cast needed later to get it back.
    auto board =
        std::make_unique<wisp::port::sim::SimBoard>(to_board_config(opts), input, on_power_off);
    wisp::port::sim::SimBoard* board_ptr = board.get();

    wisp::app::Platform platform;
    platform.board = std::move(board);
    platform.settings = std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(
        opts.data_dir + "/settings.json");
    platform.emit_line = [](const std::string& line) {
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    };
    platform.request_power_off = on_power_off;
    platform.heap_info = [] { return wisp::Value(wisp::Map{}); };
    platform.firmware_version = "0.1.0";

    wisp::app::App app(std::move(platform));
    auto started = app.start();
    if (!started) {
        std::fprintf(stderr, "wisp_sim: App::start() failed: %s\n",
                     started.error().message.c_str());
        SDL_Quit();
        return 1;
    }

    int exit_code = 0;
    if (opts.headless) {
        run_headless(app, opts, should_exit);
    } else if (!run_windowed(app, opts, *board_ptr, *input, should_exit)) {
        exit_code = 1;
    }

    app.stop();
    SDL_Quit();
    return exit_code;
}
