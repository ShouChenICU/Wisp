#include "wisp/os/flash_guard.hpp"

#include <gtest/gtest.h>

#include <string>

#include "wisp/os/os.hpp"

using namespace wisp::os;

namespace {

// A plain function, not a lambda: FlashViolationReporter is a function
// pointer, which cannot carry a capture. State lives at file scope
// instead and each test resets it before use.
const char* g_last_reported = nullptr;
int g_report_count = 0;

void capture_violation(const char* what) {
    g_last_reported = what;
    ++g_report_count;
}

}  // namespace

// detail::flash_guard_report() -- the hook WISP_ASSERT_FLASH_THREAD's
// ESP-IDF device branch calls instead of naming WLOG_E directly (which
// would need wisp/core/log.hpp, above wisp_os, in scope at every call
// site) -- reaches whatever set_flash_violation_reporter() installed,
// unaltered. Exercised directly here since the installed reporter is
// wisp_core's job (log.cpp installs one that calls WLOG_E), not
// wisp_os's.
TEST(FlashGuard, ReporterHookReceivesTheCallSite) {
    g_last_reported = nullptr;
    g_report_count = 0;
    set_flash_violation_reporter(&capture_violation);

    detail::flash_guard_report("StorageManager::write_atomic");

    EXPECT_EQ(g_report_count, 1);
    ASSERT_NE(g_last_reported, nullptr);
    EXPECT_STREQ(g_last_reported, "StorageManager::write_atomic");

    set_flash_violation_reporter(nullptr);
}

// No reporter installed (the default, and every host/sim build, which
// never reaches this hook through the macro at all) is a silent no-op,
// not a crash.
TEST(FlashGuard, NoReporterInstalledIsSilent) {
    set_flash_violation_reporter(nullptr);
    detail::flash_guard_report("StorageManager::write_atomic");
}

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

// Only the registered thread can unregister itself: a clear from any
// other thread (say, a second worker that has stopped while the first is
// still running) leaves the registration in place, so the guard keeps
// rejecting everyone but the flash thread.
TEST(FlashGuard, ClearFromAnotherThreadKeepsTheRegistration) {
    Semaphore registered(1, 0);
    Semaphore release(1, 0);
    auto owner = Task::spawn({}, [&] {
        set_flash_thread();
        registered.give();
        release.take(kForever);
        clear_flash_thread();
    });
    registered.take(kForever);

    clear_flash_thread();  // not the owner: ignored
    bool still_rejected = !on_flash_thread();

    auto other = Task::spawn({}, [] { clear_flash_thread(); });
    other->join();
    bool still_rejected_after_other = !on_flash_thread();

    release.give();
    owner->join();

    EXPECT_TRUE(still_rejected);
    EXPECT_TRUE(still_rejected_after_other);
    EXPECT_TRUE(on_flash_thread());  // the owner's own clear did unregister it
}

// A thread that registers after another one takes over; the earlier
// thread's later clear does not undo the newer registration.
TEST(FlashGuard, StaleOwnerClearDoesNotUndoANewerRegistration) {
    Semaphore a_registered(1, 0);
    Semaphore b_registered(1, 0);
    Semaphore a_cleared(1, 0);
    Semaphore release_b(1, 0);
    bool a_after_takeover = true;

    auto a = Task::spawn({}, [&] {
        set_flash_thread();
        a_registered.give();
        b_registered.take(kForever);
        a_after_takeover = on_flash_thread();
        clear_flash_thread();
        a_cleared.give();
    });
    a_registered.take(kForever);
    auto b = Task::spawn({}, [&] {
        set_flash_thread();
        b_registered.give();
        release_b.take(kForever);
        clear_flash_thread();
    });
    a_cleared.take(kForever);
    a->join();
    bool main_rejected = !on_flash_thread();
    release_b.give();
    b->join();

    EXPECT_FALSE(a_after_takeover);
    EXPECT_TRUE(main_rejected);
    EXPECT_TRUE(on_flash_thread());
}

// Off the flash thread, WISP_ASSERT_FLASH_THREAD's host/test build prints
// the call site and aborts (spec 3.2), so misplaced flash access fails
// the very ctest that exercises it instead of only showing up on
// hardware.
TEST(FlashGuardDeathTest, AssertAbortsOffTheFlashThread) {
    // Another thread stays registered across the death test, so it runs
    // in the "threadsafe" style (re-executes the binary) rather than
    // forking a multi-threaded process; the style is restored for the
    // tests after this one.
    struct RestoreStyle {
        std::string old = GTEST_FLAG_GET(death_test_style);
        ~RestoreStyle() { GTEST_FLAG_SET(death_test_style, old); }
    } restore;
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Semaphore registered(1, 0);
    Semaphore release(1, 0);
    auto owner = Task::spawn({}, [&] {
        set_flash_thread();
        registered.give();
        release.take(kForever);
        clear_flash_thread();
    });
    registered.take(kForever);

    // The test's own thread was never registered, so this trips the
    // guard.
    EXPECT_DEATH(
        { WISP_ASSERT_FLASH_THREAD("StorageManager::write_atomic"); },
        "flash access off the flash thread.*write_atomic");

    release.give();
    owner->join();
}
