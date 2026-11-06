#include "wisp/os/flash_guard.hpp"

#include <gtest/gtest.h>

#include "wisp/os/os.hpp"

using namespace wisp::os;

// With no flash thread ever registered (the common case for a test that
// never starts core::StorageWorker), the guard allows everything, on any
// thread -- this is the boot/shutdown-phase behavior spec 3.2 documents.
TEST(FlashGuard, AllowsEverythingWithNoFlashThread) {
    EXPECT_TRUE(on_flash_thread());

    Semaphore done(1, 0);
    bool other_thread_allowed = false;
    auto other = Task::spawn({}, [&] {
        other_thread_allowed = on_flash_thread();
        done.give();
    });
    done.take(kForever);
    other->join();

    EXPECT_TRUE(other_thread_allowed);
}

// Once a thread registers itself, it alone is the flash thread; every
// other thread sees on_flash_thread() == false until clear_flash_thread()
// unregisters it, at which point everything is allowed again (same as
// before any registration).
TEST(FlashGuard, OnlyTheRegisteredThreadIsTheFlashThread) {
    Semaphore registered(1, 0);
    Semaphore checked(1, 0);
    Semaphore cleared(1, 0);
    Semaphore finished(1, 0);

    bool a_sees_true_once_registered = false;
    bool a_sees_true_after_clear = false;

    auto thread_a = Task::spawn({}, [&] {
        set_flash_thread();
        a_sees_true_once_registered = on_flash_thread();
        registered.give();

        checked.take(kForever);
        clear_flash_thread();
        a_sees_true_after_clear = on_flash_thread();
        cleared.give();

        finished.take(kForever);
    });

    registered.take(kForever);
    bool b_sees_false_while_a_is_registered = on_flash_thread();
    checked.give();

    cleared.take(kForever);
    bool b_sees_true_after_clear = on_flash_thread();
    finished.give();
    thread_a->join();

    EXPECT_TRUE(a_sees_true_once_registered);
    EXPECT_FALSE(b_sees_false_while_a_is_registered);
    EXPECT_TRUE(a_sees_true_after_clear);
    EXPECT_TRUE(b_sees_true_after_clear);
}

// Off the flash thread, WISP_ASSERT_FLASH_THREAD's host/test build prints
// the call site and aborts (spec 3.2), so misplaced flash access fails
// the very ctest that exercises it instead of only showing up on
// hardware.
TEST(FlashGuardDeathTest, AssertAbortsOffTheFlashThread) {
    // Registered by a thread that has since exited and been joined --
    // registration is global, so it stays in effect -- rather than kept
    // alive across the EXPECT_DEATH fork() below, which gtest otherwise
    // warns about when other threads are still running.
    auto setter = Task::spawn({}, [] { set_flash_thread(); });
    setter->join();

    // The test's own thread was never registered, so this trips the
    // guard.
    EXPECT_DEATH(
        { WISP_ASSERT_FLASH_THREAD("StorageManager::write_atomic"); },
        "flash access off the flash thread.*write_atomic");

    // Leave global registration state as this test found it (cleared),
    // since it is process-wide and other tests in this binary run after
    // it.
    clear_flash_thread();
}
