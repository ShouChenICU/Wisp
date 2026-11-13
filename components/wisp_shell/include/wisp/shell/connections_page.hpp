#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"
#include "wisp/core/model_profiles.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::ui {
class UiRuntime;
class SystemLayer;
}  // namespace wisp::ui
namespace wisp::shell {

enum class ConnectionKind { Wifi, Llm, Asr, Tts };
struct ConnectionsDeps {
    ui::UiRuntime& ui;
    ui::SystemLayer& layer;
    net::WifiService* wifi = nullptr;
    core::ModelProfileService* models = nullptr;
    // A bounded network worker; never the UI or storage worker.
    std::function<bool(std::function<void()>)> submit;
    std::function<void()> start_wifi_setup;
    std::function<std::string()> admin_url;
    std::function<void()> cancel_navigation;
};

// Native settings sub-pages. UI thread only; jobs capture service pointers
// and owned input, and post results behind a page-lifetime guard.
class ConnectionsPage {
public:
    ConnectionsPage(ConnectionsDeps deps, ConnectionKind kind);
    ~ConnectionsPage();
    static ui::StrId title(ConnectionKind kind);
    void build(lv_obj_t* list);
    void refresh();
    bool back();
    void scan();
    void select_network(const std::string& ssid, bool secured, bool saved, bool failed);
    void test_model(const std::string& id);
    void activate_model(const std::string& id);
    lv_obj_t* wifi_switch() const { return switch_; }
    lv_obj_t* password() const { return password_; }
    lv_obj_t* list() const { return list_; }
    lv_obj_t* view() const { return popup_ ? popup_ : editor_ ? editor_ : list_; }
    lv_obj_t* detail_root() const { return detail_root_; }
    lv_obj_t* result_label() const { return result_; }
    bool busy() const { return busy_; }

private:
    lv_obj_t* button(lv_obj_t* parent, ui::StrId label, std::function<void()> action);
    void bind(lv_obj_t* obj, std::function<void()> action);
    lv_obj_t* compact_row(lv_obj_t* parent, const std::string& name, const std::string& subtitle,
                          const std::string& value, std::function<void()> action);
    lv_obj_t* actions(lv_obj_t* parent);
    lv_obj_t* icon_button(lv_obj_t* parent, const char* icon, std::function<void()> action);
    lv_obj_t* network_row(lv_obj_t* parent, const std::string& ssid, const std::string& state,
                          bool secured, bool saved, bool failed, bool connected, int rssi);
    void update_password_action();
    void animate_scan(bool scanning);
    void open_details(const std::string& ssid, bool secured, bool saved);
    void render_models();
    void render_networks();
    void edit_password(const std::string& ssid);
    void close_password();
    void close_details();
    void show_password_keyboard();
    void connect_password();
    void run(std::function<ui::StrId()> work, std::shared_ptr<std::string> detail = {});
    void report(ui::StrId text);
    core::ModelKind model_kind() const;
    static void button_cb(lv_event_t* e);
    static void switch_cb(lv_event_t* e);
    struct Action {
        ConnectionsPage* page;
        std::function<void()> call;
    };
    ConnectionsDeps deps_;
    ConnectionKind kind_;
    lv_obj_t *list_ = nullptr, *content_ = nullptr, *result_ = nullptr, *status_ = nullptr;
    lv_obj_t *switch_ = nullptr, *editor_ = nullptr, *password_ = nullptr;
    lv_obj_t *summary_ = nullptr, *nearby_ = nullptr, *setup_ = nullptr;
    lv_obj_t *editor_result_ = nullptr, *scan_result_ = nullptr, *scan_button_ = nullptr;
    lv_obj_t* join_ = nullptr;
    lv_obj_t* detail_root_ = nullptr;
    lv_obj_t* popup_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    std::string ssid_, model_signature_, wifi_signature_, expanded_, feedback_id_, feedback_,
        switch_feedback_;
    std::vector<net::ScanResult> networks_;
    bool busy_ = false;
    bool shown_enabled_ = true, switch_pending_ = false, manage_open_ = false;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};
}  // namespace wisp::shell
