#pragma once

// hal::Wifi on the ESP-IDF Wi-Fi driver (connectivity spec 2.1, 4.2,
// 4.3). The board constructs it during init and registers it as
// "net.wifi".
//
// create() brings up esp_netif (default station + access-point
// interfaces), the default event loop and the driver in station mode, and
// starts the radio right away: Platform::random (esp_fill_random) is only
// a true RNG while the RF subsystem is on, and every caller of it depends
// on net.wifi being registered (firmware/main/main.cpp). The driver keeps
// its configuration in RAM only (WIFI_STORAGE_RAM): WifiService owns the
// saved networks, so credentials are never written to NVS by the IDF.
//
// Poll-based, per hal::Wifi: the driver's event handlers only update a
// snapshot under a mutex, which link() copies; nothing here ever calls
// into service code. connect() never blocks -- it records the attempt
// (link() reports Connecting from that moment), (re)configures the
// station and returns; it may be called while another attempt is in
// flight, which it replaces. WifiService calls connect(), disconnect(),
// start_ap() and stop_ap() under its own mutex, so none of them waits on
// the network.
//
// The hotspot (start_ap) switches the driver to AP+STA: WPA2-PSK, channel
// 1 (the station's channel instead while it is connected; one radio), at
// most 4 clients, and a captive DNS responder that answers every name
// with the hotspot's address. stop_ap() returns to station-only mode.
//
// mDNS: the first time link() sees the station connected it starts the
// mDNS responder as `<hostname>.local` advertising _http._tcp on the
// admin port; the responder follows interface changes on its own from
// then on.
//
// SSIDs are arbitrary bytes (up to 32, not necessarily UTF-8) and are
// passed through unchanged; passwords are never logged.

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/wifi.hpp"

namespace wisp::port::esp {

struct EspWifiConfig {
    // DeviceInfo id/model (board-provided; e.g. "net.wifi", the chip name).
    std::string id = "net.wifi";
    std::string model;
    // The caps "bands" list: what the chip's radio supports.
    std::vector<std::string> bands = {"2.4GHz"};
    // DHCP and mDNS host name (http://wisp.local:8900, spec 6.1).
    std::string hostname = "wisp";
    // The admin backend's port, advertised over mDNS
    // (wisp::app::Platform::http_port).
    uint16_t http_port = 8900;
};

class EspWifi : public hal::Wifi {
public:
    // Fails (with the IDF error named in the message) when the network
    // stack or the driver cannot be brought up; the board then records
    // net.wifi as Failed and boot continues without networking. Call at
    // most once per boot: the default netifs and the driver are global.
    static Result<std::shared_ptr<EspWifi>> create(EspWifiConfig config);

    ~EspWifi() override;

    EspWifi(const EspWifi&) = delete;
    EspWifi& operator=(const EspWifi&) = delete;

    const hal::DeviceInfo& info() const override;

    Result<std::vector<hal::WifiScanEntry>> scan() override;
    Result<void> connect(const std::string& ssid, const std::string& password) override;
    Result<void> disconnect() override;
    hal::WifiLink link() override;
    Result<std::string> start_ap(const std::string& ssid, const std::string& password) override;
    Result<void> stop_ap() override;
    int ap_clients() override;
    std::array<uint8_t, 6> mac() override;
    Result<void> set_power_save(bool on) override;

    struct Impl;

private:
    explicit EspWifi(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

}  // namespace wisp::port::esp
