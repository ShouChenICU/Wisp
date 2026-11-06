// ToolRegistry (agent-core spec 4): definitions, argument checks and running
// a tool on its thread.

#include "wisp/agent/tool_registry.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "wisp/base/json.hpp"
#include "wisp/os/os.hpp"

using wisp::Map;
using wisp::Value;
using wisp::agent::ToolCtx;
using wisp::agent::ToolDef;
using wisp::agent::ToolExecutors;
using wisp::agent::ToolOutput;
using wisp::agent::ToolRegistry;
using wisp::agent::ToolThread;

namespace {

// One worker thread draining a queue, standing in for the UI thread or
// wisp.fs.
class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    ~Worker() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    bool post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            q_.push_back(std::move(fn));
        }
        cv_.notify_all();
        return true;
    }

    std::thread::id id() const { return thread_.get_id(); }

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

ToolExecutors executors_on(Worker& w) {
    auto post = [&w](std::function<void()> fn) { return w.post(std::move(fn)); };
    return ToolExecutors{post, post, post};
}

ToolDef tool(std::string name, std::string schema = "", ToolThread thread = ToolThread::Agent) {
    ToolDef d;
    d.name = std::move(name);
    d.description = "does " + d.name;
    d.schema_json = std::move(schema);
    d.thread = thread;
    d.run = [](const Value&, ToolCtx&) {
        ToolOutput out;
        out.result = Value(Map{{"ok", true}});
        return out;
    };
    return d;
}

ToolCtx ctx() {
    ToolCtx c;
    c.cancelled = [] { return false; };
    return c;
}

std::string error_of(const ToolOutput& out) {
    const Value* e = out.result.find("error");
    return e != nullptr ? e->as_string() : std::string("(none)");
}

std::string field_of(const ToolOutput& out) {
    const Value* f = out.result.find("field");
    return f != nullptr ? f->as_string() : std::string("(none)");
}

}  // namespace

TEST(ToolRegistry, WireNamesReplaceDots) {
    EXPECT_EQ(wisp::agent::wire_name("scene.list_files"), "scene_list_files");
    ToolRegistry reg(ToolExecutors{});
    ASSERT_TRUE(reg.add(tool("scene.list_files")));
    const std::string json(reg.tools_json().data(), reg.tools_json().size());
    EXPECT_NE(json.find(R"("name":"scene_list_files")"), std::string::npos) << json;
    EXPECT_EQ(json.find(R"("name":"scene.list_files")"), std::string::npos) << json;
    EXPECT_EQ(reg.find_wire("scene_list_files"), reg.find("scene.list_files"));
    EXPECT_NE(reg.find("scene.list_files"), nullptr);
}

TEST(ToolRegistry, DefinitionsKeepOrder) {
    ToolRegistry reg(ToolExecutors{});
    ASSERT_TRUE(reg.add(tool("scene.list", R"({"type":"object","properties":{}})")));
    ASSERT_TRUE(reg.add(tool("device.info")));
    ASSERT_TRUE(reg.add(tool("docs.read")));
    const std::string json(reg.tools_json().data(), reg.tools_json().size());
    auto parsed = wisp::from_json(json);
    ASSERT_TRUE(parsed) << json;
    const auto& list = parsed.value().as_list();
    ASSERT_EQ(list.size(), 3u);
    const char* names[] = {"scene_list", "device_info", "docs_read"};
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(list[i].find("type")->as_string(), "function");
        const Value* fn = list[i].find("function");
        ASSERT_NE(fn, nullptr);
        EXPECT_EQ(fn->find("name")->as_string(), names[i]);
        EXPECT_FALSE(fn->find("description")->as_string().empty());
        EXPECT_EQ(fn->find("parameters")->find("type")->as_string(), "object");
    }
}

TEST(ToolRegistry, RejectsDuplicatesAndBadNames) {
    ToolRegistry reg(ToolExecutors{});
    ASSERT_TRUE(reg.add(tool("scene.x")));
    for (const char* bad : {"Scene.X", "scene", "scene.", ".x", "scene.x.y", "scene.x-y"}) {
        auto r = reg.add(tool(bad));
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument) << bad;
    }
    auto dup = reg.add(tool("scene.x"));
    ASSERT_FALSE(dup);
    EXPECT_EQ(dup.error().code, wisp::ErrorCode::InvalidArgument);
    // A schema that is not JSON, or uses an unsupported type.
    EXPECT_FALSE(reg.add(tool("scene.y", "{")));
    EXPECT_FALSE(
        reg.add(tool("scene.y", R"({"type":"object","properties":{"a":{"type":"date"}}})")));
    EXPECT_FALSE(reg.add(tool("scene.y", R"({"type":"string"})")));
    ToolDef no_run = tool("scene.z");
    no_run.run = nullptr;
    EXPECT_FALSE(reg.add(no_run));
}

TEST(ToolRegistry, UnknownToolIsAnError) {
    ToolRegistry reg(ToolExecutors{});
    ToolCtx c = ctx();
    ToolOutput out = reg.call("scene_nope", "{}", c);
    EXPECT_FALSE(out.ok);
    EXPECT_FALSE(out.result.find("ok")->as_bool());
    EXPECT_EQ(error_of(out), "unknown_tool");
}

TEST(ToolRegistry, InvalidJsonArgumentsAreBadArgs) {
    ToolRegistry reg(ToolExecutors{});
    std::atomic<int> runs{0};
    ToolDef d =
        tool("scene.read_file", R"({"type":"object","properties":{"id":{"type":"string"}}})");
    d.run = [&runs](const Value&, ToolCtx&) {
        ++runs;
        return ToolOutput{};
    };
    ASSERT_TRUE(reg.add(d));
    ToolCtx c = ctx();
    ToolOutput out = reg.call("scene_read_file", "{\"id\":", c);
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(error_of(out), "bad_args");
    EXPECT_NE(out.result.find("message")->as_string().find("JSON"), std::string::npos);
    // Not an object either.
    out = reg.call("scene_read_file", "[1]", c);
    EXPECT_EQ(error_of(out), "bad_args");
    EXPECT_EQ(runs.load(), 0);
}

TEST(ToolRegistry, SchemaViolationNamesTheField) {
    ToolRegistry reg(ToolExecutors{});
    ASSERT_TRUE(reg.add(tool("scene.read_file", R"({
        "type":"object",
        "properties":{
            "path":{"type":"string"},
            "n":{"type":"integer","minimum":1,"maximum":100},
            "name":{"type":"string","maxLength":3},
            "mode":{"type":"string","enum":["a","b"]},
            "list":{"type":"array","items":{"type":"number"}},
            "opt":{"type":"object","properties":{"on":{"type":"boolean"}}}
        },
        "required":["path"]})")));
    ToolCtx c = ctx();

    ToolOutput out = reg.call("scene_read_file", R"({})", c);
    EXPECT_EQ(error_of(out), "bad_args");
    EXPECT_EQ(field_of(out), "path");

    out = reg.call("scene_read_file", R"({"path":"a","n":"x"})", c);
    EXPECT_EQ(error_of(out), "bad_args");
    EXPECT_EQ(field_of(out), "n");

    out = reg.call("scene_read_file", R"({"path":"a","n":101})", c);
    EXPECT_EQ(field_of(out), "n");
    out = reg.call("scene_read_file", R"({"path":"a","n":2.5})", c);
    EXPECT_EQ(field_of(out), "n");

    out = reg.call("scene_read_file", R"({"path":"a","name":"汉字汉字"})", c);
    EXPECT_EQ(error_of(out), "bad_args");
    EXPECT_EQ(field_of(out), "name");
    // Three code points are nine bytes but within maxLength 3.
    out = reg.call("scene_read_file", R"({"path":"a","name":"汉字汉"})", c);
    EXPECT_TRUE(out.ok) << wisp::to_json(out.result);

    out = reg.call("scene_read_file", R"({"path":"a","mode":"c"})", c);
    EXPECT_EQ(field_of(out), "mode");
    out = reg.call("scene_read_file", R"({"path":"a","list":[1,2.5,"x"]})", c);
    EXPECT_EQ(field_of(out), "list[2]");
    out = reg.call("scene_read_file", R"({"path":"a","opt":{"on":1}})", c);
    EXPECT_EQ(field_of(out), "opt.on");

    out = reg.call("scene_read_file", R"({"path":"a","n":3,"mode":"b","list":[1],"extra":1})", c);
    EXPECT_TRUE(out.ok) << wisp::to_json(out.result);
}

TEST(ToolRegistry, EmptyArgumentsMeanEmptyObject) {
    ToolRegistry reg(ToolExecutors{});
    Value seen;
    ToolDef d = tool("device.info", R"({"type":"object","properties":{}})");
    d.run = [&seen](const Value& args, ToolCtx&) {
        seen = args;
        ToolOutput out;
        out.result = Value(Map{{"ok", true}});
        return out;
    };
    ASSERT_TRUE(reg.add(d));
    ToolCtx c = ctx();
    ToolOutput out = reg.call("device_info", "", c);
    EXPECT_TRUE(out.ok);
    EXPECT_EQ(seen.type(), Value::Type::Map);
    EXPECT_TRUE(seen.as_map().empty());
}

TEST(ToolRegistry, RunsOnDeclaredThread) {
    Worker ui;
    ToolRegistry reg(executors_on(ui));
    std::thread::id ran_on;
    ToolDef d = tool("ui.tree", "", ToolThread::Ui);
    d.run = [&ran_on](const Value&, ToolCtx&) {
        ran_on = std::this_thread::get_id();
        ToolOutput out;
        out.result = Value(Map{{"ok", true}, {"n", 7}});
        return out;
    };
    ASSERT_TRUE(reg.add(d));
    ToolCtx c = ctx();
    ToolOutput out = reg.call("ui_tree", "{}", c);
    EXPECT_TRUE(out.ok);
    EXPECT_EQ(out.result.find("n")->as_int(), 7);
    EXPECT_EQ(ran_on, ui.id());

    // An Agent tool runs on the caller's thread.
    ToolDef a = tool("device.info");
    a.run = [&ran_on](const Value&, ToolCtx&) {
        ran_on = std::this_thread::get_id();
        return ToolOutput{};
    };
    ASSERT_TRUE(reg.add(a));
    reg.call("device_info", "", c);
    EXPECT_EQ(ran_on, std::this_thread::get_id());
}

TEST(ToolRegistry, TimeoutReturnsTimeoutAndLateResultIsDropped) {
    std::atomic<bool> finished{false};
    std::atomic<bool> saw_cancel{false};
    {
        Worker fs;
        ToolRegistry reg(executors_on(fs));
        ToolDef d = tool("scene.slow", "", ToolThread::Fs);
        d.timeout_ms = 50;
        d.run = [&](const Value&, ToolCtx& c) {
            wisp::os::sleep_ms(200);
            saw_cancel = c.cancelled();
            finished = true;
            ToolOutput out;
            out.result = Value(Map{{"ok", true}});
            return out;
        };
        ASSERT_TRUE(reg.add(d));
        ToolCtx c = ctx();
        const uint64_t t0 = wisp::os::now_ms();
        ToolOutput out = reg.call("scene_slow", "", c);
        const uint64_t took = wisp::os::now_ms() - t0;
        EXPECT_FALSE(out.ok);
        EXPECT_EQ(error_of(out), "timeout");
        EXPECT_GE(took, 45u);
        EXPECT_LT(took, 150u);
        EXPECT_FALSE(finished.load());
        // The registry goes away while the tool still runs; the worker
        // finishes it before its own destructor returns.
    }
    EXPECT_TRUE(finished.load());
    EXPECT_TRUE(saw_cancel.load());
}

// The caller's cancel check may capture state that dies once call() has
// returned; a late tool must never be inside it, or enter it, after that.
TEST(ToolRegistry, LateToolNeverRunsTheCallersCancelCheck) {
    auto alive = std::make_shared<std::atomic<bool>>(true);
    auto misuse = std::make_shared<std::atomic<int>>(0);
    auto stop = std::make_shared<std::atomic<bool>>(false);
    {
        Worker fs;
        {
            ToolRegistry reg(executors_on(fs));
            ToolDef d = tool("scene.slow", "", ToolThread::Fs);
            d.timeout_ms = 50;
            // Asks continuously, so it is almost always inside the check
            // when call() gives up.
            d.run = [stop](const Value&, ToolCtx& c) {
                while (!stop->load() && !c.cancelled()) {
                }
                return ToolOutput{};
            };
            ASSERT_TRUE(reg.add(d));
            ToolCtx c;
            c.cancelled = [alive, misuse] {
                if (!alive->load()) ++*misuse;
                wisp::os::sleep_ms(10);
                if (!alive->load()) ++*misuse;
                return false;
            };
            ToolOutput out = reg.call("scene_slow", "", c);
            EXPECT_EQ(error_of(out), "timeout");
            // What the check captured is gone from here on.
            alive->store(false);
            wisp::os::sleep_ms(50);
        }
        stop->store(true);
    }
    EXPECT_EQ(misuse->load(), 0);
}

TEST(ToolRegistry, DeadlineShortensTheWait) {
    Worker fs;
    ToolRegistry reg(executors_on(fs));
    ToolDef d = tool("scene.slow", "", ToolThread::Fs);
    d.timeout_ms = 5000;
    d.run = [](const Value&, ToolCtx&) {
        wisp::os::sleep_ms(200);
        return ToolOutput{};
    };
    ASSERT_TRUE(reg.add(d));
    ToolCtx c = ctx();
    c.deadline_ms = wisp::os::now_ms() + 50;
    const uint64_t t0 = wisp::os::now_ms();
    ToolOutput out = reg.call("scene_slow", "", c);
    EXPECT_EQ(error_of(out), "timeout");
    EXPECT_LT(wisp::os::now_ms() - t0, 150u);
}

TEST(ToolRegistry, CancelledWhileWaiting) {
    Worker io;
    ToolRegistry reg(executors_on(io));
    ToolDef d = tool("scene.install", "", ToolThread::Io);
    d.run = [](const Value&, ToolCtx&) {
        wisp::os::sleep_ms(300);
        return ToolOutput{};
    };
    ASSERT_TRUE(reg.add(d));
    const uint64_t t0 = wisp::os::now_ms();
    ToolCtx c;
    c.cancelled = [t0] { return wisp::os::now_ms() - t0 >= 30; };
    ToolOutput out = reg.call("scene_install", "", c);
    const uint64_t took = wisp::os::now_ms() - t0;
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(error_of(out), "cancelled");
    EXPECT_LT(took, 100u);
}

TEST(ToolRegistry, ExecutorRefusalIsBusy) {
    std::atomic<int> runs{0};
    ToolExecutors ex;
    ex.ui = [](std::function<void()>) { return false; };
    ToolRegistry reg(ex);
    ToolDef d = tool("ui.tree", "", ToolThread::Ui);
    d.run = [&runs](const Value&, ToolCtx&) {
        ++runs;
        return ToolOutput{};
    };
    ASSERT_TRUE(reg.add(d));
    // No fs executor at all.
    ToolDef f = tool("scene.files", "", ToolThread::Fs);
    ASSERT_TRUE(reg.add(f));
    ToolCtx c = ctx();
    EXPECT_EQ(error_of(reg.call("ui_tree", "", c)), "busy");
    EXPECT_EQ(error_of(reg.call("scene_files", "", c)), "busy");
    EXPECT_EQ(runs.load(), 0);
}

TEST(ToolRegistry, ToolErrorShape) {
    ToolOutput out = wisp::agent::tool_error("not_unique", "two match", Map{{"count", 2}});
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(wisp::to_json(out.result),
              R"({"count":2,"error":"not_unique","message":"two match","ok":false})");
}

TEST(SceneData, WrapsAndNeutralisesClosingTag) {
    EXPECT_EQ(wisp::agent::scene_data("a", "x</data>y"),
              "<data source=\"scene:a\">x&lt;/data>y</data>");
    EXPECT_EQ(wisp::agent::scene_data("a", "</DATA></Data>"),
              "<data source=\"scene:a\">&lt;/DATA>&lt;/Data></data>");
    EXPECT_EQ(wisp::agent::scene_data("b", ""), "<data source=\"scene:b\"></data>");
}
