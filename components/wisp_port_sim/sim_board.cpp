#include "wisp/port/sim/sim_board.hpp"

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
    auto record_disabled = [&](const std::string& id, wisp::hal::DeviceClass cls) {
        report.record({id, cls, wisp::core::DeviceState::Failed, "disabled by --disable"});
    };
    auto register_or_fail = [&](const std::shared_ptr<wisp::hal::Device>& device) {
        const auto& info = device->info();
        if (is_disabled(info.id, info.cls)) {
            record_disabled(info.id, info.cls);
            return;
        }
        registry.add(device);
    };

    if (is_disabled("display.main", wisp::hal::DeviceClass::Display)) {
        record_disabled("display.main", wisp::hal::DeviceClass::Display);
    } else {
        display_ = std::make_shared<SimDisplay>("display.main", kWidth, kHeight, config_.shape,
                                                kCornerRadius);
        registry.add(display_);
    }

    register_or_fail(std::make_shared<SimTouch>("touch.main", input_));
    register_or_fail(std::make_shared<SimImu>("imu.main", input_));
    register_or_fail(std::make_shared<SimPower>("power.main", on_power_off_));
    register_or_fail(std::make_shared<SimClock>("clock.main"));
    register_or_fail(std::make_shared<SimButton>("button.power", SimButton::Kind::Power, input_));
    register_or_fail(std::make_shared<SimButton>("button.vol_up", SimButton::Kind::VolUp, input_));
    register_or_fail(
        std::make_shared<SimButton>("button.vol_down", SimButton::Kind::VolDown, input_));
    register_or_fail(std::make_shared<SimAudioOut>("audio_out.main"));
    register_or_fail(std::make_shared<SimAudioIn>("audio_in.main"));

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
