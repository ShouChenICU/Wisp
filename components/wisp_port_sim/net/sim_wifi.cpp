#include "wisp/port/sim/sim_wifi.hpp"

#include <utility>

#include "wisp/base/value.hpp"

namespace wisp::port::sim {

namespace {

struct SimNetwork {
    const char* ssid;
    int rssi;
    const char* password;  // nullptr: open network
};

constexpr SimNetwork kNetworks[] = {
    {"WispLab", -45, "wisp1234"},
    {"Neighbor", -70, "neighbor99"},
    {"OpenCafe", -80, nullptr},
};

constexpr const char* kStationIp = "192.168.1.88";
constexpr const char* kApIp = "192.168.4.1";

}  // namespace

SimWifi::SimWifi(std::string id, bool fail_all) : fail_all_(fail_all) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sim-wifi";
    info_.caps = Properties{
        {"ap", true},
        {"bands", List{Value("2.4GHz")}},
    };
}

Result<std::vector<hal::WifiScanEntry>> SimWifi::scan() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!radio_on_) {
            return Result<std::vector<hal::WifiScanEntry>>::err(ErrorCode::DeviceFailure,
                                                                "radio off");
        }
    }
    std::vector<hal::WifiScanEntry> out;
    for (const SimNetwork& n : kNetworks) {
        out.push_back({n.ssid, n.rssi, n.password != nullptr});
    }
    return Result<std::vector<hal::WifiScanEntry>>::ok(std::move(out));
}

Result<void> SimWifi::connect(const std::string& ssid, const std::string& password) {
    const SimNetwork* found = nullptr;
    for (const SimNetwork& n : kNetworks) {
        if (ssid == n.ssid) found = &n;
    }
    hal::WifiLink next;
    next.ssid = ssid;
    if (fail_all_ || found == nullptr) {
        next.state = hal::WifiLinkState::Failed;
        next.reason = hal::WifiFailReason::NotFound;
    } else if (found->password != nullptr && password != found->password) {
        next.state = hal::WifiLinkState::Failed;
        next.reason = hal::WifiFailReason::AuthFailed;
    } else {
        next.state = hal::WifiLinkState::Connected;
        next.ip = kStationIp;
        next.rssi = found->rssi;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!radio_on_) return Result<void>::err(ErrorCode::DeviceFailure, "radio off");
    link_ = std::move(next);
    return Result<void>::ok();
}

Result<void> SimWifi::disconnect() {
    std::lock_guard<std::mutex> lock(mutex_);
    link_ = hal::WifiLink{};
    return Result<void>::ok();
}

hal::WifiLink SimWifi::link() {
    std::lock_guard<std::mutex> lock(mutex_);
    return link_;
}

Result<std::string> SimWifi::start_ap(const std::string&, const std::string&) {
    std::lock_guard<std::mutex> lock(mutex_);
    ap_active_ = true;
    return Result<std::string>::ok(kApIp);
}

Result<void> SimWifi::stop_ap() {
    std::lock_guard<std::mutex> lock(mutex_);
    ap_active_ = false;
    return Result<void>::ok();
}

int SimWifi::ap_clients() {
    std::lock_guard<std::mutex> lock(mutex_);
    return ap_active_ ? 1 : 0;
}

std::array<uint8_t, 6> SimWifi::mac() {
    // Locally administered address, so it can never clash with real hardware.
    return {0x02, 0x57, 0x49, 0x53, 0x50, 0x01};
}

Result<void> SimWifi::set_power_save(bool) { return Result<void>::ok(); }

Result<void> SimWifi::set_radio(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!on) {
        link_ = hal::WifiLink{};
        ap_active_ = false;
    }
    radio_on_ = on;
    return Result<void>::ok();
}

}  // namespace wisp::port::sim
