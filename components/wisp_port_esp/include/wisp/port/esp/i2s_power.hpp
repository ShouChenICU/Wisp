#pragma once

// Pure, platform-free decision behind EspAudioOut/EspAudioIn::set_power()
// (dev/esp_audio.cpp): given what the I2S driver reports about a channel,
// what one set_power() call should do to it. No ESP-IDF include, so it is
// unit-tested on the host (tests/port_esp/i2s_power_test.cpp).
//
// Two refusals, both reported as ErrorCode::Busy and leaving the channel on:
//   - A stream is open on the device. Disabling the channel mid-stream
//     truncates the write in flight and makes every later read/write time
//     out; the owner closes the stream first.
//   - The channel is a duplex TX whose paired RX is still enabled. In
//     duplex the TX side drives BCLK/WS for both directions, so stopping
//     it first would leave RX running without a clock. Refusing (rather
//     than remembering to stop TX later) makes RX-before-TX hold whatever
//     order the two devices are stepped down in: a refused TX is simply
//     stepped down again on the next call, after RX has gone off.
// Going up has no such guard; the caller enables TX before RX.

namespace wisp::port::esp {

struct I2sChannelView {
    bool enabled = false;       // i2s_chan_info_t::is_enabled
    bool is_tx = false;         // direction of this channel
    bool peer_enabled = false;  // the duplex pair channel exists and is enabled
    bool stream_open = false;   // the device has an open stream on it
};

enum class I2sPowerStep { None, Enable, Disable, BusyStreamOpen, BusyPeerRunning };

constexpr I2sPowerStep plan_i2s_power(const I2sChannelView& channel, bool want_on) {
    if (channel.enabled == want_on) {
        return I2sPowerStep::None;
    }
    if (want_on) {
        return I2sPowerStep::Enable;
    }
    if (channel.stream_open) {
        return I2sPowerStep::BusyStreamOpen;
    }
    if (channel.is_tx && channel.peer_enabled) {
        return I2sPowerStep::BusyPeerRunning;
    }
    return I2sPowerStep::Disable;
}

}  // namespace wisp::port::esp
