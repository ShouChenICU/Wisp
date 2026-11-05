// Unit tests for the pure decision behind EspAudioOut/EspAudioIn::
// set_power() (wisp/port/esp/i2s_power.hpp). Compiles on the host without
// any ESP-IDF include.

#include "wisp/port/esp/i2s_power.hpp"

#include <gtest/gtest.h>

using wisp::port::esp::I2sChannelView;
using wisp::port::esp::I2sPowerStep;
using wisp::port::esp::plan_i2s_power;

namespace {

I2sChannelView view(bool enabled, bool is_tx, bool peer_enabled = false, bool stream_open = false) {
    I2sChannelView v;
    v.enabled = enabled;
    v.is_tx = is_tx;
    v.peer_enabled = peer_enabled;
    v.stream_open = stream_open;
    return v;
}

}  // namespace

TEST(I2sPower, AlreadyInTargetStateDoesNothing) {
    EXPECT_EQ(plan_i2s_power(view(true, true), /*want_on=*/true), I2sPowerStep::None);
    EXPECT_EQ(plan_i2s_power(view(false, false), /*want_on=*/false), I2sPowerStep::None);
    // Even with a stream open or a running peer: nothing to change.
    EXPECT_EQ(plan_i2s_power(view(false, true, true, true), false), I2sPowerStep::None);
}

TEST(I2sPower, EnablesWhenGoingUp) {
    EXPECT_EQ(plan_i2s_power(view(false, true), true), I2sPowerStep::Enable);
    EXPECT_EQ(plan_i2s_power(view(false, false, true), true), I2sPowerStep::Enable);
}

TEST(I2sPower, RefusesToStopAnOpenStream) {
    EXPECT_EQ(plan_i2s_power(view(true, true, false, true), false), I2sPowerStep::BusyStreamOpen);
    EXPECT_EQ(plan_i2s_power(view(true, false, false, true), false), I2sPowerStep::BusyStreamOpen);
}

TEST(I2sPower, TxStaysOnWhileRxRuns) {
    EXPECT_EQ(plan_i2s_power(view(true, true, /*peer_enabled=*/true), false),
              I2sPowerStep::BusyPeerRunning);
    EXPECT_EQ(plan_i2s_power(view(true, true, /*peer_enabled=*/false), false),
              I2sPowerStep::Disable);
}

TEST(I2sPower, RxStopsWhateverTxDoes) {
    EXPECT_EQ(plan_i2s_power(view(true, false, /*peer_enabled=*/true), false),
              I2sPowerStep::Disable);
}

// Whatever order the two devices are stepped down in, RX is never left
// running after TX has stopped.
TEST(I2sPower, RxGoesOffBeforeTxInEitherOrder) {
    // TX first: refused; RX then stops; TX stops on the next try.
    bool tx_on = true;
    bool rx_on = true;
    if (plan_i2s_power(view(tx_on, true, rx_on), false) == I2sPowerStep::Disable) tx_on = false;
    EXPECT_TRUE(tx_on);
    if (plan_i2s_power(view(rx_on, false, tx_on), false) == I2sPowerStep::Disable) rx_on = false;
    EXPECT_FALSE(rx_on);
    if (plan_i2s_power(view(tx_on, true, rx_on), false) == I2sPowerStep::Disable) tx_on = false;
    EXPECT_FALSE(tx_on);

    // RX first: both stop in one pass.
    tx_on = true;
    rx_on = true;
    if (plan_i2s_power(view(rx_on, false, tx_on), false) == I2sPowerStep::Disable) rx_on = false;
    if (plan_i2s_power(view(tx_on, true, rx_on), false) == I2sPowerStep::Disable) tx_on = false;
    EXPECT_FALSE(rx_on);
    EXPECT_FALSE(tx_on);
}
