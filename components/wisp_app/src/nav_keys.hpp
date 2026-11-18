#pragma once

// The nav-role buttons (wisp/hal/roles.hpp: nav_prev, nav_next, nav_ok,
// nav_back) as keys of the UI's keypad (device-model spec 9.1): their Down
// and Up edges become UiRuntime::push_key() presses and releases. Short
// and Long are ButtonService's gestures over the same edges and are not
// keys. Any nav role makes the keypad (UiConfig::keypad); nav_next and
// nav_ok are what moves and activates the focus, nav_prev and nav_back are
// optional.

#include <cstdint>

#include "wisp/base/fixed_string.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

// Whether any button holds a nav role.
bool has_nav_keys(const core::DeviceRegistry& registry);

// Resolves the nav roles to button ids once, at construction, and listens
// to ButtonService for as long as it exists. The listener only calls
// push_key(), a non-blocking push that fails when the keypad cannot take
// the entry (ButtonService's listener rules): a refused press, in standby
// for one, is simply lost, and so is its release.
class NavKeys {
public:
    NavKeys(core::DeviceRegistry& registry, core::ButtonService& buttons, ui::UiRuntime& ui);
    // Removes the listener; when it returns, no push is in progress.
    ~NavKeys();

    NavKeys(const NavKeys&) = delete;
    NavKeys& operator=(const NavKeys&) = delete;

private:
    void on_edge(const core::ButtonEdge& edge);

    core::ButtonService& buttons_;
    ui::UiRuntime& ui_;
    // Indexed by ui::NavKey; empty: no button holds that role.
    FixedString<32> ids_[4];
    uint32_t listener_ = 0;
};

}  // namespace wisp::app
