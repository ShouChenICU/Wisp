// core::install_scene (scene-runtime-core spec 6.3): installing over a
// scene that is running must not lose what it has written, and installing
// one that is not running must not start it.
//
// The fake host keeps a real SceneStore for its "running" scene, written
// from its own thread and flushed every millisecond on the real storage
// worker, the way SceneRuntime's store is, so a flush lands inside the
// package swap unless the scene is closed first.

#include "wisp/core/scene_install.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/scene_store.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/zip.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::Value;
using wisp::core::SceneHost;
using wisp::core::SceneRepoConfig;
using wisp::core::SceneRepository;
using wisp::core::SceneState;
using wisp::core::SceneStatus;
using wisp::core::SceneStore;
using wisp::core::SceneStoreDeps;
using wisp::core::SceneStoreLimits;
using wisp::core::SourceFile;
using wisp::core::StorageWorker;
using wisp::core::ZipWriter;

namespace {

std::vector<uint8_t> todo_zip(int version, int assets) {
    ZipWriter w;
    auto add = [&](const std::string& path, const std::string& body) {
        w.add(path,
              std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
    };
    add("scene.json", R"({"id":"todo","name":"待办","version":"1.0.)" + std::to_string(version) +
                          R"(","schema_version":1})");
    add("main.lua", "return ui.label{text='hi'}\n");
    // Many files make the swap (remove_tree of the old package) take a
    // while, as it does on flash.
    for (int i = 0; i < assets; ++i) add("assets/a" + std::to_string(i) + ".txt", "x");
    wisp::os::LargeBuffer zip = w.finish();
    return {zip.data(), zip.data() + zip.size()};
}

// Runs callbacks after a delay on one thread: SceneStoreDeps::schedule.
class Scheduler {
public:
    Scheduler() : thread_([this] { run(); }) {}
    ~Scheduler() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    void at(uint32_t ms, std::function<void()> cb) {
        std::lock_guard<std::mutex> lock(mu_);
        jobs_.emplace_back(std::chrono::steady_clock::now() + std::chrono::milliseconds(ms),
                           std::move(cb));
        cv_.notify_all();
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mu_);
        while (!stop_) {
            const auto now = std::chrono::steady_clock::now();
            std::vector<std::function<void()>> due;
            for (auto it = jobs_.begin(); it != jobs_.end();) {
                if (it->first <= now) {
                    due.push_back(std::move(it->second));
                    it = jobs_.erase(it);
                } else {
                    ++it;
                }
            }
            lock.unlock();
            for (auto& cb : due) cb();
            lock.lock();
            cv_.wait_for(lock, std::chrono::milliseconds(1));
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::pair<std::chrono::steady_clock::time_point, std::function<void()>>> jobs_;
    bool stop_ = false;
    std::thread thread_;
};

// A SceneHost whose running scene is a writer thread over a real store.
// exit() does what SceneRuntime's exit does: stop the scene, flush the
// store, wait for that flush, close the store.
class StoreHost final : public SceneHost {
public:
    StoreHost(wisp::core::StorageManager& storage, StorageWorker& worker, SceneRepository& repo)
        : storage_(storage), worker_(worker), repo_(repo) {}
    ~StoreHost() override { stop_scene(); }

    Result<void> open(const std::string& id, Value) override {
        log("open " + id);
        stop_scene();
        SceneStoreLimits limits;
        limits.coalesce_ms = 1;
        store_ = SceneStore::create(SceneStoreDeps{storage_,
                                                   [this](std::function<void()> job) {
                                                       return worker_.submit(std::move(job));
                                                   },
                                                   [this](uint32_t ms, std::function<void()> cb) {
                                                       scheduler_.at(ms, std::move(cb));
                                                   }},
                                    repo_.data_dir(id), limits);
        std::promise<Result<void>> loaded;
        if (!worker_.submit([&] { loaded.set_value(store_->load()); }))
            return Result<void>::err(ErrorCode::Busy, "worker");
        Result<void> r = loaded.get_future().get();
        if (!r) return r;
        running_ = true;
        stop_writer_ = false;
        writer_ = std::thread([this] {
            while (!stop_writer_) {
                const int64_t n = next_++;
                if (store_->insert("w", R"({"n":)" + std::to_string(n) + "}")) {
                    std::lock_guard<std::mutex> lock(mu_);
                    acked_.push_back(n);
                }
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        return Result<void>::ok();
    }
    Result<void> close(const std::string& id) override {
        log("close " + id);
        stop_scene();
        return Result<void>::ok();
    }
    Result<void> exit(const std::string& id) override {
        log("exit " + id);
        if (close_error != ErrorCode::Ok) {
            return Result<void>::err(close_error, "the scene task did not answer");
        }
        stop_scene();
        return Result<void>::ok();
    }
    Result<void> reload(const std::string& id) override {
        log("reload " + id);
        stop_scene();
        return open(id, {});
    }
    SceneStatus status(const std::string&) const override {
        if (forced_state) return SceneStatus{*forced_state, "main.lua:1: boom"};
        return SceneStatus{running_ ? SceneState::Running : SceneState::Stopped, {}};
    }
    Result<void> open_background(const std::string& id) override {
        log("background " + id);
        return Result<void>::ok();
    }
    Result<void> check_sources(std::span<const SourceFile>) override { return Result<void>::ok(); }
    void on_installed(const std::string& id) override {
        log("installed " + id);
        // SceneRuntime: a running scene is reloaded.
        if (running_) {
            stop_scene();
            open(id, {});
        }
    }

    // What exit() answers instead of closing (Ok: it closes).
    ErrorCode close_error = ErrorCode::Ok;
    // What status() reports instead of the writer's state.
    std::optional<SceneState> forced_state;

    std::vector<std::string> calls() {
        std::lock_guard<std::mutex> lock(mu_);
        return calls_;
    }
    std::vector<int64_t> acked() {
        std::lock_guard<std::mutex> lock(mu_);
        return acked_;
    }
    void stop_scene() {
        if (!store_) return;
        stop_writer_ = true;
        if (writer_.joinable()) writer_.join();
        std::promise<void> flushed;
        store_->flush([&](Result<void>) { flushed.set_value(); });
        flushed.get_future().wait();
        store_->close();
        store_.reset();
        running_ = false;
    }

private:
    void log(std::string c) {
        std::lock_guard<std::mutex> lock(mu_);
        calls_.push_back(std::move(c));
    }

    wisp::core::StorageManager& storage_;
    StorageWorker& worker_;
    SceneRepository& repo_;
    Scheduler scheduler_;
    std::shared_ptr<SceneStore> store_;
    std::thread writer_;
    std::atomic<bool> stop_writer_{false};
    std::atomic<bool> running_{false};
    std::atomic<int64_t> next_{1};
    std::mutex mu_;
    std::vector<std::string> calls_;
    std::vector<int64_t> acked_;
};

struct Fixture {
    Fixture() : in(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", in, false));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        EXPECT_TRUE(worker.start());
        SceneRepoConfig config;
        config.code_root = in + "/scenes";
        config.data_root = in + "/scenes";
        // Long enough for the running scene's store (flushing every
        // millisecond) to write while the package is being swapped.
        config.on_data_moved = [] { std::this_thread::sleep_for(std::chrono::milliseconds(20)); };
        repo = std::make_unique<SceneRepository>(*storage, config);
        host = std::make_unique<StoreHost>(*storage, worker, *repo);
    }
    ~Fixture() {
        host.reset();
        worker.stop();
    }

    // The "n" of every record the scene's data holds, read back from disk.
    std::set<int64_t> stored() {
        auto store = SceneStore::create(SceneStoreDeps{*storage,
                                                       [this](std::function<void()> job) {
                                                           return worker.submit(std::move(job));
                                                       },
                                                       [](uint32_t, std::function<void()>) {}},
                                        repo->data_dir("todo"));
        std::promise<Result<void>> loaded;
        EXPECT_TRUE(worker.submit([&] { loaded.set_value(store->load()); }));
        EXPECT_TRUE(loaded.get_future().get());
        std::set<int64_t> out;
        auto found = store->find("w", {});
        EXPECT_TRUE(found);
        if (found) {
            for (const auto& rec : found.value()) {
                auto v = wisp::from_json(std::string_view(rec.json.data(), rec.json.size()));
                if (v && v.value().find("n") != nullptr) out.insert(v.value().find("n")->as_int());
            }
        }
        store->close();
        return out;
    }

    std::string in;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
    StorageWorker worker;
    std::unique_ptr<SceneRepository> repo;
    std::unique_ptr<StoreHost> host;
};

}  // namespace

TEST(SceneInstall, ReinstallOverARunningSceneKeepsEveryWrite) {
    Fixture fx;
    ASSERT_TRUE(wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(0, 20)));
    ASSERT_TRUE(fx.host->open("todo", {}));
    for (int i = 1; i <= 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        auto r = wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(i, 20));
        ASSERT_TRUE(r) << "install " << i << ": " << r.error().message;
        EXPECT_EQ(r.value().manifest.version, "1.0." + std::to_string(i));
        EXPECT_EQ(fx.host->status("todo").state, SceneState::Running);
    }
    fx.host->stop_scene();

    auto listed = fx.repo->list();
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1u);
    EXPECT_EQ(listed.value()[0].manifest.version, "1.0.10");

    const std::vector<int64_t> acked = fx.host->acked();
    ASSERT_GT(acked.size(), 50u);
    const std::set<int64_t> stored = fx.stored();
    size_t lost = 0;
    for (int64_t n : acked) lost += stored.count(n) == 0 ? 1 : 0;
    EXPECT_EQ(lost, 0u) << "of " << acked.size() << " acknowledged writes";

    // Closed before each swap (the first install's too, though nothing ran
    // yet) and opened again after it.
    std::vector<std::string> calls = fx.host->calls();
    EXPECT_EQ(std::count(calls.begin(), calls.end(), "exit todo"), 11);
    EXPECT_EQ(std::count(calls.begin(), calls.end(), "installed todo"), 1);  // the first one
}

TEST(SceneInstall, ReinstallOfAStoppedSceneDoesNotOpenIt) {
    Fixture fx;
    ASSERT_TRUE(wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(0, 2)));
    auto r = wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(1, 2));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(fx.host->calls(), (std::vector<std::string>{"exit todo", "installed todo",
                                                          "exit todo", "installed todo"}));
    EXPECT_EQ(fx.host->status("todo").state, SceneState::Stopped);
}

// A close that times out stops the install before anything is replaced:
// the old package stays, nothing is reopened, and the error says why.
TEST(SceneInstall, CloseTimeoutLeavesTheOldPackage) {
    Fixture fx;
    ASSERT_TRUE(wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(0, 2)));
    ASSERT_TRUE(fx.host->open("todo", {}));
    fx.host->close_error = ErrorCode::Timeout;
    auto r = wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(1, 2));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Timeout);
    EXPECT_NE(r.error().message.find("did not close in time"), std::string::npos)
        << r.error().message;
    // The close is still queued and happens later.
    EXPECT_NE(r.error().message.find("will still close"), std::string::npos) << r.error().message;
    EXPECT_NE(r.error().message.find("todo"), std::string::npos) << r.error().message;
    EXPECT_FALSE(fx.repo->installing("todo"));
    auto info = fx.repo->info("todo");
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().manifest.version, "1.0.0");
    EXPECT_EQ(fx.host->calls(),
              (std::vector<std::string>{"exit todo", "installed todo", "open todo", "exit todo"}));
    fx.host->close_error = ErrorCode::Ok;
}

// A close refused outright (the runtime is not running, its queue is full)
// is not a timeout and will not happen later.
TEST(SceneInstall, RefusedCloseIsNotCalledATimeout) {
    Fixture fx;
    ASSERT_TRUE(wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(0, 2)));
    ASSERT_TRUE(fx.host->open("todo", {}));
    fx.host->close_error = ErrorCode::Busy;
    auto r = wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(1, 2));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Busy);
    EXPECT_NE(r.error().message.find("could not be closed"), std::string::npos)
        << r.error().message;
    EXPECT_EQ(r.error().message.find("in time"), std::string::npos) << r.error().message;
    EXPECT_NE(r.error().message.find("left unchanged"), std::string::npos) << r.error().message;
    fx.host->close_error = ErrorCode::Ok;
}

// A scene in the error state may still have its last flush queued: it is
// closed (which waits for that flush) before the swap, like a running one,
// but not opened again -- it was not running.
TEST(SceneInstall, AnErroredSceneIsClosedFirstAndNotReopened) {
    Fixture fx;
    ASSERT_TRUE(wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(0, 2)));
    fx.host->forced_state = SceneState::Error;
    auto r = wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(1, 2));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(fx.host->calls(), (std::vector<std::string>{"exit todo", "installed todo",
                                                          "exit todo", "installed todo"}));
}

// scene-runtime-core spec 6.3 for a scene in the background (scene-device
// spec 3.2): closed for the swap, then started in the background again.
TEST(SceneInstall, ReinstallOfABackgroundSceneStartsItThereAgain) {
    Fixture fx;
    ASSERT_TRUE(wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(0, 2)));
    fx.host->forced_state = SceneState::Background;
    auto r = wisp::core::install_scene(*fx.repo, *fx.host, todo_zip(1, 2));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(fx.host->calls(), (std::vector<std::string>{"exit todo", "installed todo",
                                                          "exit todo", "background todo"}));
}
