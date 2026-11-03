#pragma once

// Simulator hal::Wifi ("net.wifi", model "sim-wifi"; connectivity spec
// 4.5). No radio is involved: three built-in networks can be scanned and
// joined --
//   WispLab   -45 dBm  password "wisp1234"
//   Neighbor  -70 dBm  password "neighbor99"
//   OpenCafe  -80 dBm  open
// -- and connect() decides the outcome at once (wrong password ->
// AuthFailed, unknown SSID -> NotFound), which the next link() reports.
// With `fail_all` (--wifi-fail) every attempt fails with NotFound, as if
// the router were unreachable, to exercise WifiService's backoff.
//
// The hotspot only changes state; while it is up ap_clients() reports one
// station, as if a phone had already joined, so the hotspot's idle timeout
// does not close it under a developer testing the setup pages.
//
// Thread-safe: WifiService calls in from several threads.

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/wifi.hpp"

namespace wisp::port::sim {

class SimWifi : public hal::Wifi {
public:
    explicit SimWifi(std::string id, bool fail_all = false);

    const hal::DeviceInfo& info() const override { return info_; }

    Result<std::vector<hal::WifiScanEntry>> scan() override;
    Result<void> connect(const std::string& ssid, const std::string& password) override;
    Result<void> disconnect() override;
    hal::WifiLink link() override;
    Result<std::string> start_ap(const std::string& ssid, const std::string& password) override;
    Result<void> stop_ap() override;
    int ap_clients() override;
    std::array<uint8_t, 6> mac() override;
    Result<void> set_power_save(bool on) override;

private:
    hal::DeviceInfo info_;
    const bool fail_all_;

    std::mutex mutex_;
    hal::WifiLink link_;
    bool ap_active_ = false;
};

}  // namespace wisp::port::sim
