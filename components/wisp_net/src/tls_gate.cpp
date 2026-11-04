#include "wisp/net/tls_gate.hpp"

namespace wisp::net {

TlsGate::TlsGate() : sem_(1, 1) {}

TlsGate& TlsGate::process() {
    static TlsGate gate;
    return gate;
}

TlsGate::Hold::Hold(TlsGate& gate, uint32_t timeout_ms)
    : gate_(gate), held_(gate.sem_.take(timeout_ms)) {}

TlsGate::Hold::~Hold() {
    if (held_) gate_.sem_.give();
}

}  // namespace wisp::net
