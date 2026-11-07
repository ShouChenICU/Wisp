// SceneRecents (wisp/shell/scene_recents.hpp, mvp spec 6.4): the scene
// list's order, its file and how often it is written.

#include "wisp/shell/scene_recents.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "support/agent_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/os/os.hpp"

using wisp::shell::SceneRecents;

namespace {

class SceneRecentsTest : public ::testing::Test {
protected:
    std::unique_ptr<SceneRecents> make() {
        return std::make_unique<SceneRecents>(*h.storage, path, [this](std::function<void()> job) {
            ++saves;
            return h.worker.submit([this, job = std::move(job)] {
                job();
                ++written;
            });
        });
    }
    void on_worker(const std::function<void()>& fn) {
        ASSERT_TRUE(h.worker.run_sync(fn, wisp::os::kForever));
    }

    wisp::test::AgentHarness h;
    std::string path =
        h.storage->write_root(wisp::core::Area::System).value() + "/shell/recents.json";
    std::atomic<int> saves{0};
    std::atomic<int> written{0};
};

}  // namespace

TEST_F(SceneRecentsTest, OrderAndPersistence) {
    auto r = make();
    on_worker([&] { ASSERT_TRUE(r->load()); });  // no file yet
    const std::vector<std::string> ids{"clock", "counter", "alpha", "todo"};
    EXPECT_EQ(r->order(ids), (std::vector<std::string>{"alpha", "clock", "counter", "todo"}));

    r->opened("todo");
    r->opened("clock");
    const std::vector<std::string> want{"clock", "todo", "alpha", "counter"};
    EXPECT_EQ(r->order(ids), want);
    // Both changes, one write, a second after the first.
    EXPECT_EQ(saves.load(), 0);
    ASSERT_TRUE(wisp::test::wait_until([&] { return written.load() == 1; }, 3000));
    wisp::os::sleep_ms(SceneRecents::kSaveDelayMs + 300);
    EXPECT_EQ(saves.load(), 1);

    std::string text;
    on_worker([&] {
        auto bytes = h.storage->read_file(path);
        ASSERT_TRUE(bytes);
        text.assign(bytes.value().begin(), bytes.value().end());
    });
    EXPECT_NE(text.find("\"version\":1"), std::string::npos) << text;
    EXPECT_NE(text.find("\"recent\""), std::string::npos) << text;

    auto again = make();
    on_worker([&] { ASSERT_TRUE(again->load()); });
    EXPECT_EQ(again->order(ids), want);

    // Opening it again moves it up; deleting drops it.
    again->opened("todo");
    again->forget("clock");
    EXPECT_EQ(again->order(ids), (std::vector<std::string>{"todo", "alpha", "clock", "counter"}));
    on_worker([&] { ASSERT_TRUE(again->flush()); });
    auto third = make();
    on_worker([&] { ASSERT_TRUE(third->load()); });
    EXPECT_EQ(third->order(ids), (std::vector<std::string>{"todo", "alpha", "clock", "counter"}));
}

TEST_F(SceneRecentsTest, MalformedFileAndForgetAll) {
    on_worker([&] {
        const std::string bad = "{not json";
        ASSERT_TRUE(h.storage->write_atomic(
            path, {reinterpret_cast<const uint8_t*>(bad.data()), bad.size()}));
    });
    auto r = make();
    on_worker([&] { EXPECT_TRUE(r->load()); });
    const std::vector<std::string> ids{"b", "a"};
    EXPECT_EQ(r->order(ids), (std::vector<std::string>{"a", "b"}));
    r->forget_all();
    r->opened("b");
    EXPECT_EQ(r->order(ids), (std::vector<std::string>{"a", "b"}));
    wisp::os::sleep_ms(SceneRecents::kSaveDelayMs + 300);
    EXPECT_EQ(saves.load(), 0);
}
