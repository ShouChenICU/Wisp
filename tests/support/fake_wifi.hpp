#pragma once

// Test-only wisp::hal::Wifi double (see support/fake_devices.hpp for why
// test fakes may use std::mutex/std::atomic freely).
//
// Connection model: connect() decides the outcome immediately and the very
// next link() reports it -- there is no intermediate Connecting phase, so
// service tests can drive a full attempt with one tick. The outcome is:
//   - SSID not in `networks`                 -> Failed / NotFound
//   - `reachable` is false                   -> Failed / Other
//   - `passwords` has an entry for the SSID
//     and the password differs               -> Failed / AuthFailed
//   - otherwise                              -> Connected, ip 192.168.1.50,
//                                               rssi from the scan entry
// A SSID with no `passwords` entry accepts any password (an open network).
// While the radio is off (set_radio(false)) scan() and connect() fail with
// DeviceFailure and the link is Idle.
//
// Thread safety: the atomics may be read or written from any thread at any
// time. `networks`, `passwords` and `mac_bytes` are plain fields that tests
// configure before handing the fake to a service; the fake itself only
// reads them under its mutex, so change them mid-test only while no other
// thread is calling into the fake.

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/wifi.hpp"

namespace wisp::test {

class FakeWifi : public wisp::hal::Wifi {
public:
    explicit FakeWifi(std::string id) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "FAKE";
        info_.caps = wisp::Properties{
            {"ap", true},
            {"bands", wisp::List{wisp::Value("2.4GHz")}},
        };
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<std::vector<wisp::hal::WifiScanEntry>> scan() override {
        ++scan_calls;
        if (!radio_on.load()) {
            return wisp::Result<std::vector<wisp::hal::WifiScanEntry>>::err(
                wisp::ErrorCode::DeviceFailure, "radio off");
        }
        if (on_scan) {
            on_scan();  // outside mutex_, so the hook may call back into the fake
        }
        std::lock_guard<std::mutex> lock(mutex_);
        return wisp::Result<std::vector<wisp::hal::WifiScanEntry>>::ok(networks);
    }

    wisp::Result<void> connect(const std::string& ssid, const std::string& password) override {
        ++connect_calls;
        if (!radio_on.load()) {
            return wisp::Result<void>::err(wisp::ErrorCode::DeviceFailure, "radio off");
        }
        std::lock_guard<std::mutex> lock(mutex_);
        wisp::hal::WifiLink next;
        next.ssid = ssid;
        const wisp::hal::WifiScanEntry* entry = nullptr;
        for (const auto& n : networks) {
            if (n.ssid == ssid) {
                entry = &n;
                break;
            }
        }
        auto pw = passwords.find(ssid);
        if (entry == nullptr) {
            next.state = wisp::hal::WifiLinkState::Failed;
            next.reason = wisp::hal::WifiFailReason::NotFound;
        } else if (!reachable.load()) {
            next.state = wisp::hal::WifiLinkState::Failed;
            next.reason = wisp::hal::WifiFailReason::Other;
        } else if (pw != passwords.end() && pw->second != password) {
            next.state = wisp::hal::WifiLinkState::Failed;
            next.reason = wisp::hal::WifiFailReason::AuthFailed;
        } else {
            next.state = wisp::hal::WifiLinkState::Connected;
            next.ip = "192.168.1.50";
            next.rssi = entry->rssi;
        }
        link_ = std::move(next);
        return wisp::Result<void>::ok();
    }

    wisp::Result<void> disconnect() override {
        ++disconnect_calls;
        std::lock_guard<std::mutex> lock(mutex_);
        link_ = wisp::hal::WifiLink{};
        return wisp::Result<void>::ok();
    }

    wisp::hal::WifiLink link() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return link_;
    }

    wisp::Result<std::string> start_ap(const std::string& ssid,
                                       const std::string& password) override {
        std::lock_guard<std::mutex> lock(mutex_);
        ap_ssid_ = ssid;
        ap_password_ = password;
        ap_active.store(true);
        return wisp::Result<std::string>::ok("192.168.4.1");
    }

    wisp::Result<void> stop_ap() override {
        ap_active.store(false);
        return wisp::Result<void>::ok();
    }

    int ap_clients() override { return ap_client_count.load(); }

    std::array<uint8_t, 6> mac() override {
        std::lock_guard<std::mutex> lock(mutex_);
        return mac_bytes;
    }

    wisp::Result<void> set_power_save(bool on) override {
        power_save.store(on);
        return wisp::Result<void>::ok();
    }

    wisp::Result<void> set_radio(bool on) override {
        ++set_radio_calls;
        if (!on) {
            std::lock_guard<std::mutex> lock(mutex_);
            link_ = wisp::hal::WifiLink{};
            ap_active.store(false);
        }
        radio_on.store(on);
        return wisp::Result<void>::ok();
    }

    // Test control: overrides the current link, e.g. to simulate losing
    // the signal after a successful connect().
    void set_link(wisp::hal::WifiLink link) {
        std::lock_guard<std::mutex> lock(mutex_);
        link_ = std::move(link);
    }

    // Credentials passed to the most recent start_ap().
    std::string ap_ssid() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ap_ssid_;
    }
    std::string ap_password() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return ap_password_;
    }

    std::vector<wisp::hal::WifiScanEntry> networks;
    std::map<std::string, std::string> passwords;  // SSID -> correct password
    std::array<uint8_t, 6> mac_bytes{0x02, 0x00, 0x00, 0x00, 0x3F, 0x2A};
    // Called at the start of every scan(), on the scanning thread -- lets a
    // test act deterministically "while the radio is scanning". Set it
    // before handing the fake to a service, like the fields above.
    std::function<void()> on_scan;

    std::atomic<bool> reachable{true};
    std::atomic<int> ap_client_count{0};
    std::atomic<bool> ap_active{false};
    std::atomic<bool> power_save{false};
    std::atomic<int> connect_calls{0};
    std::atomic<int> disconnect_calls{0};
    std::atomic<int> scan_calls{0};
    std::atomic<bool> radio_on{true};
    std::atomic<int> set_radio_calls{0};

private:
    wisp::hal::DeviceInfo info_;

    mutable std::mutex mutex_;
    wisp::hal::WifiLink link_;
    std::string ap_ssid_;
    std::string ap_password_;
};

}  // namespace wisp::test
