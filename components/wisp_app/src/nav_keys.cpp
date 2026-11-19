#include "nav_keys.hpp"

#include <memory>
#include <string_view>

#include "wisp/core/log.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/hal/roles.hpp"
#include "wisp/ui/key_nav.hpp"

namespace wisp::app {
namespace {

constexpr const char* kTag = "app";

// Indexed by ui::NavKey.
constexpr const char* kRoles[] = {hal::role::kNavPrev, hal::role::kNavNext, hal::role::kNavOk,
                                  hal::role::kNavBack};
static_assert(static_cast<int>(ui::NavKey::Prev) == 0 && static_cast<int>(ui::NavKey::Next) == 1 &&
              static_cast<int>(ui::NavKey::Ok) == 2 && static_cast<int>(ui::NavKey::Back) == 3);

}  // namespace

bool has_nav_keys(const core::DeviceRegistry& registry) {
    for (const char* role : kRoles) {
        if (registry.by_role<hal::Button>(role)) return true;
    }
    return false;
}

NavKeys::NavKeys(core::DeviceRegistry& registry, core::ButtonService& buttons, ui::UiRuntime& ui)
    : buttons_(buttons), ui_(ui) {
    for (int i = 0; i < 4; ++i) {
        auto button = registry.by_role<hal::Button>(kRoles[i]);
        if (!button) continue;
        const hal::DeviceInfo& info = button->info();
        ids_[i].assign(info.id);
        // The power and talk keys act on their own edges too, so one press
        // would also move or activate the focus.
        for (const char* other : {hal::role::kPower, hal::role::kTalk}) {
            if (hal::has_role(info, other)) {
                WLOG_W(kTag, "%s holds a nav role and %s", info.id.c_str(), other);
            }
        }
    }
    const auto& next = ids_[static_cast<int>(ui::NavKey::Next)];
    const auto& ok = ids_[static_cast<int>(ui::NavKey::Ok)];
    if (next.empty() || ok.empty()) {
        WLOG_W(kTag, "nav keys without nav_next and nav_ok: focus cannot move");
    }
    listener_ = buttons_.add_listener([this](const core::ButtonEdge& e) { on_edge(e); });
}

NavKeys::~NavKeys() { buttons_.remove_listener(listener_); }

void NavKeys::on_edge(const core::ButtonEdge& edge) {
    if (edge.kind != core::ButtonEdge::Kind::Down && edge.kind != core::ButtonEdge::Kind::Up) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        if (!ids_[i].empty() && ids_[i].view() == edge.id.view()) {
            (void)ui_.push_key(static_cast<ui::NavKey>(i),
                               edge.kind == core::ButtonEdge::Kind::Down);
            return;
        }
    }
}

}  // namespace wisp::app
