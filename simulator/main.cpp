// PC simulator entry point (spec 6.1/8.1): parses
// SimOptions (including scripted --tap/--swipe touches, see TouchScript), builds the
// wisp::app::Platform around a SimBoard, runs wisp::app::App::start() (which builds the same
// self-check UI the ESP firmware will), and then either loops headless or drives an SDL2 window at
// ~60 Hz. This is the only translation unit in the project that creates an SDL window or pumps SDL
// events -- see wisp/port/sim/sim_board.hpp's file header on why SimDisplay itself stays SDL-free,
// and AGENTS.md's layering rule ("只有 wisp_port_sim 和 simulator/ 可以 include SDL*").
//
// The audio subsystem (and, for --headless, its dummy-driver hint) is
// initialized here, before app.start() runs --
// SimBoard::init() (called from inside it) needs SDL audio already up
// to probe for a capture device via SDL_GetNumAudioDevices(1), deciding
// whether to register audio_in.main at all (see sim_board.cpp). Video
// stays separate: only run_windowed() below ever initializes it, and
// only on this (the main) thread.
//
// Networking: the admin server (SimHttpServer) listens on 127.0.0.1 only
// and runs from boot (Platform::http_always_on), even while SimWifi is
// idle, so the admin page can be developed on the PC. Its page comes from
// the assets.wpak the build packs next to this executable, or from
// --web-dir. Platform::reboot prints "WISP_REBOOT" and makes the main
// loop return, ending the process with exit code 0 after a normal
// App::stop(). A firmware upload is written to <data-dir>/firmware.bin;
// confirming it on the screen ends in that same reboot.
//
// stdin: one StdinReader (sim_stdin.hpp) owns it for the whole run, with
// or without --headless and whether or not it is a terminal. "tap"/"swipe"
// lines go to TouchScript; every other line is a serial console command
// (connectivity spec 8.2) read through Platform::read_line. SIGTTIN is
// ignored so a backgrounded `wisp_sim &` is never stopped by it (see
// sim_stdin.hpp).
//
// Signals: SIGINT and SIGTERM end the main loop like a reboot does, so the
// process stops through App::stop() and exits with code 0; SDL's own
// handlers (which would only queue an SDL_QUIT the headless loop never
// reads) are turned off. The handler is one-shot: a second signal while
// shutting down kills the process the default way.
//
// Debug commands: with WISP_SIM_DEBUG_COMMANDS (defined for the wisp_sim
// target only, never in a portable component or the firmware) the console
// gains "debug pair-code", which prints the active pairing code as
// "pair-code=<6 digits>" ("pair-code=none" without one) so scripts can pair
// without reading the screen (tests/smoke/admin_smoke.py). It is the only
// output that ever carries a pairing code. "debug scene-click <ref>" clicks
// the widget with that ref in the scene in front
// (SceneRuntime::click_ref()) and answers "scene-click=ok" or
// "scene-click=<error>". "debug panel-tap <index>" taps row <index> (from
// 0: the background scenes, then the notifications) of the open pull-down
// panel (SystemLayer::tap_panel_row()) and answers "panel-tap=ok" or
// "panel-tap=<error>". "debug screenshot <path>" writes what the screen
// shows now as a PNG, as --screenshot does at exit, and answers
// "screenshot=ok" or "screenshot=<error>". "debug clock-advance <seconds>"
// moves the simulated clock (clock.main and Platform::wall_s) forward,
// publishes system.time {"state":"synced","server":"debug","time"} as a
// sync would, and answers "clock-advance=ok" or "clock-advance=<error>".
// "debug voice-text <text>" hands the text to the conversation as a
// transcript (ConversationController::voice_text(): source voice, spoken
// unless muted) and answers "voice-text=ok" or "voice-text=<error>".
//
// Scenes (scene-runtime-core spec 6.4): --scenes-dir puts the scene
// repository in dev mode over that directory (Platform::scenes_dir), with
// the scenes' data in <data-dir>/scenes/<id>/data; the scene in front
// reloads when its files change (see components/wisp_app/src/
// scene_wiring.hpp). --open-scene opens one once App::start() returns.

#include <SDL.h>
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "sim_options.hpp"
#include "sim_stdin.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/bench.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/image_decoders.hpp"
#include "wisp/port/sim/imu_script.hpp"
#include "wisp/port/sim/log_stdout.hpp"
#include "wisp/port/sim/png_writer.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/port/sim/sim_board.hpp"
#include "wisp/port/sim/sim_firmware.hpp"
#include "wisp/port/sim/sim_http_client.hpp"
#include "wisp/port/sim/sim_http_server.hpp"
#include "wisp/port/sim/sim_input.hpp"
#include "wisp/port/sim/sim_sntp.hpp"
#include "wisp/port/sim/sim_web_pack.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/shell/conversation.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

namespace {

// Set by the main loop's exit paths: power-off, reboot, SIGINT/SIGTERM.
// Namespace scope so the signal handler can reach it.
std::atomic<bool> g_should_exit{false};
// Seconds the simulated wall clock runs ahead of the host's (`debug
// clock-advance`).
std::atomic<int64_t> g_clock_offset_s{0};
static_assert(std::atomic<bool>::is_always_lock_free, "stored from a signal handler");

extern "C" void on_stop_signal(int) { g_should_exit.store(true); }

void install_signal_handlers() {
    struct sigaction action{};
    action.sa_handler = on_stop_signal;
    sigemptyset(&action.sa_mask);
    // SA_RESTART: the signal lands on whichever thread, which must not see
    // an interrupted syscall because of it.
    action.sa_flags = SA_RESETHAND | SA_RESTART;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
    signal(SIGTTIN, SIG_IGN);
}

wisp::Result<void> write_screenshot(wisp::app::App& app, const std::string& path);

#ifdef WISP_SIM_DEBUG_COMMANDS
std::string debug_command(wisp::app::App& app, const std::vector<std::string>& args) {
    if (args.size() == 2 && args[1] == "pair-code") {
        wisp::net::PairingService* pairing = app.pairing();
        if (pairing == nullptr) return "error: pairing is unavailable";
        auto code = pairing->active_code(wisp::os::now_ms());
        return "pair-code=" + (code ? *code : std::string("none"));
    }
    if (args.size() == 3 && args[1] == "scene-click") {
        wisp::scene::SceneRuntime* scenes = app.scenes();
        if (scenes == nullptr) return "scene-click=scenes are off";
        auto r = scenes->click_ref(args[2]);
        return "scene-click=" + (r ? std::string("ok") : r.error().message);
    }
    if (args.size() == 3 && args[1] == "panel-tap") {
        wisp::ui::UiRuntime* ui = app.ui();
        wisp::ui::SystemLayer* layer = app.system_layer();
        if (ui == nullptr || layer == nullptr) return "panel-tap=there is no screen";
        char* end = nullptr;
        const unsigned long index = std::strtoul(args[2].c_str(), &end, 10);
        if (args[2].empty() || *end != '\0') return "panel-tap=the index is not a number";
        wisp::Result<void> r = wisp::Result<void>::ok();
        ui->run_blocking([&] { r = layer->tap_panel_row(index); });
        return "panel-tap=" + (r ? std::string("ok") : r.error().message);
    }
    if (args.size() == 3 && args[1] == "clock-advance") {
        char* end = nullptr;
        const long long seconds = std::strtoll(args[2].c_str(), &end, 10);
        if (args[2].empty() || *end != '\0') return "clock-advance=the seconds are not a number";
        g_clock_offset_s.fetch_add(seconds);
        if (auto clock = app.registry().get<wisp::hal::Clock>("clock.main")) {
            if (auto now = clock->get_unix()) (void)clock->set_unix(now.value() + seconds);
        }
        const int64_t now = static_cast<int64_t>(std::time(nullptr)) + g_clock_offset_s.load();
        app.bus().publish(wisp::core::Event{
            "system.time",
            "debug",
            0,
            {{"state", wisp::Value("synced")},
             {"server", wisp::Value("debug")},
             {"time", wisp::Value(now)}},
        });
        return std::string("clock-advance=ok");
    }
    if (args.size() == 3 && args[1] == "screenshot") {
        auto r = write_screenshot(app, args[2]);
        return "screenshot=" + (r ? std::string("ok") : r.error().message);
    }
    if (args.size() >= 3 && args[1] == "voice-text") {
        wisp::shell::ConversationController* conv = app.conversation();
        if (conv == nullptr) return "voice-text=voice is off";
        std::string text = args[2];
        for (size_t i = 3; i < args.size(); ++i) text += " " + args[i];
        auto r = conv->voice_text(std::move(text));
        return "voice-text=" + (r ? std::string("ok") : r.error().message);
    }
    return "error: usage: debug pair-code | debug scene-click <ref> | debug panel-tap <index> | "
           "debug screenshot <path> | debug clock-advance <seconds> | debug voice-text <text>";
}

std::vector<wisp::app::ConsoleCommand> debug_commands() {
    return {{
        "debug",
        "debug pair-code              the active pairing code (simulator only)\n"
        "debug scene-click <ref>      click a widget of the scene in front (simulator only)\n"
        "debug panel-tap <index>      tap a row of the open pull-down panel (simulator only)\n"
        "debug screenshot <path>      write the screen as a PNG (simulator only)\n"
        "debug clock-advance <s>      move the clock forward s seconds (simulator only)\n"
        "debug voice-text <text>      send text as if spoken, skipping the mic (simulator only)",
        debug_command,
    }};
}
#endif

constexpr int kScreenSize = 480;
constexpr uint32_t kFrameMs = 16;  // ~60 Hz
constexpr const char* kLogTag = "sim";

wisp::port::sim::SimBoardConfig to_board_config(const SimOptions& opts) {
    wisp::port::sim::SimBoardConfig config;
    config.shape = opts.shape;
    config.panel = opts.panel;
    config.disabled = opts.disabled;
    config.no_sd = opts.no_sd;
    config.data_dir = opts.data_dir;
    config.wifi_fail = opts.wifi_fail;
    return config;
}

// The directory wisp_sim runs from, where the build puts the files it
// packs (assets.wpak, fonts.bin).
std::filesystem::path exe_dir(const char* argv0) {
    std::error_code ec;
    std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) exe = std::filesystem::absolute(argv0, ec);
    return exe.parent_path();
}

// The admin page's WispPack: --web-dir packed in memory, else the
// assets.wpak next to the executable. Empty (API only) when neither is
// usable.
std::vector<uint8_t> load_web_assets(const SimOptions& opts, const char* argv0) {
    if (!opts.web_dir.empty()) {
        auto packed = wisp::port::sim::pack_web_dir(opts.web_dir);
        if (!packed) {
            WLOG_W(kLogTag, "--web-dir: %s; serving the API only", packed.error().message.c_str());
            return {};
        }
        return std::move(packed.value());
    }
    std::ifstream in(exe_dir(argv0) / "assets.wpak", std::ios::binary);
    if (!in) {
        WLOG_W(kLogTag, "no assets.wpak next to wisp_sim; serving the API only");
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// A font image option's bytes: the file `option` names, else `fallback`
// next to the executable; nothing for "none". Empty when unreadable:
// UiRuntime then logs why and degrades. Kept in memory for the whole run,
// since Platform::font_image/glyph_image point into it.
std::vector<uint8_t> load_image_option(const std::string& option, const char* fallback,
                                       const char* argv0) {
    if (option == "none") return {};
    const std::filesystem::path path =
        option.empty() ? exe_dir(argv0) / fallback : std::filesystem::path(option);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        WLOG_W(kLogTag, "cannot read the font image %s", path.c_str());
        return {};
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// The vector font image (vector-font spec 3.3): --font-image's file, else
// fonts.bin; without it large text uses bitmap fonts.
std::vector<uint8_t> load_font_image(const SimOptions& opts, const char* argv0) {
    return load_image_option(opts.font_image, "fonts.bin", argv0);
}

// The body font image (glyph-partition spec 3.3): --glyph-image's file,
// else glyphs.bin; without it the built-in body font stands in, or for
// "none" the 16 px vector font, as on a device whose glyphs partition is
// missing.
std::vector<uint8_t> load_glyph_image(const SimOptions& opts, const char* argv0) {
    return load_image_option(opts.glyph_image, "glyphs.bin", argv0);
}

// --imu-script's steps (wisp/port/sim/imu_script.hpp); an unreadable or
// malformed file stops the simulator, as a bad option does.
wisp::Result<std::vector<wisp::port::sim::ImuScriptStep>> load_imu_script(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return wisp::Result<std::vector<wisp::port::sim::ImuScriptStep>>::err(
            wisp::ErrorCode::NotFound, "cannot read the IMU script " + path);
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    auto steps = wisp::port::sim::parse_imu_script(text);
    if (!steps) {
        return wisp::Result<std::vector<wisp::port::sim::ImuScriptStep>>::err(
            steps.error().code, path + ": " + steps.error().message);
    }
    return steps;
}

// Writes what the screen shows (app.ui()->snapshot_rgb888()) to `path` as
// a PNG.
wisp::Result<void> write_screenshot(wisp::app::App& app, const std::string& path) {
    if (app.ui() == nullptr) {
        return wisp::Result<void>::err(wisp::ErrorCode::Unsupported, "no display/UI is running");
    }
    auto snap = app.ui()->snapshot_rgb888();
    if (!snap) {
        return wisp::Result<void>::err(snap.error().code,
                                       "snapshot failed: " + snap.error().message);
    }
    auto written =
        wisp::port::sim::write_png_rgb888(path, snap.value().data(), kScreenSize, kScreenSize);
    if (!written) {
        return wisp::Result<void>::err(written.error().code, "failed to write screenshot " + path +
                                                                 ": " + written.error().message);
    }
    return wisp::Result<void>::ok();
}

// Takes a screenshot per --screenshot. Logs to stderr (rather than failing
// the whole process) if there's no UI or the write fails.
void take_screenshot(wisp::app::App& app, const std::string& path) {
    if (auto r = write_screenshot(app, path); !r) {
        std::fprintf(stderr, "wisp_sim: --screenshot: %s\n", r.error().message.c_str());
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

// Plays --tap/--swipe (SimTouchStep) against SimInput on its own thread,
// timed from construction (right after App::start()), so both the
// headless and the SDL loop keep running meanwhile. Touch lines from stdin
// (sim_stdin.hpp) arrive through enqueue(), which only queues them; a
// second thread plays them in order as they come. In a windowed run a
// real mouse press in between would overwrite the scripted point. The
// destructor stops early and joins.
class TouchScript {
public:
    TouchScript(std::vector<SimTouchStep> steps, wisp::port::sim::SimInput& input)
        : steps_(std::move(steps)), input_(input) {
        if (!steps_.empty()) {
            thread_ = std::thread([this] { run(); });
        }
        queue_thread_ = std::thread([this] { run_queue(); });
    }
    ~TouchScript() {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            stop_.store(true);
        }
        queue_cv_.notify_all();
        for (std::thread* t : {&thread_, &queue_thread_}) {
            if (t->joinable()) {
                t->join();
            }
        }
    }
    TouchScript(const TouchScript&) = delete;
    TouchScript& operator=(const TouchScript&) = delete;

    // Never blocks: called on the stdin reader's thread.
    void enqueue(const std::string& line) {
        auto step = parse_touch_command(line);
        if (!step) {
            WLOG_W("sim", "%s", step.error().message.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push_back(step.value());
        }
        queue_cv_.notify_all();
    }

private:
    static constexpr uint32_t kTapMs = 100;
    static constexpr int kSwipeSteps = 20;
    static constexpr uint32_t kSwipeMs = 400;
    static constexpr uint32_t kSwipeRestMs = 200;

    // False once stop_ is set, so a long wait never delays shutdown.
    bool wait_until(uint64_t at) {
        while (wisp::os::now_ms() < at) {
            if (stop_.load()) {
                return false;
            }
            wisp::os::sleep_ms(5);
        }
        return !stop_.load();
    }

    void run() {
        const uint64_t t0 = wisp::os::now_ms();
        for (const SimTouchStep& step : steps_) {
            if (!wait_until(t0 + step.at_ms)) {
                return;
            }
            play(step);
        }
    }

    void run_queue() {
        while (true) {
            SimTouchStep step;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait(lock, [this] { return stop_.load() || !queue_.empty(); });
                if (stop_.load()) {
                    return;
                }
                step = queue_.front();
                queue_.pop_front();
            }
            play(step);
        }
    }

    void play(const SimTouchStep& step) {
        input_.mouse(step.x1, step.y1, true);
        if (step.kind == SimTouchStep::Kind::Tap) {
            wait_until(wisp::os::now_ms() + kTapMs);
        } else {
            const uint64_t start = wisp::os::now_ms();
            for (int i = 1; i <= kSwipeSteps; ++i) {
                if (!wait_until(start + kSwipeMs * i / kSwipeSteps)) {
                    break;
                }
                input_.mouse(step.x1 + (step.x2 - step.x1) * i / kSwipeSteps,
                             step.y1 + (step.y2 - step.y1) * i / kSwipeSteps, true);
            }
            wait_until(wisp::os::now_ms() + kSwipeRestMs);
        }
        input_.mouse(step.x2, step.y2, false);
    }

    const std::vector<SimTouchStep> steps_;
    wisp::port::sim::SimInput& input_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<SimTouchStep> queue_;
    std::thread queue_thread_;
};

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
// Captures the mouse while the (simulated touch) button is held, so a
// drag that leaves the window still keeps delivering motion events
// instead of silently losing the touch.
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

    // Apply the display's software brightness by modulating the
    // texture's color channels at blit time (rather than rescaling
    // every RGB565 pixel on the CPU each frame).
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
// subsystem or the window/renderer/texture couldn't be created -- the
// audio subsystem is already up (see main()).
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

// --wisp-demo (spec 6.1): a screen of its own, its background from
// wisp_background(panel), with the wisp in the middle at 56% of the safe
// area's short side. Every 3 s it moves on to the
// next mood; Listening and Speaking get a level swinging at 1 Hz, and every
// other round Thinking runs busy. The self-check screen stays behind it,
// so WISP_PERF keeps coming.
struct WispDemo {
    wisp::ui::WispView* view = nullptr;
    uint64_t start_ms = 0;
    int slot = -1;
};

void wisp_demo_tick(lv_timer_t* t) {
    auto* demo = static_cast<WispDemo*>(lv_timer_get_user_data(t));
    const uint64_t elapsed = wisp::os::now_ms() - demo->start_ms;
    const int slot = static_cast<int>(elapsed / 3000);
    const auto mood = static_cast<wisp::ui::WispMood>(slot % 5);
    if (slot != demo->slot) {
        demo->slot = slot;
        demo->view->set_mood(mood);
        demo->view->set_busy(mood == wisp::ui::WispMood::Thinking && (slot / 5) % 2 == 1);
        WLOG_I(kLogTag, "wisp demo: mood %d", static_cast<int>(mood));
    }
    if (mood == wisp::ui::WispMood::Listening || mood == wisp::ui::WispMood::Speaking) {
        const double phase = static_cast<double>(elapsed % 1000) / 1000.0;
        demo->view->set_level(static_cast<uint8_t>(50 + 45 * std::sin(phase * 6.283185307179586)));
    } else {
        demo->view->set_level(0);
    }
}

void start_wisp_demo(wisp::app::App& app) {
    wisp::ui::UiRuntime* ui = app.ui();
    auto display = app.registry().get<wisp::hal::Display>("display.main");
    if (ui == nullptr || !display) {
        WLOG_W(kLogTag, "--wisp-demo: no display");
        return;
    }
    auto shape = wisp::hal::screen_shape_from_caps(display->info().caps);
    if (!shape) {
        WLOG_W(kLogTag, "--wisp-demo: %s", shape.error().message.c_str());
        return;
    }
    static WispDemo demo;
    ui->run_blocking([&] {
        lv_obj_t* scr = lv_obj_create(nullptr);
        lv_obj_set_style_bg_color(
            scr, wisp::ui::wisp_background(wisp::ui::display_panel(lv_obj_get_display(scr))), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(scr, 0, 0);
        lv_obj_set_style_border_width(scr, 0, 0);
        lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
        // 56% of the safe area's short side makes the wisp itself (tips to
        // bottom) about half the screen's height; its 5:4 box still fits
        // the safe area, circle included.
        demo.view = wisp::ui::WispView::create(
            scr, wisp::ui::wisp_size_px(wisp::hal::safe_rect(shape.value()), 56));
        if (demo.view == nullptr) {
            lv_obj_delete(scr);
            WLOG_E(kLogTag, "--wisp-demo: out of memory");
            return;
        }
        lv_screen_load(scr);
        demo.start_ms = wisp::os::now_ms();
        lv_timer_create(wisp_demo_tick, 50, &demo);
    });
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
    // Absolute: StorageManager only removes or renames inside an absolute
    // mount point, so with a relative --data-dir (the default among them)
    // installing a scene would fail.
    if (auto abs = std::filesystem::absolute(opts.data_dir, ec); !ec) {
        opts.data_dir = abs.lexically_normal().string();
        if (opts.data_dir.size() > 1 && opts.data_dir.back() == '/') opts.data_dir.pop_back();
    }

    // log::init() is called again (harmlessly -- see log.hpp) inside
    // App::start(); it's called here too, first, so the WLOG_W below
    // is actually recorded/delivered instead of being a silent no-op
    // before the first init() (see log.hpp).
    static wisp::port::sim::StdoutLogSink log_sink;
    wisp::log::init();
    wisp::log::add_sink(&log_sink);

    if (!opts.screenshot.empty() && opts.exit_after_ms == 0) {
        // A screenshot is only ever taken on the exit_after_ms timeout
        // path (see take_screenshot()'s call sites above) -- without
        // one, --screenshot would silently never fire.
        WLOG_W(kLogTag, "--screenshot given without --exit-after-ms; no screenshot will be taken");
    }

    // Audio must be up before app.start() runs -- see this file's
    // header comment.
    if (opts.headless) {
        SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
    }
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    install_signal_handlers();
    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        // A host with no audio backend at all (e.g. a minimal
        // container image) shouldn't stop the whole
        // simulator from running -- warn and continue.
        // SimBoard::init() checks SDL_WasInit(SDL_INIT_AUDIO) and
        // degrades audio_out.main/audio_in.main to Failed/"SDL audio
        // unavailable" instead (see sim_board.cpp).
        WLOG_W(kLogTag, "SDL_Init(AUDIO) failed: %s; audio devices will be unavailable",
               SDL_GetError());
    }

    std::atomic<bool>& should_exit = g_should_exit;
    // Outlives `app`: Platform::read_line pops from it.
    ConsoleLines console_lines;
    // Outlives `app` (Platform::assets points into it).
    const std::vector<uint8_t> web_assets = load_web_assets(opts, argv[0]);
    // Outlives `app` (Platform::font_image points into it).
    const std::vector<uint8_t> font_image = load_font_image(opts, argv[0]);
    // Outlives `app` (Platform::glyph_image points into it).
    const std::vector<uint8_t> glyph_image = load_glyph_image(opts, argv[0]);
    auto input = std::make_shared<wisp::port::sim::SimInput>(/*sd_present_initial=*/!opts.no_sd);
    if (!opts.imu_script.empty()) {
        auto steps = load_imu_script(opts.imu_script);
        if (!steps) {
            std::fprintf(stderr, "wisp_sim: %s\n", steps.error().message.c_str());
            return 2;
        }
        input->set_imu_script(std::move(steps.value()));
    }
    auto on_power_off = [&should_exit] { should_exit.store(true); };

    // Keep the concrete SimBoard* from make_unique directly, before
    // it's moved into Platform::board (a
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
        // One stdio call, not fputs()+fputc(): StdoutLogSink::printf()
        // can run on another thread between two separate calls and land
        // its own line in between them, splitting this one across two
        // lines of output -- fatal for a reader (e.g. the smoke test)
        // that expects "WISP_BOOT_REPORT {...}" as a single whole line.
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    platform.request_power_off = on_power_off;
    // No real heap diagnostics on a PC; "large_total" 0 tells the
    // self-check UI there's no PSRAM to show (self_check_readouts.cpp's
    // PerfSection), rather than leaving the key out and looking the same
    // as "unknown".
    platform.heap_info = [] { return wisp::Value(wisp::Map{{"large_total", int64_t{0}}}); };
    // No light sleep on a PC: log it, so a smoke test can see the hook fire.
    platform.set_light_sleep = [](bool on) {
        WLOG_I(kLogTag, "sim: light sleep %s", on ? "on" : "off");
    };
    platform.install_image_decoders = wisp::port::sim::install_image_decoders;
    platform.font_image = std::span<const uint8_t>(font_image);
    platform.glyph_image = std::span<const uint8_t>(glyph_image);
    platform.builtin_glyphs = opts.glyph_image != "none";
    platform.firmware_version = WISP_VERSION;
    platform.target = "simulator";
    // Uploads land in <data-dir>/firmware.bin; images must be built for
    // board "simulator" (connectivity spec 6.5).
    platform.firmware = std::make_unique<wisp::port::sim::SimFirmwareUpdater>(opts.data_dir);
    platform.random = [](uint8_t* out, size_t len) {
        // Read the OS entropy source on every call; this is only the
        // simulator, so the cost does not matter.
        std::random_device rd;
        for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(rd());
    };
    if (opts.http_port != 0) {
        platform.http_server = std::make_unique<wisp::port::sim::SimHttpServer>("127.0.0.1");
    }
    platform.http_client = std::make_unique<wisp::port::sim::SimHttpClient>();
    // The time sync runs as on the device; set_wall_clock stays empty
    // since the host clock is already right.
    platform.sntp = std::make_unique<wisp::port::sim::SimSntp>();
    platform.wall_s = [] {
        return static_cast<int64_t>(std::time(nullptr)) + g_clock_offset_s.load();
    };
    platform.assets = std::span<const uint8_t>(web_assets);
    platform.http_port = opts.http_port;
    platform.http_always_on = true;
    // Called from the admin server's reboot timer thread while every other
    // thread is still running, so it must not exit() here (static
    // destructors would race those threads): it only announces the reboot
    // and lets the main loop return through App::stop().
    platform.reboot = [&should_exit] {
        std::printf("WISP_REBOOT\n");
        std::fflush(stdout);
        should_exit.store(true);
    };

    // stdin lines that are not touches (sim_stdin.hpp).
    platform.read_line = [&console_lines](std::string& line, uint32_t timeout_ms) {
        return console_lines.read(line, timeout_ms);
    };
#ifdef WISP_SIM_DEBUG_COMMANDS
    platform.console_commands = debug_commands();
#endif
    // `bench scroll`, as on the device (wisp/app/bench.hpp).
    platform.console_commands.push_back(wisp::app::bench_command());
    if (!opts.scenes_dir.empty()) {
        // Absolute, so the scene paths in logs and errors say where they are.
        std::filesystem::path dir = std::filesystem::absolute(opts.scenes_dir, ec);
        if (ec || !std::filesystem::is_directory(dir, ec)) {
            std::fprintf(stderr, "wisp_sim: --scenes-dir %s is not a directory\n",
                         opts.scenes_dir.c_str());
            SDL_Quit();
            return 2;
        }
        platform.scenes_dir = dir.lexically_normal().string();
        if (platform.scenes_dir.size() > 1 && platform.scenes_dir.back() == '/') {
            platform.scenes_dir.pop_back();
        }
        platform.scenes_data_dir = opts.data_dir + "/scenes";
    }

    wisp::app::App app(std::move(platform));
    auto started = app.start();
    if (!started) {
        std::fprintf(stderr, "wisp_sim: App::start() failed: %s\n",
                     started.error().message.c_str());
        SDL_Quit();
        return 1;
    }

    if (!opts.open_scene.empty()) {
        wisp::scene::SceneRuntime* scenes = app.scenes();
        auto opened = scenes != nullptr
                          ? scenes->open(opts.open_scene)
                          : wisp::Result<void>::err(wisp::ErrorCode::Unsupported, "scenes are off");
        if (!opened) {
            WLOG_E(kLogTag, "--open-scene %s: %s", opts.open_scene.c_str(),
                   opened.error().message.c_str());
        }
    }

    if (opts.wisp_demo) start_wisp_demo(app);

    int exit_code = 0;
    {
        TouchScript touches(opts.touches, *input);
        // Declared after `touches`, so it stops before what it feeds.
        StdinReader stdin_reader(
            STDIN_FILENO, [&touches](const std::string& line) { touches.enqueue(line); },
            console_lines);
        if (opts.headless) {
            run_headless(app, opts, should_exit);
        } else if (!run_windowed(app, opts, *board_ptr, *input, should_exit)) {
            exit_code = 1;
        }
    }

    app.stop();
    SDL_Quit();
    return exit_code;
}
