#pragma once

// What the agent knows about the device when it builds a request: the
// values a context source gathers without touching flash (cached state
// only), since it runs on the wisp.agent task.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "wisp/agent/conversation.hpp"
#include "wisp/hal/display_geometry.hpp"

namespace wisp::agent {

struct DeviceSnapshot {
    std::string lang = "zh";  // "zh" | "ja" | "en"
    hal::ScreenShape shape;
    std::string foreground;  // "" none
    std::vector<std::string> background;
    int battery = -1;
    bool charging = false;
    std::string wifi;
    int64_t wall_s = 0;
    int utc_offset_min = 480;
    std::vector<std::string> memory;  // "[<id>] <text>"
    int weekday = 0;                  // 1 = Monday; 0 while the clock is unset
    Source source = Source::Web;      // the running turn's
};

// Called on the wisp.agent task once per request, with the running turn's
// source.
using ContextSource = std::function<DeviceSnapshot(Source)>;

}  // namespace wisp::agent
