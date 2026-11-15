#pragma once

// Roles: what a device is for, so portable code can ask for "the main
// display" without knowing the board's id (spec 2.3). The board declares a
// device's roles in caps "role"; wisp::core::DeviceRegistry::by_role<T>()
// finds the device, and add() keeps each (interface, role) pair unique
// (for a generic device, interface None: each (kind, role) pair, the kind
// being the id's prefix before the dot).
//
// The vocabulary portable code relies on:
//
//   Interface  Role                                   Used by
//   ---------  -------------------------------------  ---------------------------------
//   Display    main                                   UiRuntime, self-check, system
//                                                     duties, simulator tools
//   Touch      main                                   UiRuntime (the pointer over the
//                                                     main display)
//   AudioOut   main                                   TonePlayer, PcmPlayer, loopback
//   AudioIn    main                                   VoiceService, loopback
//   Power      main                                   PowerMonitor, net card,
//                                                     self-check, power-off
//   Clock      main                                   RTC sync at boot and after SNTP
//   Imu        main                                   SensorService
//   Wifi       main                                   WifiService
//   Storage    internal, removable                    StorageManager, self-check
//   Button     power                                  ButtonService standby polling,
//                                                     PowerManager, system duties
//   Button     talk                                   TalkKey
//   Button     nav_prev, nav_next, nav_ok, nav_back   LVGL keypad
//
// A board may use other names for devices only it or the generic path
// cares about; no portable code looks those up.

#include <string>
#include <string_view>
#include <vector>

#include "wisp/hal/device.hpp"

namespace wisp::hal::role {
inline constexpr const char* kMain = "main";
inline constexpr const char* kInternal = "internal";    // Storage
inline constexpr const char* kRemovable = "removable";  // Storage
inline constexpr const char* kPower = "power";          // Button
inline constexpr const char* kTalk = "talk";            // Button
inline constexpr const char* kNavPrev = "nav_prev";     // Button, device model spec 9.1
inline constexpr const char* kNavNext = "nav_next";
inline constexpr const char* kNavOk = "nav_ok";
inline constexpr const char* kNavBack = "nav_back";
}  // namespace wisp::hal::role

namespace wisp::hal {

// The interfaces whose devices portable code looks up by role main (the
// "main" rows of the table above). Kept beside the vocabulary so the two
// cannot drift; core::main_role_gaps() checks a registry against it.
inline constexpr Interface kInterfacesWithMain[] = {
    Interface::Display, Interface::Touch,    Interface::Power,   Interface::Clock,
    Interface::Imu,     Interface::AudioOut, Interface::AudioIn, Interface::Wifi,
};

// caps "role": a string, or a list of strings (a device serving two
// purposes, e.g. one key that is both the power and the talk key);
// anything else, including non-string list items and empty strings, is
// no role.
bool has_role(const DeviceInfo& info, std::string_view role);
std::vector<std::string> roles_of(const DeviceInfo& info);

}  // namespace wisp::hal
