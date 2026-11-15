#include "wisp/hal/roles.hpp"

namespace wisp::hal {

namespace {

// Calls `fn` with every role string in caps "role", stopping early when it
// returns true; returns whether it did. An empty string is skipped here, so
// a board writing role "" (as a table row with no role does) declares no
// role for every caller alike, instead of a role named "" that two devices
// could collide on.
template <class Fn>
bool any_role(const DeviceInfo& info, Fn fn) {
    auto it = info.caps.find("role");
    if (it == info.caps.end()) return false;
    auto visit = [&fn](const Value& item) {
        return item.type() == Value::Type::String && !item.as_string().empty() &&
               fn(item.as_string());
    };
    const Value& role = it->second;
    if (role.type() != Value::Type::List) return visit(role);
    for (const Value& item : role.as_list()) {
        if (visit(item)) return true;
    }
    return false;
}

}  // namespace

bool has_role(const DeviceInfo& info, std::string_view role) {
    return any_role(info, [role](const std::string& r) { return r == role; });
}

std::vector<std::string> roles_of(const DeviceInfo& info) {
    std::vector<std::string> roles;
    any_role(info, [&roles](const std::string& r) {
        roles.push_back(r);
        return false;
    });
    return roles;
}

}  // namespace wisp::hal
