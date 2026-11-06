#pragma once

// AgentHarness: everything an agent::AgentService needs, wired for tests.
// A temporary directory behind a StorageManager, a real StorageWorker
// (wisp.fs), the scripted FakeLlmServer behind a real LlmClient (retry
// waits skipped), a ModelProfileService with one llm profile (or none),
// an EventBus, a WakeLockRegistry, a NotificationService that saves
// nothing, an empty ToolRegistry whose ui/fs/io executors are plain
// worker threads, and a clock that runs with os::now_ms() but can be
// moved ahead.
//
// Tests add tools to `tools` before they start the service.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "support/fake_devices.hpp"
#include "support/fake_llm.hpp"
#include "support/temp_dir.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/conversation.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/net/llm_client.hpp"
#include "wisp/os/os.hpp"

namespace wisp::test {

// A thread draining a queue of jobs: the stand-in for the UI thread and
// the one-shot io tasks.
class JobThread {
public:
    JobThread() : thread_([this] { loop(); }) {}
    ~JobThread() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
    JobThread(const JobThread&) = delete;
    JobThread& operator=(const JobThread&) = delete;

    bool post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            q_.push_back(std::move(fn));
        }
        cv_.notify_all();
        return true;
    }

private:
    void loop() {
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
                if (q_.empty()) return;
                fn = std::move(q_.front());
                q_.pop_front();
            }
            fn();
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
    std::thread thread_;
};

struct AgentHarness {
    explicit AgentHarness(bool with_model = true) : root(temp_dir().string()) {
        registry.add(std::make_shared<FakeStorage>("storage.int", root, /*removable=*/false));
        bus.start();
        storage = std::make_unique<core::StorageManager>(registry, bus);
        storage->init();
        worker.start();
        models = std::make_unique<core::ModelProfileService>(*storage, bus);
        models->init();
        if (with_model) {
            models->create({.kind = core::ModelKind::Llm,
                            .name = "main",
                            .preset = "custom",
                            .base_url = "https://llm.example.com/v1",
                            .model = "m",
                            .api_key = "sk-test"});
        }
        llm = std::make_unique<net::LlmClient>(server, *models, [](uint32_t) {});
        locks = std::make_unique<core::WakeLockRegistry>(bus, [this] { return now(); });
        notes = std::make_unique<core::NotificationService>(
            core::NotificationDeps{.bus = bus, .storage = *storage});
        dir = storage->write_root(core::Area::System).value() + "/agent";
        store = std::make_unique<agent::ConversationStore>(*storage, dir);
        tools = std::make_unique<agent::ToolRegistry>(agent::ToolExecutors{
            .ui = [this](std::function<void()> fn) { return ui_thread.post(std::move(fn)); },
            .fs = [this](std::function<void()> fn) { return worker.submit(std::move(fn)); },
            .io = [this](std::function<void()> fn) { return io_thread.post(std::move(fn)); },
        });
    }

    ~AgentHarness() {
        worker.stop();
        notes.reset();
        bus.stop();
    }

    AgentHarness(const AgentHarness&) = delete;
    AgentHarness& operator=(const AgentHarness&) = delete;

    agent::AgentDeps deps() {
        return agent::AgentDeps{
            .bus = bus,
            .store = *store,
            .tools = *tools,
            .llm = *llm,
            .models = *models,
            .submit_io = [this](std::function<void()> fn) { return worker.submit(std::move(fn)); },
            .locks = locks.get(),
            .notes = notes.get(),
            .screen_off = [this] { return screen_off.load(); },
            .network_up = [this] { return network_up.load(); },
            .lang =
                [this] {
                    std::lock_guard<std::mutex> lock(lang_mu);
                    return lang;
                },
            .now_ms = [this] { return now(); },
            .wall_s = [] { return int64_t{1793923200}; },
        };
    }

    uint64_t now() const { return os::now_ms() + offset.load(); }
    void advance(uint64_t ms) { offset += ms; }

    void set_lang(std::string l) {
        std::lock_guard<std::mutex> lock(lang_mu);
        lang = std::move(l);
    }

    std::string root;
    core::DeviceRegistry registry;
    core::EventBus bus;
    std::unique_ptr<core::StorageManager> storage;
    core::StorageWorker worker;
    FakeLlmServer server;
    std::unique_ptr<core::ModelProfileService> models;
    std::unique_ptr<net::LlmClient> llm;
    std::unique_ptr<core::WakeLockRegistry> locks;
    std::unique_ptr<core::NotificationService> notes;
    std::string dir;
    std::unique_ptr<agent::ConversationStore> store;
    JobThread ui_thread, io_thread;
    std::unique_ptr<agent::ToolRegistry> tools;

    std::atomic<bool> screen_off{false};
    std::atomic<bool> network_up{true};
    std::atomic<uint64_t> offset{0};
    std::mutex lang_mu;
    std::string lang = "zh";
};

}  // namespace wisp::test
