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
bool g_last_sd = false;
int g_report_count = 0;

void capture_violation(const char* what, bool sd) {
    g_last_reported = what;
    g_last_sd = sd;
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

    detail::flash_guard_report("StorageManager::write_atomic", false);

    EXPECT_EQ(g_report_count, 1);
    ASSERT_NE(g_last_reported, nullptr);
    EXPECT_STREQ(g_last_reported, "StorageManager::write_atomic");
    EXPECT_FALSE(g_last_sd);

    set_flash_violation_reporter(nullptr);
}

// An SD-rule violation reaches the same reporter marked as such, so the
// installed reporter can word it as an SD violation rather than a flash
// one.
TEST(FlashGuard, ReporterHookTellsAnSdViolationApart) {
    g_last_reported = nullptr;
    g_report_count = 0;
    set_flash_violation_reporter(&capture_violation);

    detail::flash_guard_report("StorageManager::append", true);

    EXPECT_EQ(g_report_count, 1);
    ASSERT_NE(g_last_reported, nullptr);
    EXPECT_STREQ(g_last_reported, "StorageManager::append");
    EXPECT_TRUE(g_last_sd);

    set_flash_violation_reporter(nullptr);
}

// No reporter installed (the default, and every host/sim build, which
// never reaches this hook through the macro at all) is a silent no-op,
// not a crash.
TEST(FlashGuard, NoReporterInstalledIsSilent) {
    set_flash_violation_reporter(nullptr);
    detail::flash_guard_report("StorageManager::write_atomic", false);
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

// The SD I/O thread registers the same way as the flash thread: only the
// registered thread sees on_sd_io_thread(), and its own clear ends that.
TEST(FlashGuard, SdIoThreadRegistersLikeTheFlashThread) {
    EXPECT_FALSE(on_sd_io_thread());

    Semaphore registered(1, 0);
    Semaphore checked(1, 0);
    Semaphore cleared(1, 0);
    bool on_task = false;
    auto sd = Task::spawn({}, [&] {
        set_sd_io_thread();
        on_task = on_sd_io_thread();
        registered.give();
        checked.take(kForever);
        clear_sd_io_thread();
        cleared.give();
    });
    registered.take(kForever);
    bool on_main = on_sd_io_thread();
    checked.give();
    cleared.take(kForever);
    sd->join();

    EXPECT_TRUE(on_task);
    EXPECT_FALSE(on_main);
    EXPECT_FALSE(on_sd_io_thread());
}

// The SD I/O thread must never touch internal flash, so it is not the
// flash thread even in the phases where nothing holds the flash role.
TEST(FlashGuard, SdIoThreadIsNeverTheFlashThread) {
    Semaphore registered(1, 0);
    Semaphore release(1, 0);
    bool sd_on_flash = true;
    auto sd = Task::spawn({}, [&] {
        set_sd_io_thread();
        sd_on_flash = on_flash_thread();
        registered.give();
        release.take(kForever);
        clear_sd_io_thread();
    });
    registered.take(kForever);
    bool main_on_flash = on_flash_thread();
    release.give();
    sd->join();

    EXPECT_FALSE(sd_on_flash);
    EXPECT_TRUE(main_on_flash);
}

// SD access is allowed on the SD I/O thread and wherever flash access is.
TEST(FlashGuard, SdRuleAllowsTheSdAndFlashThreads) {
    Semaphore f_registered(1, 0);
    Semaphore s_registered(1, 0);
    Semaphore f_check(1, 0);
    Semaphore s_check(1, 0);
    Semaphore f_checked(1, 0);
    Semaphore s_checked(1, 0);
    Semaphore release(2, 0);
    bool f_allowed = false;
    bool s_allowed = false;
    auto f = Task::spawn({}, [&] {
        set_flash_thread();
        f_registered.give();
        f_check.take(kForever);
        f_allowed = sd_io_allowed();
        f_checked.give();
        release.take(kForever);
        clear_flash_thread();
    });
    auto s = Task::spawn({}, [&] {
        set_sd_io_thread();
        s_registered.give();
        s_check.take(kForever);
        s_allowed = sd_io_allowed();
        s_checked.give();
        release.take(kForever);
        clear_sd_io_thread();
    });
    f_registered.take(kForever);
    s_registered.take(kForever);
    f_check.give();
    s_check.give();
    f_checked.take(kForever);
    s_checked.take(kForever);
    bool main_allowed = sd_io_allowed();
    release.give();
    release.give();
    f->join();
    s->join();

    EXPECT_TRUE(f_allowed);
    EXPECT_TRUE(s_allowed);
    EXPECT_FALSE(main_allowed);
}

// With neither role registered (boot, shutdown, tests that never start a
// worker) the SD rule allows everything, like the flash rule.
TEST(FlashGuard, SdRuleAllowsEverythingWithNothingRegistered) { EXPECT_TRUE(sd_io_allowed()); }

// Off both the SD I/O and the flash thread, WISP_ASSERT_SD_IO_THREAD's
// host build prints the call site and aborts, like the flash assert.
TEST(FlashGuardDeathTest, SdAssertAbortsOffBothThreads) {
    // Threads stay registered across the death test: threadsafe style, as
    // in AssertAbortsOffTheFlashThread.
    struct RestoreStyle {
        std::string old = GTEST_FLAG_GET(death_test_style);
        ~RestoreStyle() { GTEST_FLAG_SET(death_test_style, old); }
    } restore;
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    Semaphore registered(2, 0);
    Semaphore release(2, 0);
    auto f = Task::spawn({}, [&] {
        set_flash_thread();
        registered.give();
        release.take(kForever);
        clear_flash_thread();
    });
    auto s = Task::spawn({}, [&] {
        set_sd_io_thread();
        registered.give();
        release.take(kForever);
        clear_sd_io_thread();
    });
    registered.take(kForever);
    registered.take(kForever);

    EXPECT_DEATH(
        { WISP_ASSERT_SD_IO_THREAD("StorageManager::append"); },
        "sd access off the sd i/o thread.*append");

    release.give();
    release.give();
    f->join();
    s->join();
}

// The SD I/O thread reaching a flash entry point trips the flash assert,
// even with no flash thread registered.
TEST(FlashGuardDeathTest, FlashAssertAbortsOnTheSdIoThread) {
    struct RestoreStyle {
        std::string old = GTEST_FLAG_GET(death_test_style);
        ~RestoreStyle() { GTEST_FLAG_SET(death_test_style, old); }
    } restore;
    GTEST_FLAG_SET(death_test_style, "threadsafe");

    EXPECT_DEATH(
        {
            auto sd = Task::spawn({}, [] {
                set_sd_io_thread();
                WISP_ASSERT_FLASH_THREAD("x");
            });
            sd->join();
        },
        "flash access off the flash thread");
}
