#pragma once

// Serial console (connectivity spec 8.2): line-based commands read through
// Platform::read_line -- USB serial on the device, the process stdin in the
// simulator. Parsing and the commands live here, in the portable layer;
// only reading the input is platform code.
//
// Commands:
//   help
//   wifi status                  state=<s> ssid=<x> ip=<y> rssi=<n>[ reason=<r>],
//                                then one "saved ssid=<x>[ auth_failed]" line
//                                per saved network and, while the hotspot is
//                                up, "ap ssid=<x> clients=<n>"
//   wifi add <ssid> [password]   "ok" or "error: <message>"
//   wifi forget <ssid>
//   wifi ap on                   "ap ssid=<x> ip=<y>"
//   wifi ap off
// plus Platform::console_commands. Anything else: "error: unknown command".
//
// An <ssid> argument is taken byte for byte as typed (UTF-8 from a
// terminal); "hex:<bytes>" names one by its bytes instead, for SSIDs that
// are not valid UTF-8 (the same bytes the admin API reports as ssid_hex).
// A printed <x> is double-quoted with \" and \\ escaped and every byte
// that is a control character or not part of valid UTF-8 written as \xNN,
// so a raw SSID can never break the serial line.
//
// Secrets: nothing printed contains a Wi-Fi or hotspot password (the
// hotspot password only shows on the device screen), and the console
// never echoes or logs an input line. Output goes to Platform::emit_line,
// not through WLOG, so it never reaches the log ring either. Local echo
// done by the terminal program itself is outside the device's control.

#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"

namespace wisp::app {

class App;

// Splits a line into words at runs of spaces/tabs. Double quotes group
// words with spaces; inside them \" and \\ stand for " and \ (a backslash
// before anything else is kept as is). Quoted and unquoted parts next to
// each other join into one word. An unterminated quote fails with
// InvalidArgument (the message never quotes the line).
Result<std::vector<std::string>> split_command(std::string_view line);

// Runs one console line against `app` and returns what to print -- possibly
// several lines joined by '\n', "" for a blank line. Safe to call from any
// thread while `app` is started.
std::string run_command(App& app, std::string_view line);

// The quoted, escaped form of an SSID described in the file header.
std::string quote_ssid(std::string_view ssid);

}  // namespace wisp::app
