// CompletionBox (scene_runtime.hpp, "Results"): one VM's finished
// wisp.http / wisp.files calls waiting for the scene task.

#include "completion_box.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>

#include "wisp/os/os.hpp"
#include "wisp/port/sim/sim_os.hpp"

namespace {

using wisp::scene::CompletionBox;
using wisp::scene::FilesResult;

CompletionBox::Payload http_result(int status) {
    wisp::net::SceneHttpResult r;
    r.status = status;
    return CompletionBox::Payload(std::move(r));
}

TEST(CompletionBox, KeepsOrderAndAsksForOneWake) {
    auto box = CompletionBox::make();
    ASSERT_TRUE(box);
    bool wake = false;
    ASSERT_TRUE(box->put(1, false, http_result(201), &wake));
    EXPECT_TRUE(wake);
    ASSERT_TRUE(box->put(2, true, CompletionBox::Payload(FilesResult{"busy", {}}), &wake));
    EXPECT_FALSE(wake);
    ASSERT_TRUE(box->put(3, false, http_result(203), &wake));
    EXPECT_FALSE(wake);
    EXPECT_FALSE(box->empty());

    CompletionBox::Item item;
    ASSERT_TRUE(box->take(item));
    EXPECT_EQ(item.id, 1u);
    EXPECT_FALSE(item.files);
    ASSERT_TRUE(item.payload);
    ASSERT_NE(std::get_if<wisp::net::SceneHttpResult>(item.payload.get()), nullptr);
    EXPECT_EQ(std::get<wisp::net::SceneHttpResult>(*item.payload).status, 201);
    ASSERT_TRUE(box->take(item));
    EXPECT_EQ(item.id, 2u);
    EXPECT_TRUE(item.files);
    ASSERT_TRUE(item.payload);
    EXPECT_EQ(std::get<FilesResult>(*item.payload).error, "busy");
    ASSERT_TRUE(box->take(item));
    EXPECT_EQ(item.id, 3u);
    EXPECT_FALSE(box->take(item));
    EXPECT_TRUE(box->empty());

    ASSERT_TRUE(box->put(4, false, http_result(204), &wake));
    EXPECT_TRUE(wake);
}

TEST(CompletionBox, FailedWakeIsAskedForAgain) {
    auto box = CompletionBox::make();
    ASSERT_TRUE(box);
    bool wake = false;
    ASSERT_TRUE(box->put(1, false, http_result(200), &wake));
    EXPECT_TRUE(wake);
    box->wake_failed();
    ASSERT_TRUE(box->put(2, false, http_result(200), &wake));
    EXPECT_TRUE(wake);
}

TEST(CompletionBox, CloseFreesAndRefuses) {
    auto box = CompletionBox::make();
    ASSERT_TRUE(box);
    bool wake = false;
    ASSERT_TRUE(box->put(1, false, http_result(200), &wake));
    ASSERT_TRUE(box->put(2, true, CompletionBox::Payload(FilesResult{}), &wake));
    EXPECT_FALSE(box->closed());
    EXPECT_EQ(box->close(), 2u);
    EXPECT_TRUE(box->closed());
    wake = true;
    EXPECT_FALSE(box->put(3, false, http_result(200), &wake));
    EXPECT_FALSE(wake);
    CompletionBox::Item item;
    EXPECT_FALSE(box->take(item));
    EXPECT_TRUE(box->empty());
    EXPECT_EQ(box->close(), 0u);
}

TEST(CompletionBox, NoMemoryStillKeepsTheCall) {
    auto box = CompletionBox::make();
    ASSERT_TRUE(box);
    bool wake = false;
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, true);
    const bool put = box->put(7, true, CompletionBox::Payload(FilesResult{}), &wake);
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, false);
    ASSERT_TRUE(put);
    EXPECT_TRUE(wake);
    CompletionBox::Item item;
    ASSERT_TRUE(box->take(item));
    EXPECT_EQ(item.id, 7u);
    EXPECT_TRUE(item.files);
    EXPECT_EQ(item.payload, nullptr);
}

TEST(CompletionBox, FullRingRefuses) {
    auto box = CompletionBox::make();
    ASSERT_TRUE(box);
    bool wake = false;
    for (uint32_t i = 0; i < CompletionBox::kCapacity; ++i)
        ASSERT_TRUE(box->put(i + 1, false, http_result(200), &wake)) << i;
    EXPECT_FALSE(box->put(99, false, http_result(200), &wake));
    EXPECT_FALSE(wake);
    EXPECT_FALSE(box->closed());
    // Taking one makes room again, oldest first, across the ring's end.
    CompletionBox::Item item;
    ASSERT_TRUE(box->take(item));
    EXPECT_EQ(item.id, 1u);
    EXPECT_TRUE(box->put(100, false, http_result(200), &wake));
    for (uint32_t i = 2; i <= CompletionBox::kCapacity; ++i) {
        ASSERT_TRUE(box->take(item));
        EXPECT_EQ(item.id, i);
    }
    ASSERT_TRUE(box->take(item));
    EXPECT_EQ(item.id, 100u);
    EXPECT_FALSE(box->take(item));
}

TEST(CompletionBox, MakeFailsWithoutMemory) {
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, true);
    auto box = CompletionBox::make();
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, false);
    EXPECT_FALSE(box);
}

}  // namespace
