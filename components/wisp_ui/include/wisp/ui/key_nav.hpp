#pragma once

// Keypad navigation (device-model spec 9.1): the keys a board's nav buttons
// map to. UiRuntime::push_key() feeds them to LVGL's keypad input device.

#include <cstdint>

namespace wisp::ui {

// Prev and Next move the focus, Ok activates the focused object, Back goes
// back. Back has no LVGL key: only the nav-key hook acts on it.
enum class NavKey : uint8_t { Prev, Next, Ok, Back };

}  // namespace wisp::ui
