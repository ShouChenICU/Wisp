#pragma once

// Simulator net::Sntp (connectivity spec 5.6): the host clock is already
// right, so every server "answers" with it at once and the time sync flow
// runs as on the device. A host under the reserved .invalid domain (RFC
// 6761) times out instead, so the fallback to the next server in
// ntp_servers can be tried.

#include <cstdint>
#include <string>

#include "wisp/base/result.hpp"
#include "wisp/net/time_sync.hpp"

namespace wisp::port::sim {

class SimSntp : public net::Sntp {
public:
    Result<int64_t> query(const std::string& host, uint32_t timeout_ms) override;
};

}  // namespace wisp::port::sim
