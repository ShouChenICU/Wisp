#include <chrono>
#include <utility>

#include "sim_devices.hpp"

namespace wisp::port::sim {

namespace {

int64_t wall_clock_seconds() {
    using std::chrono::duration_cast;
    using std::chrono::seconds;
    using std::chrono::system_clock;
    return static_cast<int64_t>(
        duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace

SimClock::SimClock(std::string id) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "system-clock";
}

wisp::Result<int64_t> SimClock::get_unix() {
    return wisp::Result<int64_t>::ok(wall_clock_seconds() + offset_seconds_.load());
}

wisp::Result<void> SimClock::set_unix(int64_t seconds) {
    offset_seconds_.store(seconds - wall_clock_seconds());
    return wisp::Result<void>::ok();
}

}  // namespace wisp::port::sim
