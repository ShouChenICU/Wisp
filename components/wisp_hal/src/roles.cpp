#include "wisp/hal/roles.hpp"

namespace wisp::hal {

namespace {

// Calls `fn` with every role string in caps "role", stopping early when it
// returns true; returns whether it did.
template <class Fn>
bool any_role(const DeviceInfo& info, Fn fn) {
    auto it = info.caps.find("role");
    if (it == info.caps.end()) return false;
    const Value& role = it->second;
    if (role.type() == Value::Type::String) return fn(role.as_string());
    if (role.type() != Value::Type::List) return false;
    for (const Value& item : role.as_list()) {
        if (item.type() == Value::Type::String && fn(item.as_string())) return true;
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
