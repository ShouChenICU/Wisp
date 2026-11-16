// ToolRegistry (agent-core spec 4): definitions, argument checks and running
// a tool on its thread.

#include "wisp/agent/tool_registry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "wisp/agent/io_runner.hpp"
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

TEST(ToolRegistry, SchemaSupportedRejectsUnknownKeywords) {
    auto schema = wisp::from_json(R"({"type":"object","oneOf":[]})");
    ASSERT_TRUE(schema);
    std::string why;
    EXPECT_FALSE(wisp::agent::schema_supported(schema.value(), &why));
    EXPECT_NE(why.find("oneOf"), std::string::npos);
}

TEST(ToolRegistry, TypeListAcceptsAnyOfItsTypes) {
    ToolRegistry reg(ToolExecutors{});
    ASSERT_TRUE(reg.add(tool("settings.set", R"({"type":"object",
        "properties":{"value":{"type":["integer","string"]}},"required":["value"]})")));
    ToolCtx c = ctx();
    EXPECT_TRUE(reg.call("settings_set", R"({"value":3})", c).ok);
    EXPECT_TRUE(reg.call("settings_set", R"({"value":"en"})", c).ok);
    ToolOutput out = reg.call("settings_set", R"({"value":true})", c);
    EXPECT_EQ(error_of(out), "bad_args");
    EXPECT_EQ(field_of(out), "value");

    // A property that may be cleared: ["string", "null"].
    ASSERT_TRUE(reg.add(tool("todo.update", R"({"type":"object",
        "properties":{"due":{"type":["string","null"]}}})")));
    EXPECT_TRUE(reg.call("todo_update", R"({"due":null})", c).ok);
    EXPECT_TRUE(reg.call("todo_update", R"({"due":"x"})", c).ok);
    ToolOutput not_null = reg.call("todo_update", R"({"due":5})", c);
    EXPECT_EQ(error_of(not_null), "bad_args");
    EXPECT_EQ(field_of(not_null), "due");

    std::string why;
    EXPECT_FALSE(wisp::agent::schema_supported(
        wisp::from_json(R"({"type":"object","properties":{"v":{"type":[]}}})").value(), &why));
    EXPECT_FALSE(wisp::agent::schema_supported(
        wisp::from_json(R"({"type":"object","properties":{"v":{"type":["integer","date"]}}})")
            .value(),
        &why));
    EXPECT_FALSE(wisp::agent::schema_supported(
        wisp::from_json(R"({"type":["object","string"]})").value(), &why));
}

namespace {

// A tool that records the thread it ran on.
ToolDef thread_probe(std::string name, ToolThread thread, std::thread::id* ran_on) {
    ToolDef d = tool(std::move(name), "", thread);
    d.run = [ran_on](const Value&, ToolCtx&) {
        *ran_on = std::this_thread::get_id();
        return ToolOutput{};
    };
    return d;
}

}  // namespace

TEST(ToolRegistry, FsInteractiveToolsUseTheirExecutor) {
    Worker fs;
    Worker interactive;
    ToolExecutors ex;
    ex.fs = [&fs](std::function<void()> fn) { return fs.post(std::move(fn)); };
    ex.fs_interactive = [&interactive](std::function<void()> fn) {
        return interactive.post(std::move(fn));
    };
    ToolRegistry reg(ex);
    std::thread::id note_on;
    std::thread::id files_on;
    ASSERT_TRUE(reg.add(thread_probe("memory.add", ToolThread::FsInteractive, &note_on)));
    ASSERT_TRUE(reg.add(thread_probe("scene.files", ToolThread::Fs, &files_on)));
    ToolCtx c = ctx();
    EXPECT_TRUE(reg.call("memory_add", "", c).ok);
    EXPECT_TRUE(reg.call("scene_files", "", c).ok);
    EXPECT_EQ(note_on, interactive.id());
    EXPECT_EQ(files_on, fs.id());
}

TEST(ToolRegistry, FsInteractiveFallsBackToFs) {
    Worker fs;
    ToolExecutors ex;
    ex.fs = [&fs](std::function<void()> fn) { return fs.post(std::move(fn)); };
    ToolRegistry reg(ex);
    std::thread::id note_on;
    ASSERT_TRUE(reg.add(thread_probe("memory.add", ToolThread::FsInteractive, &note_on)));
    ToolCtx c = ctx();
    EXPECT_TRUE(reg.call("memory_add", "", c).ok);
    EXPECT_EQ(note_on, fs.id());
}

namespace {

ToolDef vision_tool(std::string name, std::atomic<int>* runs = nullptr) {
    ToolDef d = tool(std::move(name));
    d.vision_only = true;
    if (runs != nullptr) {
        d.run = [runs](const Value&, ToolCtx&) {
            runs->fetch_add(1);
            ToolOutput out;
            out.result = Value(Map{{"ok", true}});
            return out;
        };
    }
    return d;
}

std::string str(const wisp::os::LargeString& s) { return std::string(s.data(), s.size()); }

std::vector<std::string> names_in(const wisp::os::LargeString& json) {
    std::vector<std::string> names;
    auto parsed = wisp::from_json(str(json));
    if (!parsed) return names;
    for (const Value& t : parsed.value().as_list()) {
        names.push_back(t.find("function")->find("name")->as_string());
    }
    return names;
}

}  // namespace

TEST(ToolRegistry, WithoutVisionTheArrayIsTodays) {
    ToolRegistry reg(ToolExecutors{});
    ASSERT_TRUE(reg.add(tool("a.b")));
    ASSERT_TRUE(reg.add(vision_tool("v.shot")));
    ASSERT_TRUE(reg.add(tool("c.d")));
    EXPECT_EQ(&reg.tools_json(false), &reg.tools_json());
    EXPECT_EQ(names_in(reg.tools_json(false)), (std::vector<std::string>{"a_b", "c_d"}));
    EXPECT_EQ(str(reg.tools_json(false)).find("v_shot"), std::string::npos);

    ToolRegistry plain(ToolExecutors{});
    ASSERT_TRUE(plain.add(tool("a.b")));
    ASSERT_TRUE(plain.add(tool("c.d")));
    EXPECT_EQ(str(reg.tools_json(false)), str(plain.tools_json()));
}

TEST(ToolRegistry, VisionOnlyToolsGoLast) {
    ToolRegistry reg(ToolExecutors{});
    ASSERT_TRUE(reg.add(tool("a.b")));
    ASSERT_TRUE(reg.add(vision_tool("v.shot")));
    ASSERT_TRUE(reg.add(tool("c.d")));
    const wisp::os::LargeString& vision = reg.tools_json(true);
    EXPECT_EQ(names_in(vision), (std::vector<std::string>{"a_b", "c_d", "v_shot"})) << str(vision);
    EXPECT_EQ(reg.tools_json(true).data(), vision.data());
    EXPECT_NE(vision.data(), reg.tools_json(false).data());

    // Only vision-only tools: the base array is empty and the vision one has no stray comma.
    ToolRegistry only(ToolExecutors{});
    ASSERT_TRUE(only.add(vision_tool("v.shot")));
    EXPECT_EQ(str(only.tools_json(false)), "[]");
    EXPECT_EQ(names_in(only.tools_json(true)), (std::vector<std::string>{"v_shot"}))
        << str(only.tools_json(true));
}

TEST(ToolRegistry, NoVisionOnlyToolMeansOneArray) {
    ToolRegistry reg(ToolExecutors{});
    ASSERT_TRUE(reg.add(tool("a.b")));
    ASSERT_TRUE(reg.add(tool("c.d")));
    EXPECT_EQ(reg.tools_json(true).data(), reg.tools_json(false).data());
}

TEST(ToolRegistry, VisionOnlyToolIsUnknownWithoutVision) {
    ToolRegistry reg(ToolExecutors{});
    std::atomic<int> runs{0};
    ASSERT_TRUE(reg.add(vision_tool("v.shot", &runs)));
    ToolCtx c = ctx();
    const ToolOutput refused = reg.call("v_shot", "", c);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(error_of(refused), "unknown_tool");
    EXPECT_EQ(runs.load(), 0);

    c.vision = true;
    EXPECT_TRUE(reg.call("v_shot", "", c).ok);
    EXPECT_EQ(runs.load(), 1);
}

// Without vision the model must not be able to tell a vision-only tool from a
// name that does not exist: the answer is the same, byte for byte.
TEST(ToolRegistry, VisionOnlyToolWithoutVisionAnswersLikeAnUnknownName) {
    ToolRegistry with(ToolExecutors{});
    ASSERT_TRUE(with.add(tool("a.b")));
    ASSERT_TRUE(with.add(vision_tool("v.shot")));
    ToolRegistry without(ToolExecutors{});
    ASSERT_TRUE(without.add(tool("a.b")));
    ToolCtx c = ctx();
    const ToolOutput hidden = with.call("v_shot", R"({"x":1})", c);
    const ToolOutput unknown = without.call("v_shot", R"({"x":1})", c);
    EXPECT_EQ(hidden.ok, unknown.ok);
    EXPECT_EQ(wisp::to_json(hidden.result), wisp::to_json(unknown.result));
    EXPECT_EQ(str(hidden.result_json), str(unknown.result_json));
    EXPECT_EQ(hidden.note, unknown.note);
    EXPECT_EQ(hidden.image, nullptr);
    EXPECT_EQ(error_of(unknown), "unknown_tool");
}

TEST(ToolRegistry, ImageSurvivesTheThreadHop) {
    Worker ui;
    ToolRegistry reg(executors_on(ui));
    const wisp::agent::ToolImage* made = nullptr;
    ToolDef d = tool("ui.shot", "", ToolThread::Ui);
    d.run = [&made](const Value&, ToolCtx&) {
        auto img = wisp::agent::make_tool_image("image/jpeg", 4);
        if (img) {
            const uint8_t bytes[] = {1, 2, 3, 4};
            std::copy(bytes, bytes + 4, img->bytes.data());
            made = img.get();
        }
        ToolOutput out;
        out.result = Value(Map{{"ok", true}});
        out.image = std::move(img);
        return out;
    };
    ASSERT_TRUE(reg.add(d));
    ToolCtx c = ctx();
    const ToolOutput out = reg.call("ui_shot", "", c);
    ASSERT_TRUE(out.ok);
    ASSERT_NE(out.image, nullptr);
    EXPECT_EQ(out.image.get(), made);
    EXPECT_STREQ(out.image->mime, "image/jpeg");
    ASSERT_EQ(out.image->bytes.size(), 4u);
    EXPECT_EQ(out.image->bytes.data()[0], 1);
    EXPECT_EQ(out.image->bytes.data()[3], 4);
    EXPECT_EQ(out.image.use_count(), 1);
}

// The image slot frees an older image by dropping its pointer (spec 3.5); that
// only frees the bytes if the registry's call state keeps no copy of it.
TEST(ToolRegistry, IoCallKeepsNoImageAfterDelivery) {
    wisp::agent::IoRunner io;
    ToolExecutors ex;
    ex.io = [&io](std::function<void()> fn) { return io.post(std::move(fn)); };
    ToolRegistry reg(std::move(ex));
    ToolDef d = tool("ui.shot", "", ToolThread::Io);
    d.run = [](const Value&, ToolCtx&) {
        ToolOutput out;
        out.result = Value(Map{{"ok", true}});
        out.image = wisp::agent::make_tool_image("image/jpeg", 4);
        return out;
    };
    ASSERT_TRUE(reg.add(d));
    ToolCtx c = ctx();
    ToolOutput out = reg.call("ui_shot", "", c);
    ASSERT_TRUE(out.ok);
    ASSERT_NE(out.image, nullptr);
    EXPECT_EQ(out.image.use_count(), 1);
    std::weak_ptr<const wisp::agent::ToolImage> weak = out.image;
    io.join_all();
    out.image.reset();
    EXPECT_TRUE(weak.expired());
}

// A late Io tool's image is never delivered; its bytes go when the tool returns.
TEST(ToolRegistry, AbandonedIoCallFreesItsImage) {
    wisp::agent::IoRunner io;
    ToolExecutors ex;
    ex.io = [&io](std::function<void()> fn) { return io.post(std::move(fn)); };
    ToolRegistry reg(std::move(ex));
    auto weak = std::make_shared<std::weak_ptr<const wisp::agent::ToolImage>>();
    std::atomic<bool> made{false};
    ToolDef d = tool("ui.shot", "", ToolThread::Io);
    d.timeout_ms = 30;
    d.run = [weak, &made](const Value&, ToolCtx&) {
        wisp::os::sleep_ms(100);
        ToolOutput out;
        out.image = wisp::agent::make_tool_image("image/jpeg", 4);
        *weak = out.image;
        made = true;
        return out;
    };
    ASSERT_TRUE(reg.add(d));
    ToolCtx c = ctx();
    const ToolOutput out = reg.call("ui_shot", "", c);
    EXPECT_EQ(error_of(out), "timeout");
    io.join_all();
    EXPECT_TRUE(made.load());
    EXPECT_TRUE(weak->expired());
}

TEST(ToolRegistry, ImageJsonShape) {
    auto img = wisp::agent::make_tool_image("image/png", 4);
    ASSERT_NE(img, nullptr);
    img->width = 2;
    img->height = 3;
    wisp::os::LargeString out;
    wisp::agent::append_image_json(out, *img);
    EXPECT_EQ(str(out), R"({"mime":"image/png","bytes":4,"width":2,"height":3})");

    img->width = 0;
    out.clear();
    wisp::agent::append_image_json(out, *img);
    EXPECT_EQ(str(out), R"({"mime":"image/png","bytes":4})");
}
