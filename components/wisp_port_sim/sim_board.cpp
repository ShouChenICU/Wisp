#include "wisp/port/sim/sim_board.hpp"

#include <SDL.h>

#include <filesystem>
#include <utility>

#include "dev/sim_devices.hpp"
#include "wisp/core/log.hpp"

namespace wisp::port::sim {

namespace {

constexpr const char* kTag = "sim_board";
constexpr int32_t kWidth = 480;
constexpr int32_t kHeight = 480;
constexpr int32_t kCornerRadius = 40;
// storage.int's total matches spec 7.4's "storage" (LittleFS) partition
// size, 0x5E0000 bytes.
constexpr uint64_t kIntTotalBytes = 6160384;
constexpr uint64_t kSdTotalBytes = 32ull * 1024 * 1024 * 1024;

// True (marking `disabled[idx]` as matched in `used`) if some entry of
// `disabled` names `id` or `cls` exactly -- task-13-brief.md's
// "--disable 匹配设备 id 或类别名" rule, e.g. "--disable imu" matches
// "imu.main" (class DeviceClass::Imu) and "--disable touch.main" matches
// only that one device.
bool matches_disable(const std::string& id, wisp::hal::DeviceClass cls,
                     const std::vector<std::string>& disabled, std::vector<bool>& used) {
    for (size_t i = 0; i < disabled.size(); ++i) {
        if (disabled[i] == id || disabled[i] == wisp::hal::to_string(cls)) {
            used[i] = true;
            return true;
        }
    }
    return false;
}

}  // namespace

SimBoard::SimBoard(SimBoardConfig config, std::shared_ptr<SimInput> input,
                   std::function<void()> on_power_off)
    : config_(std::move(config)),
      input_(std::move(input)),
      on_power_off_(std::move(on_power_off)) {}

void SimBoard::init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) {
    std::vector<bool> used(config_.disabled.size(), false);

    auto is_disabled = [&](const std::string& id, wisp::hal::DeviceClass cls) {
        return matches_disable(id, cls, config_.disabled, used);
    };
    auto record_disabled = [&](const std::string& id, wisp::hal::DeviceClass cls,
                               const std::string& reason = "disabled by --disable") {
        report.record({id, cls, wisp::core::DeviceState::Failed, reason});
    };
    // Checks --disable *before* constructing the device (fix round 1,
    // item 10): avoids doing any device-construction work for an id the
    // caller explicitly disabled, rather than building it and throwing
    // it away.
    using Factory = std::function<std::shared_ptr<wisp::hal::Device>()>;
    auto make_or_fail = [&](const std::string& id, wisp::hal::DeviceClass cls,
                            const Factory& factory) {
        if (is_disabled(id, cls)) {
            record_disabled(id, cls);
            return;
        }
        registry.add(factory());
    };

    if (is_disabled("display.main", wisp::hal::DeviceClass::Display)) {
        record_disabled("display.main", wisp::hal::DeviceClass::Display);
    } else {
        display_ = std::make_shared<SimDisplay>("display.main", kWidth, kHeight, config_.shape,
                                                kCornerRadius);
        registry.add(display_);
    }

    make_or_fail("touch.main", wisp::hal::DeviceClass::Touch,
                 [&] { return std::make_shared<SimTouch>("touch.main", input_); });
    make_or_fail("imu.main", wisp::hal::DeviceClass::Imu,
                 [&] { return std::make_shared<SimImu>("imu.main", input_); });
    make_or_fail("power.main", wisp::hal::DeviceClass::Power,
                 [&] { return std::make_shared<SimPower>("power.main", on_power_off_); });
    make_or_fail("clock.main", wisp::hal::DeviceClass::Clock,
                 [&] { return std::make_shared<SimClock>("clock.main"); });
    make_or_fail("button.power", wisp::hal::DeviceClass::Button, [&] {
        return std::make_shared<SimButton>("button.power", SimButton::Kind::Power, input_);
    });
    make_or_fail("button.vol_up", wisp::hal::DeviceClass::Button, [&] {
        return std::make_shared<SimButton>("button.vol_up", SimButton::Kind::VolUp, input_);
    });
    make_or_fail("button.vol_down", wisp::hal::DeviceClass::Button, [&] {
        return std::make_shared<SimButton>("button.vol_down", SimButton::Kind::VolDown, input_);
    });
    make_or_fail("audio_out.main", wisp::hal::DeviceClass::AudioOut,
                 [&] { return std::make_shared<SimAudioOut>("audio_out.main"); });

    // audio_in.main needs a real SDL capture device; the "dummy" driver
    // the simulator selects in --headless mode (and some hosts) have
    // none. Rather than letting SimAudioIn::open() fail at "录音回放"
    // time with no explanation, probe up front (fix round 1, item 1)
    // and simply don't register the device if there's nothing to
    // capture from -- a missing device already degrades the rest of
    // the system gracefully instead of crashing (see board.hpp).
    if (is_disabled("audio_in.main", wisp::hal::DeviceClass::AudioIn)) {
        record_disabled("audio_in.main", wisp::hal::DeviceClass::AudioIn);
    } else if (SDL_GetNumAudioDevices(/*iscapture=*/1) <= 0) {
        record_disabled("audio_in.main", wisp::hal::DeviceClass::AudioIn, "no capture device");
    } else {
        registry.add(std::make_shared<SimAudioIn>("audio_in.main"));
    }

    std::error_code ec;
    std::filesystem::create_directories(config_.data_dir + "/int", ec);
    std::filesystem::create_directories(config_.data_dir + "/sd", ec);

    if (is_disabled("storage.int", wisp::hal::DeviceClass::Storage)) {
        record_disabled("storage.int", wisp::hal::DeviceClass::Storage);
    } else {
        registry.add(std::make_shared<SimStorage>("storage.int", config_.data_dir + "/int",
                                                  /*removable=*/false, "littlefs", kIntTotalBytes,
                                                  nullptr));
    }

    if (is_disabled("storage.sd", wisp::hal::DeviceClass::Storage)) {
        record_disabled("storage.sd", wisp::hal::DeviceClass::Storage);
    } else {
        auto input = input_;
        auto sd_storage = std::make_shared<SimStorage>("storage.sd", config_.data_dir + "/sd",
                                                       /*removable=*/true, "fat", kSdTotalBytes,
                                                       [input] { return input->sd_present(); });
        registry.add(sd_storage);
        if (!sd_storage->online()) {
            report.record({"storage.sd", wisp::hal::DeviceClass::Storage,
                           wisp::core::DeviceState::Missing, "no SD card"});
        }
    }

    for (size_t i = 0; i < config_.disabled.size(); ++i) {
        if (!used[i]) {
            WLOG_W(kTag, "--disable %s matched no device", config_.disabled[i].c_str());
        }
    }
}

}  // namespace wisp::port::sim
