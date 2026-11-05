#include "wisp/port/sim/sim_sntp.hpp"

#include <ctime>
#include <string_view>

#include "wisp/base/error.hpp"

namespace wisp::port::sim {

Result<int64_t> SimSntp::query(const std::string& host, uint32_t) {
    constexpr std::string_view kInvalid = ".invalid";
    if (host.size() >= kInvalid.size() &&
        std::string_view(host).substr(host.size() - kInvalid.size()) == kInvalid) {
        return Result<int64_t>::err(ErrorCode::Timeout, "no answer from " + host);
    }
    return Result<int64_t>::ok(static_cast<int64_t>(std::time(nullptr)));
}

}  // namespace wisp::port::sim
