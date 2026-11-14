#include "wisp/shell/connections_page.hpp"

#include <algorithm>
#include <utility>

#include "page_frame.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/symbols.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

namespace wisp::shell {
using ui::StrId;

ConnectionsPage::ConnectionsPage(ConnectionsDeps deps, ConnectionKind kind)
    : deps_(std::move(deps)), kind_(kind) {}
ConnectionsPage::~ConnectionsPage() {
    alive_->store(false);
    if (timer_) lv_timer_delete(timer_);
    close_password();
    close_details();
}
StrId ConnectionsPage::title(ConnectionKind kind) {
    switch (kind) {
        case ConnectionKind::Wifi:
            return StrId::SettingsWifi;
        case ConnectionKind::Llm:
            return StrId::ModelLlm;
        case ConnectionKind::Asr:
            return StrId::ModelAsr;
        case ConnectionKind::Tts:
            return StrId::ModelTts;
    }
    return StrId::SettingsWifi;
}
core::ModelKind ConnectionsPage::model_kind() const {
    if (kind_ == ConnectionKind::Asr) return core::ModelKind::Asr;
    if (kind_ == ConnectionKind::Tts) return core::ModelKind::Tts;
    return core::ModelKind::Llm;
}
void ConnectionsPage::bind(lv_obj_t* obj, std::function<void()> action) {
    auto* handler = new Action{this, std::move(action)};
    lv_obj_add_event_cb(obj, button_cb, LV_EVENT_CLICKED, handler);
    lv_obj_add_event_cb(
        obj, [](lv_event_t* e) { delete static_cast<Action*>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, handler);
}
lv_obj_t* ConnectionsPage::actions(lv_obj_t* parent) {
    auto* box = detail::make_box(parent);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(box, ui::theme::kGapSm, 0);
    return box;
}
lv_obj_t* ConnectionsPage::button(lv_obj_t* parent, StrId label, std::function<void()> action) {
    auto* btn = lv_button_create(parent);
    lv_obj_set_width(btn, LV_PCT(100));
    lv_obj_set_height(btn, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(btn, detail::kMinTouchPx, 0);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (lv_obj_get_style_flex_flow(parent, LV_PART_MAIN) == LV_FLEX_FLOW_ROW) {
        lv_obj_set_width(btn, 0);
        lv_obj_set_flex_grow(btn, 1);
    }
    auto* text = detail::make_text(btn, ui::tr(label));
    lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_CENTER, 0);
    ui::set_button_role(btn, label == StrId::ModelActivate || label == StrId::WifiConnect
                                 ? ui::ButtonRole::Primary
                                 : ui::ButtonRole::Secondary);
    bind(btn, std::move(action));
    detail::bubble_tree(btn);
    return btn;
}
lv_obj_t* ConnectionsPage::icon_button(lv_obj_t* parent, const char* icon,
                                       std::function<void()> action) {
    auto* btn = lv_button_create(parent);
    lv_obj_set_size(btn, detail::kMinTouchPx, detail::kMinTouchPx);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btn, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, LV_STATE_DISABLED);
    lv_obj_set_style_text_color(btn, detail::accent_colour(parent), 0);
    lv_obj_set_style_text_opa(btn, LV_OPA_30, LV_STATE_DISABLED);
    auto* label = lv_label_create(btn);
    lv_label_set_text(label, icon);
    lv_obj_center(label);
    bind(btn, std::move(action));
    detail::bubble_tree(btn);
    return btn;
}
lv_obj_t* ConnectionsPage::network_row(lv_obj_t* parent, const std::string& ssid,
                                       const std::string& state, bool secured, bool saved,
                                       bool failed, bool connected, int rssi) {
    auto parts = detail::make_list_row(parent, nullptr, "", true);
    auto* row = parts.row;
    lv_obj_delete(parts.name);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_set_style_pad_right(row, 0, 0);
    lv_obj_set_height(row, 56);
    lv_obj_set_style_pad_column(row, 6, 0);
    if (connected) {
        auto* mark = lv_label_create(row);
        lv_label_set_text(mark, LV_SYMBOL_OK);
        lv_obj_set_style_text_color(mark, detail::accent_colour(row), 0);
    }
    auto* text = detail::make_box(row, 0);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    auto* name = detail::make_text(text, ssid.c_str());
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_height(name, detail::line_height(name));
    if (!state.empty()) {
        auto* label = detail::make_text(text, state.c_str(), true);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_height(label, detail::line_height(label));
    }
    if (secured) {
        auto* lock = lv_label_create(row);
        lv_label_set_text(lock, WISP_SYMBOL_LOCK);
        lv_obj_set_style_text_color(lock, detail::dim_colour(row), 0);
    }
    auto* signal = lv_label_create(row);
    lv_label_set_text(signal, rssi ? LV_SYMBOL_WIFI : LV_SYMBOL_MINUS);
    lv_obj_set_style_text_opa(signal, rssi >= -65 ? LV_OPA_COVER : LV_OPA_50, 0);
    icon_button(row, WISP_SYMBOL_INFO,
                [this, ssid, secured, saved] { open_details(ssid, secured, saved); });
    bind(row, [this, ssid, secured, saved, failed, connected] {
        if (connected)
            open_details(ssid, secured, saved);
        else
            select_network(ssid, secured, saved, failed);
    });
    if (busy_) lv_obj_add_state(row, LV_STATE_DISABLED);
    detail::bubble_tree(row);
    return row;
}
lv_obj_t* ConnectionsPage::compact_row(lv_obj_t* parent, const std::string& name,
                                       const std::string& subtitle, const std::string& value,
                                       std::function<void()> action) {
    auto row = detail::make_list_row(parent, nullptr, name.c_str(), true);
    lv_obj_delete(row.name);
    auto* text = detail::make_box(row.row, 0);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    auto* primary = detail::make_text(text, name.c_str());
    lv_obj_set_height(primary, detail::line_height(primary));
    lv_label_set_long_mode(primary, LV_LABEL_LONG_DOT);
    if (!subtitle.empty()) {
        auto* secondary = detail::make_text(text, subtitle.c_str(), true);
        lv_obj_set_height(secondary, detail::line_height(secondary));
        lv_label_set_long_mode(secondary, LV_LABEL_LONG_DOT);
    }
    detail::fit_row(row.row, detail::line_height(row.row) * (subtitle.empty() ? 1 : 2));
    if (!value.empty()) detail::add_row_value(row.row, value.c_str(), false);
    bind(row.row, std::move(action));
    detail::bubble_tree(row.row);
    return row.row;
}
void ConnectionsPage::button_cb(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto* a = static_cast<Action*>(lv_event_get_user_data(e));
    auto* self = a->page;
    self->deps_.ui.post([call = a->call, alive = self->alive_] {
        if (alive->load()) call();
    });
}
void ConnectionsPage::switch_cb(lv_event_t* e) {
    auto* self = static_cast<ConnectionsPage*>(lv_event_get_user_data(e));
    bool enabled = lv_obj_has_state(self->switch_, LV_STATE_CHECKED);
    self->deps_.ui.post([self, enabled, alive = self->alive_] {
        if (!alive->load()) return;
        self->back();
        self->switch_feedback_.clear();
        self->switch_pending_ = true;
        lv_obj_add_state(self->switch_, LV_STATE_DISABLED);
        self->refresh();
        auto* ui = &self->deps_.ui;
        auto* wifi = self->deps_.wifi;
        if (!self->deps_.submit || !self->deps_.submit([self, ui, alive, wifi, enabled] {
                const bool ok = static_cast<bool>(wifi->set_enabled(enabled, os::now_ms()));
                ui->post([self, alive, ok] {
                    if (!alive->load()) return;
                    self->switch_pending_ = false;
                    lv_obj_remove_state(self->switch_, LV_STATE_DISABLED);
                    self->switch_feedback_ = ok ? "" : ui::tr(StrId::ConnectionFailed);
                    self->render_networks();
                    self->refresh();
                    if (ok && self->deps_.wifi->enabled()) self->scan();
                });
            })) {
            self->switch_pending_ = false;
            lv_obj_remove_state(self->switch_, LV_STATE_DISABLED);
            self->switch_feedback_ = ui::tr(StrId::ConnectionUnavailable);
            self->refresh();
        }
    });
}
void ConnectionsPage::build(lv_obj_t* list) {
    list_ = list;
    // Match grouped-row insets on rectangular and shaped displays alike.
    const auto safe = hal::safe_rect(deps_.layer.shape());
    lv_obj_set_style_pad_left(list, std::max<int32_t>(ui::theme::kGap, safe.x1), 0);
    lv_obj_set_style_pad_right(
        list, std::max<int32_t>(ui::theme::kGap, deps_.layer.shape().width - 1 - safe.x2), 0);
    if (kind_ == ConnectionKind::Wifi) {
        if (!deps_.wifi) {
            result_ = detail::make_text(list, ui::tr(StrId::ConnectionUnavailable), true);
            return;
        }
        summary_ = detail::make_row_card(list);
        auto row = detail::make_list_row(summary_, nullptr, "Wi-Fi", false);
        lv_obj_set_flex_grow(row.name, 1);
        switch_ = lv_switch_create(row.row);
        lv_obj_add_flag(switch_, detail::kNoSwipe);
        lv_obj_add_event_cb(switch_, switch_cb, LV_EVENT_VALUE_CHANGED, this);
        status_ = detail::make_text(detail::make_row_box(summary_), "", true);
        nearby_ = detail::make_box(list);
        lv_obj_set_flex_flow(nearby_, LV_FLEX_FLOW_COLUMN);
        auto* heading = actions(nearby_);
        lv_obj_set_flex_align(heading, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        auto* label = detail::make_text(heading, ui::tr(StrId::WifiNearby), true);
        lv_obj_set_width(label, 0);
        lv_obj_set_flex_grow(label, 1);
        scan_button_ = icon_button(heading, LV_SYMBOL_REFRESH, [this] { scan(); });
        lv_obj_set_style_text_opa(scan_button_, LV_OPA_COVER, LV_STATE_DISABLED);
        result_ = scan_result_ = detail::make_text(nearby_, "", true);
        lv_obj_add_flag(result_, LV_OBJ_FLAG_HIDDEN);
        content_ = detail::make_box(nearby_);
        lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(content_, ui::theme::kGapSm, 0);
        if (deps_.start_wifi_setup) {
            auto* card = detail::make_row_card(nearby_);
            lv_obj_set_style_margin_top(card, ui::theme::kGapLg, 0);
            auto row = detail::make_list_row(card, LV_SYMBOL_SETTINGS,
                                             ui::tr(StrId::WifiSetupPhone), true);
            setup_ = row.row;
            detail::add_row_value(row.row, "", true);
            bind(row.row, [this] {
                if (!busy_ && deps_.wifi->enabled())
                    run([setup = deps_.start_wifi_setup] {
                        setup();
                        return StrId::WifiSetupPhone;
                    });
            });
        }
        render_networks();
        scan();
    } else {
        if (!deps_.models) {
            result_ = detail::make_text(list, ui::tr(StrId::ConnectionUnavailable), true);
            return;
        }
        content_ = detail::make_box(list);
        lv_obj_set_flex_flow(content_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(content_, 0, 0);
    }
    refresh();
    detail::bubble_tree(list);
    timer_ = lv_timer_create(
        [](lv_timer_t* t) { static_cast<ConnectionsPage*>(lv_timer_get_user_data(t))->refresh(); },
        1000, this);
}
void ConnectionsPage::report(StrId text) {
    feedback_ = ui::tr(text);
    if (result_) {
        lv_label_set_text(result_, feedback_.c_str());
        lv_obj_remove_flag(result_, LV_OBJ_FLAG_HIDDEN);
    }
}
void ConnectionsPage::run(std::function<StrId()> work, std::shared_ptr<std::string> detail) {
    if (busy_) return;
    busy_ = true;
    report(StrId::ConnectionBusy);
    if (kind_ != ConnectionKind::Wifi) {
        model_signature_.clear();
        render_models();
    } else {
        wifi_signature_.clear();
        render_networks();
    }
    auto alive = alive_;
    auto* ui = &deps_.ui;
    if (!deps_.submit || !deps_.submit([this, alive, ui, detail, work = std::move(work)] {
            if (!alive->load()) return;
            const StrId result = work();
            ui->post([this, alive, result, detail] {
                if (!alive->load()) return;
                busy_ = false;
                report(result);
                if (detail && !detail->empty()) {
                    feedback_ += " · " + *detail;
                    if (result_) lv_label_set_text(result_, feedback_.c_str());
                }
                if (result == StrId::ModelActivated) expanded_.clear();
                model_signature_.clear();
                refresh();
            });
        })) {
        busy_ = false;
        report(StrId::ConnectionUnavailable);
        refresh();
    }
}
void ConnectionsPage::refresh() {
    if (!content_) return;
    if (kind_ != ConnectionKind::Wifi) {
        if (!busy_) render_models();
        return;
    }
    const bool enabled =
        switch_pending_ ? lv_obj_has_state(switch_, LV_STATE_CHECKED) : deps_.wifi->enabled();
    if (enabled)
        lv_obj_add_state(switch_, LV_STATE_CHECKED);
    else
        lv_obj_remove_state(switch_, LV_STATE_CHECKED);
    if (!enabled && shown_enabled_) {
        close_password();
        close_details();
        networks_.clear();
        render_networks();
    }
    shown_enabled_ = enabled;
    const auto s = deps_.wifi->status();
    if (s.ssid == feedback_id_ &&
        (s.state == net::WifiState::Connected || s.reason == "auth_failed"))
        feedback_.clear();
    StrId state = !enabled                                ? StrId::WifiOff
                  : s.state == net::WifiState::Connected  ? StrId::WifiConnected
                  : s.state == net::WifiState::Connecting ? StrId::WifiConnecting
                  : s.reason == "auth_failed"             ? StrId::WifiAuthFailed
                                                          : StrId::WifiDisconnected;
    std::string text = ui::tr(state);
    if (enabled && !s.ssid.empty() && s.state != net::WifiState::Connected) text += "\n" + s.ssid;
    if (enabled && s.state == net::WifiState::Connected)
        text += " · " + std::string(ui::tr(s.rssi >= -65 ? StrId::WifiStrong : StrId::WifiWeak));
    if (enabled && s.reason == "not_found") text += "\n" + std::string(ui::tr(StrId::WifiNotFound));
    if (switch_pending_) text = ui::tr(StrId::ConnectionBusy);
    if (!switch_feedback_.empty()) text += "\n" + switch_feedback_;
    lv_label_set_text(status_, text.c_str());
    if (enabled && s.state == net::WifiState::Connected && !switch_pending_ &&
        switch_feedback_.empty())
        lv_obj_add_flag(lv_obj_get_parent(status_), LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(lv_obj_get_parent(status_), LV_OBJ_FLAG_HIDDEN);
    if (enabled)
        lv_obj_remove_flag(nearby_, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(nearby_, LV_OBJ_FLAG_HIDDEN);
    std::string signature = std::string(enabled ? "+" : "-") + s.ssid + "\n" +
                            net::to_string(s.state) + feedback_id_ + feedback_ + (busy_ ? "!" : "");
    for (auto& n : deps_.wifi->saved_networks())
        signature += "\n" + n.ssid + (n.auth_failed ? "!" : ".");
    if (signature != wifi_signature_) {
        wifi_signature_ = std::move(signature);
        render_networks();
    }
}
void ConnectionsPage::animate_scan(bool scanning) {
    auto* icon = lv_obj_get_child(scan_button_, 0);
    lv_anim_delete(icon, nullptr);
    lv_obj_set_style_transform_rotation(icon, 0, 0);
    if (!scanning) {
        lv_obj_remove_state(scan_button_, LV_STATE_DISABLED);
        return;
    }
    lv_obj_add_state(scan_button_, LV_STATE_DISABLED);
    lv_obj_update_layout(scan_button_);
    lv_obj_set_style_transform_pivot_x(icon, lv_obj_get_width(icon) / 2, 0);
    lv_obj_set_style_transform_pivot_y(icon, lv_obj_get_height(icon) / 2, 0);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, icon);
    lv_anim_set_values(&animation, 0, 3600);
    lv_anim_set_duration(&animation, 1000);
    lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&animation, [](void* obj, int32_t angle) {
        lv_obj_set_style_transform_rotation(static_cast<lv_obj_t*>(obj), angle, 0);
    });
    lv_anim_start(&animation);
}
void ConnectionsPage::scan() {
    if (busy_ || switch_pending_ || !deps_.wifi || !deps_.wifi->enabled()) return;
    auto results = std::make_shared<std::vector<net::ScanResult>>();
    auto ok = std::make_shared<bool>(false);
    // Owned results are read only after the worker has posted completion.
    auto alive = alive_;
    auto* ui = &deps_.ui;
    auto* wifi = deps_.wifi;
    busy_ = true;
    animate_scan(true);
    lv_obj_add_flag(scan_result_, LV_OBJ_FLAG_HIDDEN);
    if (!deps_.submit || !deps_.submit([this, alive, ui, wifi, results, ok] {
            if (!alive->load()) return;
            auto found = wifi->scan();
            if (found) {
                *results = std::move(found.value());
                *ok = true;
            }
            ui->post([this, alive, results, ok] {
                if (!alive->load()) return;
                busy_ = false;
                if (deps_.wifi->enabled() && *ok) networks_ = std::move(*results);
                if (!deps_.wifi->enabled()) networks_.clear();
                animate_scan(false);
                if (*ok)
                    lv_obj_add_flag(scan_result_, LV_OBJ_FLAG_HIDDEN);
                else {
                    lv_label_set_text(scan_result_, ui::tr(StrId::WifiScanFailed));
                    lv_obj_remove_flag(scan_result_, LV_OBJ_FLAG_HIDDEN);
                }
                render_networks();
                refresh();
            });
        })) {
        busy_ = false;
        animate_scan(false);
        lv_label_set_text(scan_result_, ui::tr(StrId::ConnectionUnavailable));
        lv_obj_remove_flag(scan_result_, LV_OBJ_FLAG_HIDDEN);
    }
}
void ConnectionsPage::render_networks() {
    if (!content_ || editor_) return;
    result_ = scan_result_;
    lv_obj_clean(content_);
    // The current-network shortcut belongs to the summary, outside content_.
    while (lv_obj_get_child_count(summary_) > 2) lv_obj_delete(lv_obj_get_child(summary_, 2));
    const bool enabled =
        switch_pending_ ? lv_obj_has_state(switch_, LV_STATE_CHECKED) : deps_.wifi->enabled();
    if (!enabled) return;
    auto rows = networks_;
    std::stable_sort(rows.begin(), rows.end(),
                     [](const auto& a, const auto& b) { return a.rssi > b.rssi; });
    const auto saved = deps_.wifi->saved_networks();
    const auto status = deps_.wifi->status();
    for (const auto& n : saved)
        if (std::none_of(rows.begin(), rows.end(), [&](const auto& r) { return r.ssid == n.ssid; }))
            rows.push_back({.ssid = n.ssid, .secured = n.secured, .saved = true});
    auto* summary_text = lv_obj_get_parent(status_);
    if (status.state == net::WifiState::Connected && !switch_pending_ && switch_feedback_.empty())
        lv_obj_add_flag(summary_text, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(summary_text, LV_OBJ_FLAG_HIDDEN);
    // Keep the connected network in the summary only.
    if (status.state == net::WifiState::Connected) {
        const auto saved_current = std::find_if(
            saved.begin(), saved.end(), [&](const auto& n) { return n.ssid == status.ssid; });
        const bool secured = saved_current != saved.end() && saved_current->secured;
        auto* row = network_row(content_, status.ssid, "", secured, saved_current != saved.end(),
                                false, true, status.rssi);
        lv_obj_set_parent(row, summary_);
    }
    auto* card = detail::make_row_card(content_);
    size_t count = 0;
    for (const auto& n : rows) {
        if (status.state == net::WifiState::Connected && status.ssid == n.ssid) continue;
        ++count;
        auto found = std::find_if(saved.begin(), saved.end(),
                                  [&](const auto& s) { return s.ssid == n.ssid; });
        const bool known = found != saved.end();
        const bool failed = known && found->auth_failed;
        std::string state;
        if (failed)
            state = ui::tr(StrId::WifiAuthFailed);
        else if (!n.rssi)
            state = ui::tr(StrId::WifiNotFound);
        if (status.ssid == n.ssid && status.state == net::WifiState::Connecting)
            state = ui::tr(StrId::WifiConnecting);
        if (feedback_id_ == n.ssid && !feedback_.empty()) state = feedback_;
        auto* row = network_row(card, n.ssid, state, n.secured, known, failed, false, n.rssi);
        if (feedback_id_ == n.ssid && !feedback_.empty()) {
            auto* text = lv_obj_get_child(row, 0);
            result_ = lv_obj_get_child(text, -1);
        }
    }
    if (!count) {
        lv_obj_set_style_pad_all(card, ui::theme::kGapMd, 0);
        auto* hint = detail::make_text(card, ui::tr(StrId::WifiEmpty), true);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    }
    detail::bubble_tree(content_);
}
void ConnectionsPage::open_details(const std::string& ssid, bool secured, bool saved) {
    if (busy_ || editor_ || popup_) return;
    auto frame = detail::make_page(deps_.layer.shape(), StrId::WifiDetails);
    detail_root_ = frame.root;
    lv_obj_set_parent(detail_root_, lv_screen_active());
    lv_obj_set_style_bg_color(detail_root_,
                              ui::wisp_background(ui::display_panel(lv_obj_get_display(list_))), 0);
    lv_obj_set_style_bg_opa(detail_root_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(detail_root_, detail::text_colour(detail_root_), 0);
    lv_obj_set_style_text_font(detail_root_, ui::default_font(), 0);
    editor_ = frame.list;
    lv_obj_delete(frame.screen);
    lv_obj_set_style_pad_row(editor_, ui::theme::kGap, 0);
    auto* info = detail::make_card(editor_);
    auto* name = detail::make_text(info, ssid.c_str());
    lv_obj_set_style_text_color(name, detail::accent_colour(name), 0);
    const auto s = deps_.wifi->status();
    const bool connected = s.ssid == ssid && s.state == net::WifiState::Connected;
    lv_obj_set_style_text_font(name, ui::font(24), 0);
    std::string summary = ui::tr(connected ? StrId::WifiConnected : StrId::WifiDisconnected);
    if (saved) summary += std::string(" · ") + ui::tr(StrId::WifiSaved);
    detail::make_text(info, summary.c_str(), true);
    auto details = connected ? s.details : hal::WifiDetails{};
    int rssi = s.rssi;
    bool in_range = connected;
    if (!connected) {
        for (const auto& n : networks_) {
            if (n.ssid != ssid) continue;
            details = n.details;
            rssi = n.rssi;
            in_range = true;
            break;
        }
    }
    auto field = [&](lv_obj_t* card, StrId label, const std::string& value) {
        if (value.empty()) return;
        auto* row = detail::make_box(card);
        const bool address = label == StrId::WifiBssid || label == StrId::WifiMac;
        lv_obj_set_flex_flow(row, address ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_row(row, ui::theme::kGapXs, 0);
        lv_obj_set_style_pad_column(row, ui::theme::kGapSm, 0);
        auto* key = detail::make_text(row, ui::tr(label), true);
        auto* text = detail::make_text(row, value.c_str());
        if (!address) {
            lv_obj_set_width(key, LV_PCT(40));
            lv_obj_set_width(text, 0);
            lv_obj_set_flex_grow(text, 1);
            lv_obj_set_style_text_align(text, LV_TEXT_ALIGN_RIGHT, 0);
        }
    };
    auto* radio = detail::make_card(editor_);
    lv_obj_set_style_pad_row(radio, ui::theme::kGap, 0);
    field(radio, StrId::WifiSecurity,
          details.security.empty() || details.security == "Open"
              ? ui::tr(secured ? StrId::WifiSecured : StrId::WifiOpen)
              : details.security);
    if (in_range)
        field(radio, StrId::WifiSignal,
              std::to_string(rssi) + " dBm · " +
                  ui::tr(rssi >= -65 ? StrId::WifiStrong : StrId::WifiWeak));
    if (details.channel) field(radio, StrId::WifiChannel, std::to_string(details.channel));
    field(radio, StrId::WifiBssid, details.bssid);
    if (connected) {
        auto* addresses = detail::make_card(editor_);
        lv_obj_set_style_pad_row(addresses, ui::theme::kGap, 0);
        field(addresses, StrId::WifiIpAddress, s.ip);
        field(addresses, StrId::WifiSubnet, details.netmask);
        field(addresses, StrId::WifiGateway, details.gateway);
        field(addresses, StrId::WifiDns, details.dns);
        field(addresses, StrId::WifiMac, details.mac);
    }
    auto* controls = detail::make_box(editor_);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(controls, ui::theme::kGapSm, 0);
    if (connected) {
        button(controls, StrId::WifiDisconnect, [this] {
            back();
            run([wifi = deps_.wifi] {
                return wifi->disconnect() ? StrId::WifiDisconnected : StrId::ConnectionFailed;
            });
        });
    } else {
        button(controls, StrId::WifiConnect, [this, ssid, secured, saved] {
            if (secured && !saved)
                edit_password(ssid);
            else {
                back();
                select_network(ssid, secured, saved, false);
            }
        });
    }
    if (secured) button(controls, StrId::WifiChangePassword, [this, ssid] { edit_password(ssid); });
    // Non-scrollable cards must pass a drag on their text to the page list.
    auto chain = [&](auto&& self, lv_obj_t* obj) -> void {
        lv_obj_add_flag(obj, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_SCROLL_CHAIN_VER |
                                                        LV_OBJ_FLAG_EVENT_BUBBLE));
        for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
            self(self, lv_obj_get_child(obj, static_cast<int32_t>(i)));
    };
    chain(chain, detail_root_);
}
bool ConnectionsPage::back() {
    if (popup_) {
        close_password();
        return true;
    }
    if (!detail_root_) return false;
    close_details();
    refresh();
    return true;
}
void ConnectionsPage::close_details() {
    if (!detail_root_) return;
    if (deps_.cancel_navigation) deps_.cancel_navigation();
    lv_obj_delete(detail_root_);
    detail_root_ = editor_ = nullptr;
    wifi_signature_.clear();
}
void ConnectionsPage::select_network(const std::string& ssid, bool secured, bool saved,
                                     bool failed) {
    if (busy_ || switch_pending_ || !deps_.wifi || !deps_.wifi->enabled()) return;
    close_password();
    feedback_id_ = ssid;
    if (secured && (!saved || failed)) {
        edit_password(ssid);
        return;
    }
    run([wifi = deps_.wifi, ssid, saved] {
        auto r = saved ? wifi->connect_saved(ssid, os::now_ms())
                       : wifi->add_network(ssid, "", os::now_ms());
        return r ? StrId::WifiScheduled : StrId::ConnectionFailed;
    });
}
void ConnectionsPage::edit_password(const std::string& ssid) {
    close_password();
    ssid_ = ssid;
    popup_ = deps_.layer.open_dialog();
    if (!popup_) {
        ssid_.clear();
        return;
    }
    const auto safe = hal::safe_rect(deps_.layer.shape());
    lv_obj_set_style_pad_all(popup_, 0, 0);
    lv_obj_set_style_border_width(popup_, 0, 0);
    lv_obj_set_style_radius(popup_, ui::theme::kRadius, 0);
    lv_obj_align(popup_, LV_ALIGN_TOP_MID, 0, safe.y1);
    auto* form = lv_msgbox_get_content(popup_);
    lv_obj_set_style_pad_all(form, ui::theme::kGapSm, 0);
    lv_obj_set_style_pad_row(form, ui::theme::kGapSm, 0);
    lv_obj_set_flex_flow(form, LV_FLEX_FLOW_COLUMN);
    auto* nav = actions(form);
    lv_obj_set_height(nav, detail::kMinTouchPx);
    lv_obj_set_style_pad_column(nav, ui::theme::kGapSm, 0);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    icon_button(nav, LV_SYMBOL_CLOSE, [this] { close_password(); });
    auto* title = detail::make_box(nav, 0);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_flex_flow(title, LV_FLEX_FLOW_COLUMN);
    auto* name = detail::make_text(title, ssid.c_str());
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_height(name, detail::line_height(name));
    editor_result_ = detail::make_text(title, ui::tr(StrId::WifiPassword), true);
    lv_obj_set_style_text_align(editor_result_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(editor_result_, detail::line_height(editor_result_));
    lv_label_set_long_mode(editor_result_, LV_LABEL_LONG_DOT);
    join_ = icon_button(nav, LV_SYMBOL_OK, [this] { connect_password(); });
    auto* box = actions(form);
    lv_obj_set_height(box, detail::kMinTouchPx);
    lv_obj_set_style_pad_column(box, ui::theme::kGapSm, 0);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(box, ui::theme::kRadius, 0);
    if (detail::colour_panel(box)) {
        lv_obj_set_style_bg_color(box, lv_color_hex(ui::theme::kSurface2), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    }
    password_ = lv_textarea_create(box);
    lv_obj_set_width(password_, 0);
    lv_obj_set_height(password_, detail::kMinTouchPx);
    lv_obj_set_flex_grow(password_, 1);
    lv_obj_set_style_border_width(password_, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(password_, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_bg_opa(password_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_ver(password_, (detail::kMinTouchPx - detail::line_height(password_)) / 2,
                             0);
    lv_textarea_set_one_line(password_, true);
    lv_textarea_set_max_length(password_, 63);
    lv_textarea_set_password_mode(password_, true);
    lv_textarea_set_password_show_time(password_, 0);
    lv_textarea_set_placeholder_text(password_, ui::tr(StrId::WifiPassword));
    lv_obj_add_flag(password_, detail::kNoSwipe);
    lv_obj_add_event_cb(
        password_,
        [](lv_event_t* e) {
            static_cast<ConnectionsPage*>(lv_event_get_user_data(e))->show_password_keyboard();
        },
        LV_EVENT_FOCUSED, this);
    lv_obj_add_event_cb(
        password_,
        [](lv_event_t* e) {
            static_cast<ConnectionsPage*>(lv_event_get_user_data(e))->update_password_action();
        },
        LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(
        password_,
        [](lv_event_t* e) {
            auto* self = static_cast<ConnectionsPage*>(lv_event_get_user_data(e));
            self->deps_.ui.post([self, alive = self->alive_] {
                if (alive->load()) self->connect_password();
            });
        },
        LV_EVENT_READY, this);
    icon_button(box, LV_SYMBOL_EYE_OPEN, [this] {
        if (!password_) return;
        const bool hidden = !lv_textarea_get_password_mode(password_);
        lv_textarea_set_password_mode(password_, hidden);
        auto* control = lv_obj_get_child(lv_obj_get_parent(password_), 1);
        lv_label_set_text(lv_obj_get_child(control, 0),
                          hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    });
    lv_obj_add_event_cb(
        password_,
        [](lv_event_t* e) {
            auto* self = static_cast<ConnectionsPage*>(lv_event_get_user_data(e));
            self->deps_.ui.post([self, alive = self->alive_] {
                if (alive->load()) self->close_password();
            });
        },
        LV_EVENT_CANCEL, this);
    detail::bubble_tree(popup_);
    update_password_action();
    show_password_keyboard();
}
void ConnectionsPage::update_password_action() {
    const std::string text = lv_textarea_get_text(password_);
    const bool valid = text.size() >= 8 && text.size() <= 63;
    if (valid)
        lv_obj_remove_state(join_, LV_STATE_DISABLED);
    else
        lv_obj_add_state(join_, LV_STATE_DISABLED);
    lv_label_set_text(editor_result_, ui::tr(StrId::WifiPassword));
}
void ConnectionsPage::show_password_keyboard() {
    if (!password_) return;
    lv_obj_update_layout(popup_);
    deps_.layer.show_keyboard(password_);
    deps_.layer.raise();
}
void ConnectionsPage::close_password() {
    if (!popup_) return;
    deps_.layer.hide_keyboard();
    if (password_) lv_textarea_set_text(password_, "");
    deps_.layer.close_confirm(popup_);
    popup_ = password_ = editor_result_ = join_ = nullptr;
    ssid_.clear();
    wifi_signature_.clear();
}
void ConnectionsPage::connect_password() {
    if (busy_ || !password_ || !deps_.wifi->enabled()) return;
    std::string password = lv_textarea_get_text(password_);
    if (password.size() < 8 || password.size() > 63) {
        lv_label_set_text(editor_result_, ui::tr(StrId::WifiPasswordLength));
        show_password_keyboard();
        return;
    }
    auto ssid = ssid_;
    feedback_id_ = ssid;
    close_password();
    if (detail_root_) back();
    run([wifi = deps_.wifi, ssid = std::move(ssid), password = std::move(password)] {
        return wifi->add_network(ssid, password, os::now_ms()) ? StrId::WifiScheduled
                                                               : StrId::ConnectionFailed;
    });
}
void ConnectionsPage::render_models() {
    const auto state = deps_.models->get();
    const auto kind = model_kind();
    std::string signature = state.active_for(kind) + expanded_ + feedback_ + (busy_ ? "!" : "");
    for (const auto& p : state.profiles)
        if (p.kind == kind) signature += "\n" + p.id + "\n" + p.name + "\n" + p.model;
    if (signature == model_signature_ && lv_obj_get_child_count(content_) > 0) return;
    model_signature_ = signature;
    auto scroll = lv_obj_get_scroll_y(list_);
    result_ = nullptr;
    lv_obj_clean(content_);
    auto* group = detail::make_row_card(content_);
    size_t count = 0;
    for (const auto& p : state.profiles) {
        if (p.kind != kind) continue;
        ++count;
        const bool current = state.active_for(kind) == p.id;
        const bool expanded = expanded_ == p.id;
        auto* row = compact_row(group, p.name, p.model, expanded ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
                                [this, id = p.id] {
                                    if (busy_) return;
                                    expanded_ = expanded_ == id ? "" : id;
                                    render_models();
                                });
        // A definite height centres accessories in the full flex row.
        lv_obj_set_height(row, 64);
        auto* mark = lv_label_create(row);
        lv_label_set_text(mark, current ? LV_SYMBOL_OK : "");
        lv_obj_set_width(mark, 20);
        lv_obj_set_style_text_color(mark, detail::accent_colour(row), 0);
        lv_obj_move_to_index(mark, 0);
        if (busy_) lv_obj_add_state(row, LV_STATE_DISABLED);
        if (expanded) {
            auto* body = detail::make_box(group);
            lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
            if (detail::colour_panel(body)) {
                lv_obj_set_style_bg_color(body, lv_color_hex(ui::theme::kSurface2), 0);
                lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
            }
            if (p.name.size() > 24 || p.model.size() > 30)
                detail::make_text(detail::make_row_box(body), (p.name + "\n" + p.model).c_str(),
                                  true);
            auto action = [&](const char* icon, StrId label, std::function<void()> call) {
                auto control = detail::make_list_row(body, icon, ui::tr(label), true);
                lv_obj_set_width(control.name, 0);
                lv_obj_set_flex_grow(control.name, 1);
                lv_obj_set_style_text_color(control.row, detail::accent_colour(control.row), 0);
                bind(control.row, std::move(call));
                if (busy_) lv_obj_add_state(control.row, LV_STATE_DISABLED);
            };
            action(LV_SYMBOL_REFRESH, StrId::ModelTest, [this, id = p.id] { test_model(id); });
            if (!current)
                action(LV_SYMBOL_OK, StrId::ModelActivate,
                       [this, id = p.id] { activate_model(id); });
            if (feedback_id_ == p.id && !feedback_.empty())
                result_ = detail::make_text(detail::make_row_box(body), feedback_.c_str(), true);
        }
    }
    if (!count) detail::make_text(detail::make_row_box(group), ui::tr(StrId::ModelNone), true);
    auto* management = detail::make_row_card(content_);
    lv_obj_set_style_margin_top(management, ui::theme::kGapLg, 0);
    auto manage =
        detail::make_list_row(management, LV_SYMBOL_SETTINGS, ui::tr(StrId::ModelManage), true);
    lv_obj_set_width(manage.name, 0);
    lv_obj_set_flex_grow(manage.name, 1);
    detail::add_row_value(manage.row, "", true);
    bind(manage.row, [this] {
        manage_open_ = !manage_open_;
        model_signature_.clear();
        render_models();
    });
    if (manage_open_ || !count) {
        auto* box = detail::make_row_box(management);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
        detail::make_text(box, ui::tr(StrId::ModelManageHint), true);
        if (deps_.admin_url) detail::make_text(box, deps_.admin_url().c_str(), true);
    }
    detail::bubble_tree(content_);
    lv_obj_update_layout(list_);
    lv_obj_scroll_to_y(list_, scroll, LV_ANIM_OFF);
}
void ConnectionsPage::test_model(const std::string& id) {
    if (!deps_.models || busy_) return;
    expanded_ = feedback_id_ = id;
    auto detail = std::make_shared<std::string>();
    run(
        [models = deps_.models, id, detail] {
            auto r = models->test(id);
            if (!r) return StrId::ModelTestFailed;
            const auto& p = r.value();
            if (p.ok) {
                *detail = std::to_string(p.latency_ms) + " ms";
                return StrId::ModelTestOk;
            }
            if (p.http_status == 401 || p.http_status == 403) return StrId::ModelTestAuth;
            if (p.error == "timeout") return StrId::ModelTestTimeout;
            if (p.error == "tls") return StrId::ModelTestTls;
            if (p.error == "dns" || p.error == "connect") return StrId::ModelTestNetwork;
            if (p.error == "no_model_list") return StrId::ModelTestNoList;
            if (p.error == "http") return StrId::ModelTestHttp;
            return StrId::ModelTestFailed;
        },
        detail);
}
void ConnectionsPage::activate_model(const std::string& id) {
    if (!deps_.models || busy_) return;
    expanded_ = feedback_id_ = id;
    run([models = deps_.models, id] {
        return models->activate(id) ? StrId::ModelActivated : StrId::ConnectionFailed;
    });
}
}  // namespace wisp::shell
