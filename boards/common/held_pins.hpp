#pragma once

// The list of output pins the board kit keeps driving through automatic
// light sleep (boards/common/board_kit.cpp, hold_outputs()). Pure, so the
// host tests cover it (tests/boards/held_pins_test.cpp).
//
// The pins come from the bmgr YAML: a pin the YAML leaves at -1 (a board
// without a touch reset line) is skipped, and a pin named twice (PA and
// CS sharing a GPIO) is held once, keeping the order of first mention.

#include <array>
#include <cstddef>

namespace wisp::board_kit {

inline constexpr size_t kMaxHeldPins = 8;

struct HeldPins {
    std::array<int, kMaxHeldPins> pins{};
    size_t count = 0;

    // False, and the list unchanged, for a negative pin, a pin already
    // held or a full list.
    bool add(int pin) {
        if (pin < 0 || count == pins.size()) {
            return false;
        }
        for (size_t i = 0; i < count; ++i) {
            if (pins[i] == pin) {
                return false;
            }
        }
        pins[count++] = pin;
        return true;
    }
};

}  // namespace wisp::board_kit
