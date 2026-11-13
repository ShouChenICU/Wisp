#include "wisp/port/esp/esp_wifi.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

#include "captive_dns.hpp"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/esp/wifi_reason.hpp"

namespace wisp::port::esp {

// wifi_reason.hpp spells these out so it can be tested on the host.
static_assert(kReasonAssocLeave == WIFI_REASON_ASSOC_LEAVE);
static_assert(kReason4wayHandshakeTimeout == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);
static_assert(kReason8021xAuthFailed == WIFI_REASON_802_1X_AUTH_FAILED);
static_assert(kReasonNoApFound == WIFI_REASON_NO_AP_FOUND);
static_assert(kReasonAuthFail == WIFI_REASON_AUTH_FAIL);
static_assert(kReasonHandshakeTimeout == WIFI_REASON_HANDSHAKE_TIMEOUT);
static_assert(kReasonNoApFoundCompatibleSecurity == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY);
static_assert(kReasonNoApFoundInAuthmodeThreshold == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD);
static_assert(kReasonNoApFoundInRssiThreshold == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD);

namespace {

constexpr const char* kTag = "esp_wifi";

// Spec 4.3: the hotspot runs on channel 1 with room for a few phones.
constexpr uint8_t kApChannel = 1;
constexpr uint8_t kApMaxClients = 4;
// Scan results kept; a crowded neighbourhood can report far more.
constexpr uint16_t kMaxScanRecords = 32;

constexpr size_t kMaxSsid = 32;
constexpr size_t kMinPassphrase = 8;
constexpr size_t kMaxPassphrase = 63;
// A raw PSK is given as exactly 64 hex digits.
constexpr size_t kRawPskLen = 64;

Result<void> fail(ErrorCode code, const char* what, esp_err_t err) {
    return Result<void>::err(code, std::string(what) + ": " + esp_err_to_name(err));
}

ErrorCode code_for(esp_err_t err) {
    switch (err) {
        case ESP_ERR_NO_MEM:
            return ErrorCode::NoMemory;
        case ESP_ERR_INVALID_ARG:
            return ErrorCode::InvalidArgument;
        case ESP_ERR_WIFI_STATE:
            return ErrorCode::Busy;
        case ESP_ERR_WIFI_NOT_INIT:
        case ESP_ERR_WIFI_NOT_STARTED:
            return ErrorCode::DeviceFailure;
        default:
            return ErrorCode::IoError;
    }
}

bool valid_ssid(const std::string& ssid) {
    // The driver's SSID fields are fixed 32-byte arrays read up to the
    // first NUL, so an embedded NUL could not be represented.
    return !ssid.empty() && ssid.size() <= kMaxSsid && ssid.find('\0') == std::string::npos;
}

bool is_hex(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// Empty (an open network), a WPA passphrase, or a raw 64-hex-digit PSK.
bool valid_sta_password(const std::string& pw) {
    if (pw.empty()) return true;
    if (pw.find('\0') != std::string::npos) return false;
    if (pw.size() == kRawPskLen) return is_hex(pw);
    return pw.size() >= kMinPassphrase && pw.size() <= kMaxPassphrase;
}

std::string ip_string(uint32_t network_order) {
    char buf[16];
    esp_ip4_addr_t addr = {network_order};
    esp_ip4addr_ntoa(&addr, buf, sizeof(buf));
    return buf;
}

std::string mac_string(const uint8_t* mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    return buf;
}

hal::WifiDetails ap_details(const wifi_ap_record_t& ap) {
    hal::WifiDetails details;
    details.bssid = mac_string(ap.bssid);
    details.channel = ap.primary;
    switch (ap.authmode) {
        case WIFI_AUTH_OPEN:
            details.security = "Open";
            break;
        case WIFI_AUTH_WEP:
            details.security = "WEP";
            break;
        case WIFI_AUTH_WPA_PSK:
            details.security = "WPA-Personal";
            break;
        case WIFI_AUTH_WPA2_PSK:
            details.security = "WPA2-Personal";
            break;
        case WIFI_AUTH_WPA_WPA2_PSK:
            details.security = "WPA/WPA2-Personal";
            break;
        case WIFI_AUTH_WPA2_ENTERPRISE:
            details.security = "WPA2-Enterprise";
            break;
        case WIFI_AUTH_WPA3_PSK:
            details.security = "WPA3-Personal";
            break;
        case WIFI_AUTH_WPA2_WPA3_PSK:
            details.security = "WPA2/WPA3-Personal";
            break;
        case WIFI_AUTH_OWE:
            details.security = "OWE";
            break;
        default:
            break;
    }
    return details;
}

// Zeroes a config that held a password before it leaves the stack.
void wipe(wifi_config_t& cfg) {
    volatile uint8_t* p = reinterpret_cast<volatile uint8_t*>(&cfg);
    for (size_t i = 0; i < sizeof(cfg); ++i) p[i] = 0;
}

// esp_netif_create_default_wifi_sta()/_ap() without their internal
// abort-on-failure: a netif that cannot be created leaves net.wifi
// failed and the rest of the device running (AGENTS.md section 3).
esp_err_t create_wifi_netif(bool ap, esp_netif_t** out) {
    *out = nullptr;
    const esp_netif_config_t ap_cfg = ESP_NETIF_DEFAULT_WIFI_AP();
    const esp_netif_config_t sta_cfg = ESP_NETIF_DEFAULT_WIFI_STA();
    esp_netif_t* netif = esp_netif_new(ap ? &ap_cfg : &sta_cfg);
    if (netif == nullptr) return ESP_ERR_NO_MEM;
    esp_err_t err = ap ? esp_netif_attach_wifi_ap(netif) : esp_netif_attach_wifi_station(netif);
    if (err == ESP_OK) {
        err =
            ap ? esp_wifi_set_default_wifi_ap_handlers() : esp_wifi_set_default_wifi_sta_handlers();
    }
    if (err != ESP_OK) {
        esp_netif_destroy_default_wifi(netif);
        return err;
    }
    *out = netif;
    return ESP_OK;
}

// Leaves the access point interface with a hidden WPA2 configuration and
// a throwaway passphrase, then returns to station-only mode. The driver
// keeps an interface's configuration across mode changes, and
// esp_wifi_set_config(WIFI_IF_AP) is only accepted while the AP
// interface is enabled, so start_ap() has to switch to AP+STA first and
// the driver briefly beacons whatever the AP interface last held: the
// default open "ESP_xxxxxx" network on a fresh driver, or the previous
// hotspot with a password that was shown on screen. Called before the
// radio starts (no beacon then) and when the hotspot stops. At the first
// call the RNG is not yet seeded by the radio; the passphrase is never
// shown or used, it only has to be unguessable for that brief window.
esp_err_t install_placeholder_ap() {
    static constexpr char kSsid[] = "wisp-off";
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        wifi_config_t ac = {};
        std::memcpy(ac.ap.ssid, kSsid, sizeof(kSsid) - 1);
        ac.ap.ssid_len = sizeof(kSsid) - 1;
        uint8_t raw[kMaxPassphrase];
        esp_fill_random(raw, sizeof(raw));
        for (size_t i = 0; i < kMaxPassphrase; ++i) {
            ac.ap.password[i] = static_cast<uint8_t>(kAlphabet[raw[i] % (sizeof(kAlphabet) - 1)]);
        }
        ac.ap.password[kMaxPassphrase] = 0;
        std::memset(raw, 0, sizeof(raw));
        ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ac.ap.ssid_hidden = 1;
        ac.ap.channel = kApChannel;
        ac.ap.max_connection = kApMaxClients;
        err = esp_wifi_set_config(WIFI_IF_AP, &ac);
        wipe(ac);
    }
    const esp_err_t back = esp_wifi_set_mode(WIFI_MODE_STA);
    return err != ESP_OK ? err : back;
}

}  // namespace

struct EspWifi::Impl {
    EspWifiConfig config;
    hal::DeviceInfo info;
    esp_netif_t* sta = nullptr;
    esp_netif_t* ap = nullptr;
    esp_event_handler_instance_t wifi_handler = nullptr;
    esp_event_handler_instance_t ip_handler = nullptr;
    bool driver_ready = false;

    // Written by the driver's event handlers (default event loop task) and
    // by the calls below; link() copies it.
    std::mutex mutex;
    hal::WifiLink link;
    // The SSID of the attempt in flight or of the current connection.
    std::string attempt_ssid;
    // The last scan and when it finished (os::now_ms()), for an
    // authentication failure reported without a signal reading.
    std::vector<hal::WifiScanEntry> last_scan;
    uint64_t last_scan_ms = 0;

    // Only touched from start_ap()/stop_ap()/set_radio(), which WifiService
    // serializes.
    CaptiveDns dns;
    // The AP interface may still hold the last hotspot's configuration
    // (its password was on screen) because replacing it with the
    // placeholder was refused; the next start_ap() then configures the
    // interface with the radio stopped, so that configuration never beacons.
    bool ap_config_stale = false;
    // Only touched from link(), which WifiService calls from its one tick.
    bool mdns_started = false;
    // Only touched from set_radio(), which WifiService serializes.
    bool radio_on = true;

    static void on_wifi_event(void* arg, esp_event_base_t, int32_t id, void* data);
    static void on_ip_event(void* arg, esp_event_base_t, int32_t id, void* data);
    void start_mdns();
    void teardown();

    // Installs the placeholder AP configuration and returns to station
    // mode; if the driver refuses the placeholder, remembers that the
    // interface still holds a real one.
    void park_ap() {
        const esp_err_t err = install_placeholder_ap();
        if (err != ESP_OK) {
            ap_config_stale = true;
            WLOG_W(kTag, "hotspot configuration not replaced (%s); reconfigured on next start",
                   esp_err_to_name(err));
        }
    }
    esp_err_t revert_to_station() { return esp_wifi_set_mode(WIFI_MODE_STA); }
};

void EspWifi::Impl::on_wifi_event(void* arg, esp_event_base_t, int32_t id, void* data) {
    auto* self = static_cast<Impl*>(arg);
    if (id != WIFI_EVENT_STA_DISCONNECTED || data == nullptr) return;
    const auto* ev = static_cast<const wifi_event_sta_disconnected_t*>(data);
    enum class Outcome { None, Failed, Lost } outcome = Outcome::None;
    int scan_rssi = 0;
    {
        std::lock_guard<std::mutex> lock(self->mutex);
        hal::WifiLink& link = self->link;
        // connect() disconnects whatever it replaces; that disconnect is
        // reported later and is not the new attempt's outcome.
        if (link.state == hal::WifiLinkState::Connecting && !is_own_leave(ev->reason)) {
            link.state = hal::WifiLinkState::Failed;
            link.ssid.assign(reinterpret_cast<const char*>(ev->ssid),
                             std::min<size_t>(ev->ssid_len, kMaxSsid));
            if (ev->rssi == 0) {
                scan_rssi =
                    scan_rssi_for(self->last_scan, link.ssid, os::now_ms() - self->last_scan_ms);
            }
            link.reason = fail_reason_for(ev->reason, ev->rssi, scan_rssi);
            link.ip.clear();
            link.rssi = ev->rssi != 0 ? ev->rssi : scan_rssi;
            outcome = Outcome::Failed;
        } else if (link.state == hal::WifiLinkState::Connected) {
            link = hal::WifiLink{};
            outcome = Outcome::Lost;
        }
    }
    // Only the numeric reason, outside the lock: never anything that came
    // with the credentials.
    if (outcome == Outcome::Failed) {
        WLOG_I(kTag, "connect attempt failed: reason %u rssi %d (scan %d)",
               static_cast<unsigned>(ev->reason), static_cast<int>(ev->rssi), scan_rssi);
    } else if (outcome == Outcome::Lost) {
        WLOG_I(kTag, "station disconnected: reason %u", static_cast<unsigned>(ev->reason));
    }
}

void EspWifi::Impl::on_ip_event(void* arg, esp_event_base_t, int32_t id, void* data) {
    auto* self = static_cast<Impl*>(arg);
    std::lock_guard<std::mutex> lock(self->mutex);
    hal::WifiLink& link = self->link;
    if (id == IP_EVENT_STA_GOT_IP && data != nullptr) {
        // Late news for an attempt that has since been cancelled.
        if (link.state != hal::WifiLinkState::Connecting &&
            link.state != hal::WifiLinkState::Connected) {
            return;
        }
        const auto* ev = static_cast<const ip_event_got_ip_t*>(data);
        link.state = hal::WifiLinkState::Connected;
        link.ssid = self->attempt_ssid;
        link.ip = ip_string(ev->ip_info.ip.addr);
        link.reason = hal::WifiFailReason::None;
    } else if (id == IP_EVENT_STA_LOST_IP) {
        if (link.state == hal::WifiLinkState::Connected) link = hal::WifiLink{};
    }
}

void EspWifi::Impl::start_mdns() {
    if (mdns_started) return;
    mdns_started = true;  // one try per boot; a failure is logged, not retried
    esp_err_t err = mdns_init();
    if (err == ESP_OK) err = mdns_hostname_set(config.hostname.c_str());
    if (err == ESP_OK) err = mdns_instance_name_set("Wisp");
    if (err == ESP_OK)
        err = mdns_service_add(nullptr, "_http", "_tcp", config.http_port, nullptr, 0);
    if (err != ESP_OK) {
        WLOG_W(kTag, "mDNS unavailable: %s", esp_err_to_name(err));
        return;
    }
    WLOG_I(kTag, "mDNS: %s.local, _http._tcp port %u", config.hostname.c_str(),
           static_cast<unsigned>(config.http_port));
}

void EspWifi::Impl::teardown() {
    dns.stop();
    if (wifi_handler != nullptr) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler);
        wifi_handler = nullptr;
    }
    if (ip_handler != nullptr) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_handler);
        ip_handler = nullptr;
    }
    if (driver_ready) {
        esp_wifi_stop();
        esp_wifi_deinit();
        driver_ready = false;
    }
    if (sta != nullptr) {
        esp_netif_destroy_default_wifi(sta);
        sta = nullptr;
    }
    if (ap != nullptr) {
        esp_netif_destroy_default_wifi(ap);
        ap = nullptr;
    }
}

Result<std::shared_ptr<EspWifi>> EspWifi::create(EspWifiConfig config) {
    using R = Result<std::shared_ptr<EspWifi>>;
    auto impl = std::make_unique<Impl>();
    impl->info.id = config.id;
    impl->info.cls = kClass;
    impl->info.model = config.model;
    List bands;
    for (const std::string& b : config.bands) bands.emplace_back(b);
    impl->info.caps = Properties{{"ap", true}, {"bands", std::move(bands)}};
    impl->config = std::move(config);

    const auto failed = [&impl](const char* what, esp_err_t err) {
        impl->teardown();
        return R::err(code_for(err), std::string(what) + ": " + esp_err_to_name(err));
    };

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) return failed("esp_netif_init", err);
    err = esp_event_loop_create_default();
    // Someone else (e.g. a board component) may have created it already.
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return failed("esp_event_loop_create_default", err);
    }
    err = create_wifi_netif(/*ap=*/false, &impl->sta);
    if (err != ESP_OK) return failed("station netif", err);
    err = create_wifi_netif(/*ap=*/true, &impl->ap);
    if (err != ESP_OK) return failed("access point netif", err);
    esp_netif_set_hostname(impl->sta, impl->config.hostname.c_str());

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) return failed("esp_wifi_init", err);
    impl->driver_ready = true;
    // WifiService owns credential persistence.
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) return failed("esp_wifi_set_storage", err);

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &Impl::on_wifi_event,
                                              impl.get(), &impl->wifi_handler);
    if (err != ESP_OK) return failed("register WIFI_EVENT", err);
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &Impl::on_ip_event,
                                              impl.get(), &impl->ip_handler);
    if (err != ESP_OK) return failed("register IP_EVENT", err);

    // Station only until the hotspot is asked for, and the AP interface
    // pre-configured so that switching to AP+STA never exposes an open
    // network.
    err = install_placeholder_ap();
    if (err != ESP_OK) return failed("access point placeholder", err);
    // Radio on now: esp_fill_random() is a true RNG only from here on.
    err = esp_wifi_start();
    if (err != ESP_OK) return failed("esp_wifi_start", err);

    WLOG_I(kTag, "radio up (station mode)");
    return R::ok(std::shared_ptr<EspWifi>(new EspWifi(std::move(impl))));
}

EspWifi::EspWifi(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

EspWifi::~EspWifi() { impl_->teardown(); }

const hal::DeviceInfo& EspWifi::info() const { return impl_->info; }

Result<std::vector<hal::WifiScanEntry>> EspWifi::scan() {
    using R = Result<std::vector<hal::WifiScanEntry>>;
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_err_t err = esp_wifi_scan_start(&sc, /*block=*/true);
    if (err != ESP_OK) {
        // ESP_ERR_WIFI_STATE: the station is busy connecting.
        return R::err(code_for(err), std::string("esp_wifi_scan_start: ") + esp_err_to_name(err));
    }
    uint16_t count = kMaxScanRecords;
    std::vector<wifi_ap_record_t> records(count);
    // Also frees the driver's copy of the list, including the records
    // beyond `count`.
    err = esp_wifi_scan_get_ap_records(&count, records.data());
    if (err != ESP_OK) {
        return R::err(code_for(err),
                      std::string("esp_wifi_scan_get_ap_records: ") + esp_err_to_name(err));
    }
    std::vector<hal::WifiScanEntry> out;
    out.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        const wifi_ap_record_t& r = records[i];
        const auto* ssid = reinterpret_cast<const char*>(r.ssid);
        hal::WifiScanEntry e;
        e.ssid.assign(ssid, strnlen(ssid, kMaxSsid));
        e.rssi = r.rssi;
        e.details = ap_details(r);
        // OWE ("enhanced open") needs no password either.
        e.secured = r.authmode != WIFI_AUTH_OPEN && r.authmode != WIFI_AUTH_OWE;
        out.push_back(std::move(e));
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->last_scan = out;
        impl_->last_scan_ms = os::now_ms();
    }
    return R::ok(std::move(out));
}

Result<void> EspWifi::connect(const std::string& ssid, const std::string& password) {
    if (!valid_ssid(ssid)) {
        return Result<void>::err(ErrorCode::InvalidArgument, "SSID must be 1-32 bytes");
    }
    if (!valid_sta_password(password)) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "password must be 8-63 characters or 64 hex digits");
    }

    wifi_config_t wc = {};
    std::memcpy(wc.sta.ssid, ssid.data(), ssid.size());
    std::memcpy(wc.sta.password, password.data(), password.size());
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    // With a password, nothing weaker than WPA2 (WPA2/WPA3 mixes pass);
    // an older network fails as "not found" rather than silently
    // downgrading.
    wc.sta.threshold.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    bool replacing = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        replacing = impl_->link.state == hal::WifiLinkState::Connecting ||
                    impl_->link.state == hal::WifiLinkState::Connected;
        impl_->link = hal::WifiLink{};
        impl_->link.state = hal::WifiLinkState::Connecting;
        impl_->link.ssid = ssid;
        impl_->attempt_ssid = ssid;
    }
    if (replacing) esp_wifi_disconnect();

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    wipe(wc);
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->link = hal::WifiLink{};
        return fail(code_for(err), "esp_wifi_connect", err);
    }
    return Result<void>::ok();
}

Result<void> EspWifi::disconnect() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->link = hal::WifiLink{};
        impl_->attempt_ssid.clear();
    }
    const esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        return fail(code_for(err), "esp_wifi_disconnect", err);
    }
    return Result<void>::ok();
}

hal::WifiLink EspWifi::link() {
    hal::WifiLink out;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        out = impl_->link;
    }
    if (out.state == hal::WifiLinkState::Connected) {
        wifi_ap_record_t ap = {};
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            out.rssi = ap.rssi;
            out.details = ap_details(ap);
        }
        esp_netif_ip_info_t ip{};
        if (esp_netif_get_ip_info(impl_->sta, &ip) == ESP_OK) {
            out.details.gateway = ip_string(ip.gw.addr);
            out.details.netmask = ip_string(ip.netmask.addr);
        }
        esp_netif_dns_info_t dns{};
        if (esp_netif_get_dns_info(impl_->sta, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
            dns.ip.type == ESP_IPADDR_TYPE_V4) {
            out.details.dns = ip_string(dns.ip.u_addr.ip4.addr);
        }
        uint8_t mac[6]{};
        if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) out.details.mac = mac_string(mac);
        // Started here rather than from the event handler: mdns_init()
        // creates a task and registers handlers, more than the small
        // default event loop stack should be asked to carry. The caller
        // (WifiService's tick) polls link() a few times a second.
        impl_->start_mdns();
    }
    return out;
}

Result<std::string> EspWifi::start_ap(const std::string& ssid, const std::string& password) {
    using R = Result<std::string>;
    if (!valid_ssid(ssid)) return R::err(ErrorCode::InvalidArgument, "SSID must be 1-32 bytes");
    if (password.size() < kMinPassphrase || password.size() > kMaxPassphrase ||
        password.find('\0') != std::string::npos) {
        return R::err(ErrorCode::InvalidArgument, "password must be 8-63 characters");
    }

    wifi_config_t ac = {};
    std::memcpy(ac.ap.ssid, ssid.data(), ssid.size());
    ac.ap.ssid_len = static_cast<uint8_t>(ssid.size());
    std::memcpy(ac.ap.password, password.data(), password.size());
    ac.ap.channel = kApChannel;
    ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ac.ap.max_connection = kApMaxClients;

    // Restarting the radio drops the station; WifiService sees the link
    // go idle (a reconnect, or an attempt that times out and is retried).
    const bool restart = impl_->ap_config_stale;
    if (restart) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->link = hal::WifiLink{};
            impl_->attempt_ssid.clear();
        }
        esp_wifi_stop();
    }
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ac);
    wipe(ac);
    if (err == ESP_OK) impl_->ap_config_stale = false;
    if (restart) {
        if (err != ESP_OK) esp_wifi_set_mode(WIFI_MODE_STA);
        const esp_err_t started = esp_wifi_start();
        if (err == ESP_OK) err = started;
    }
    if (err != ESP_OK) {
        // The AP interface was not given the new configuration (or the
        // radio did not come back), so it holds what it held before.
        impl_->revert_to_station();
        return R::err(code_for(err),
                      std::string("starting the access point: ") + esp_err_to_name(err));
    }

    esp_netif_ip_info_t ip = {};
    err = esp_netif_get_ip_info(impl_->ap, &ip);
    if (err != ESP_OK || ip.ip.addr == 0) {
        // The new configuration (with the password about to be shown) is
        // in place: replace it before leaving AP+STA.
        impl_->park_ap();
        return R::err(ErrorCode::DeviceFailure, "the access point has no address");
    }
    // Without it the portal does not pop up by itself, but the setup page
    // is still reachable at the address the screen shows.
    if (auto dns = impl_->dns.start(lwip_ntohl(ip.ip.addr)); !dns) {
        WLOG_W(kTag, "captive DNS unavailable: %s", dns.error().message.c_str());
    }
    return R::ok(ip_string(ip.ip.addr));
}

Result<void> EspWifi::stop_ap() {
    impl_->dns.stop();
    impl_->park_ap();
    const esp_err_t err = impl_->revert_to_station();
    if (err != ESP_OK) return fail(code_for(err), "stopping the access point", err);
    return Result<void>::ok();
}

int EspWifi::ap_clients() {
    wifi_sta_list_t list = {};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) return 0;
    return list.num;
}

std::array<uint8_t, 6> EspWifi::mac() {
    std::array<uint8_t, 6> out{};
    esp_wifi_get_mac(WIFI_IF_STA, out.data());
    return out;
}

Result<void> EspWifi::set_power_save(bool on) {
    const esp_err_t err = esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (err != ESP_OK) return fail(code_for(err), "esp_wifi_set_ps", err);
    return Result<void>::ok();
}

Result<void> EspWifi::set_radio(bool on) {
    if (on == impl_->radio_on) return Result<void>::ok();
    if (!on) {
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->link = hal::WifiLink{};
            impl_->attempt_ssid.clear();
        }
        impl_->dns.stop();
        const esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            return fail(code_for(err), "esp_wifi_stop", err);
        }
        impl_->radio_on = false;
        WLOG_I(kTag, "radio off");
        return Result<void>::ok();
    }
    const esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) return fail(code_for(err), "esp_wifi_start", err);
    impl_->radio_on = true;
    WLOG_I(kTag, "radio on");
    return Result<void>::ok();
}

}  // namespace wisp::port::esp
