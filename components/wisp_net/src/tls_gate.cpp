#include "wisp/net/tls_gate.hpp"

namespace wisp::net {

TlsGate::TlsGate() : sem_(1, 1) {}

TlsGate& TlsGate::process() {
    static TlsGate gate;
    return gate;
}

TlsGate::Hold::Hold(TlsGate& gate, uint32_t timeout_ms)
    : gate_(gate), held_(gate.sem_.take(timeout_ms)) {}

TlsGate::Hold::Hold(TlsGate& gate, uint32_t timeout_ms, const std::function<bool()>& give_up)
    : gate_(gate) {
    // Waits in slices so that give_up is asked while the gate is busy.
    const uint64_t deadline = timeout_ms == os::kForever ? 0 : os::now_ms() + timeout_ms;
    for (;;) {
        if (give_up && give_up()) return;
        uint32_t slice = kGiveUpPollMs;
        if (deadline != 0) {
            const uint64_t now = os::now_ms();
            if (now >= deadline) {
                held_ = gate.sem_.take(0);
                return;
            }
            if (deadline - now < slice) slice = static_cast<uint32_t>(deadline - now);
        }
        if (gate.sem_.take(slice)) {
            held_ = true;
            return;
        }
    }
}

TlsGate::Hold::~Hold() {
    if (held_) gate_.sem_.give();
}

}  // namespace wisp::net
