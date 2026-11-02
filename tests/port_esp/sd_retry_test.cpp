// Unit tests for the pure SD-card retry/backoff policy used by
// wisp::port::esp::EspSdStorage::online() (see wisp/port/esp/sd_retry.hpp's
// file comment). Compiles on the host without any ESP-IDF include -- see
// tests/CMakeLists.txt's sd_retry_test target, which adds
// components/wisp_port_esp/include to this translation unit's include
// path.

#include "wisp/port/esp/sd_retry.hpp"

#include <gtest/gtest.h>

using wisp::port::esp::SdCardState;
using wisp::port::esp::SdRetryState;

TEST(SdRetry, StartsNotMountedAndRetriesImmediately) {
    SdRetryState state(2000);
    EXPECT_FALSE(state.mounted());
    // No attempt has ever been made: the very first should_retry() call
    // (at any timestamp, including 0) is true.
    EXPECT_TRUE(state.should_retry(0));
    EXPECT_TRUE(state.should_retry(12345));
}

TEST(SdRetry, MarkMountedStopsRetrying) {
    SdRetryState state(2000);
    state.mark_mounted();
    EXPECT_TRUE(state.mounted());
    EXPECT_FALSE(state.should_retry(0));
    EXPECT_FALSE(state.should_retry(999999));
}

TEST(SdRetry, FailedAttemptBlocksRetryUntilIntervalElapses) {
    SdRetryState state(2000);
    state.mark_unmounted(1000);
    EXPECT_FALSE(state.mounted());
    EXPECT_FALSE(state.should_retry(1000));  // no time elapsed
    EXPECT_FALSE(state.should_retry(2999));  // 1999ms elapsed, still < 2000
    EXPECT_TRUE(state.should_retry(3000));   // exactly 2000ms elapsed
    EXPECT_TRUE(state.should_retry(5000));   // well past the interval
}

TEST(SdRetry, UnmountAfterMountedResetsAttemptClock) {
    SdRetryState state(2000);
    state.mark_mounted();
    state.mark_unmounted(10000);  // card disappeared at t=10000
    EXPECT_FALSE(state.should_retry(10000));
    EXPECT_FALSE(state.should_retry(11999));
    EXPECT_TRUE(state.should_retry(12000));
}

TEST(SdRetry, RepeatedFailuresKeepPushingTheWindowOut) {
    SdRetryState state(2000);
    state.mark_unmounted(0);
    EXPECT_TRUE(state.should_retry(2000));
    // A second failed attempt at t=2000 pushes the next retry to 4000.
    state.mark_unmounted(2000);
    EXPECT_FALSE(state.should_retry(3999));
    EXPECT_TRUE(state.should_retry(4000));
}

// --- Fix round 1: the worker-thread Mounted/Removed/Unmounted cycle. ---

TEST(SdRetry, MarkRemovedTransitionsFromMountedAndStopsNeitherMountedNorRetrying) {
    SdRetryState state(2000);
    state.mark_mounted();
    state.mark_removed();
    EXPECT_EQ(state.state(), SdCardState::Removed);
    EXPECT_FALSE(state.mounted());
    EXPECT_TRUE(state.removed());
    // Removed is not a retry candidate yet -- online() must unmount
    // (mark_unmounted()) before a new attempt is allowed.
    EXPECT_FALSE(state.should_retry(0));
    EXPECT_FALSE(state.should_retry(999999));
}

TEST(SdRetry, MarkRemovedIsANoOpUnlessCurrentlyMounted) {
    SdRetryState state(2000);
    // Not mounted yet: mark_removed() must not fabricate a Removed state
    // (the worker only ever calls it from its own Mounted-branch code
    // path, but the guard is defensive).
    state.mark_removed();
    EXPECT_EQ(state.state(), SdCardState::Unmounted);
    EXPECT_TRUE(state.should_retry(0));
}

TEST(SdRetry, FullMountedRemovedUnmountedRetryCycle) {
    SdRetryState state(2000);
    state.mark_mounted();
    EXPECT_TRUE(state.mounted());

    // Worker's presence probe fails.
    state.mark_removed();
    EXPECT_TRUE(state.removed());

    // online() unmounts and transitions to Unmounted at t=5000.
    state.mark_unmounted(5000);
    EXPECT_EQ(state.state(), SdCardState::Unmounted);
    EXPECT_FALSE(state.should_retry(5000));
    EXPECT_FALSE(state.should_retry(6999));
    EXPECT_TRUE(state.should_retry(7000));

    // Worker remounts successfully.
    state.mark_mounted();
    EXPECT_TRUE(state.mounted());
    EXPECT_FALSE(state.should_retry(7000));
}
