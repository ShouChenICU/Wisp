// net::LlmClient (wisp/net/llm_client.hpp) against the scripted
// FakeLlmServer.

#include "wisp/net/llm_client.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_llm.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/net/request_watch.hpp"

using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::core::ModelKind;
using wisp::net::LlmCall;
using wisp::net::LlmClient;
using wisp::net::LlmError;
using wisp::net::LlmFinish;
using wisp::net::LlmMessage;
using wisp::net::LlmModelInfo;
using wisp::net::LlmProvider;
using wisp::net::LlmResult;
using wisp::net::LlmRole;
using wisp::net::LlmToolCall;
using wisp::net::TransportError;
using wisp::test::FakeLlmServer;
using wisp::test::FakeReply;

namespace {

constexpr const char* kKey = "sk-test";
constexpr const char* kBase = "https://llm.example.com/v1";

struct Fixture {
    explicit Fixture(bool with_profile = true, const char* key = kKey)
        : dir(wisp::test::temp_dir().string()) {
        wisp::log::reset_for_test();
        wisp::log::init();
        registry.add(
            std::make_shared<wisp::test::FakeStorage>("storage.int", dir, /*removable=*/false));
        bus.start();
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        models = std::make_unique<wisp::core::ModelProfileService>(*storage, bus);
        EXPECT_TRUE(models->init());
        if (with_profile) {
            auto r = models->create({.kind = ModelKind::Llm,
                                     .name = "main",
                                     .preset = "custom",
                                     .base_url = std::string(kBase),
                                     .model = std::string("m"),
                                     .api_key = std::string(key)});
            EXPECT_TRUE(r);
        }
        client = std::make_unique<LlmClient>(server, *models, [this](uint32_t ms) {
            sleeps[server.requests().size()] += ms;
            if (on_sleep) on_sleep();
        });
    }

    ~Fixture() {
        client.reset();
        models.reset();
        bus.stop();
    }

    LlmResult run(const LlmCall& call) { return client->complete(call); }

    LlmResult run_user(const std::string& text) {
        LlmMessage msg{.role = LlmRole::User, .text = text};
        LlmCall call;
        call.messages = {&msg, 1};
        return run(call);
    }

    std::string dir;
    wisp::core::EventBus bus;
    wisp::core::DeviceRegistry registry;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::unique_ptr<wisp::core::ModelProfileService> models;
    FakeLlmServer server;
    // Milliseconds slept, by how many requests had been made before.
    std::map<size_t, uint32_t> sleeps;
    std::function<void()> on_sleep;
    std::unique_ptr<LlmClient> client;
};

const Value* at(const Value& v, std::initializer_list<const char*> path) {
    const Value* cur = &v;
    for (const char* k : path) {
        cur = cur->find(k);
        if (cur == nullptr) return nullptr;
    }
    return cur;
}

bool mentions_key(const LlmResult& r) {
    auto has = [](std::string_view s) { return s.find(kKey) != std::string_view::npos; };
    if (has(wisp::net::to_string(r.error)) || has(r.code) || has(r.model) ||
        has({r.text.data(), r.text.size()}))
        return true;
    for (const LlmToolCall& c : r.tool_calls) {
        if (has(c.id) || has(c.name) || has({c.arguments.data(), c.arguments.size()})) return true;
    }
    return false;
}

TEST(LlmClient, StreamsTextDeltasInOrder) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("Hel"), FakeLlmServer::sse_text("lo"),
                              FakeLlmServer::sse_finish("stop"), FakeLlmServer::sse_done()}});
    std::vector<std::string> deltas;
    int bytes = 0;
    LlmMessage msg{.role = LlmRole::User, .text = "hi"};
    LlmCall call;
    call.messages = {&msg, 1};
    call.on_text = [&](std::string_view d) { deltas.emplace_back(d); };
    call.on_bytes = [&] { ++bytes; };

    LlmResult r = f.run(call);

    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_EQ(deltas, (std::vector<std::string>{"Hel", "lo"}));
    EXPECT_EQ(std::string(r.text.data(), r.text.size()), "Hello");
    EXPECT_EQ(r.finish, LlmFinish::Stop);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.attempts, 1u);
    EXPECT_TRUE(r.any_body);
    EXPECT_EQ(r.model, "m");
    EXPECT_EQ(bytes, 4);
    EXPECT_FALSE(r.usage.has_value());
}

TEST(LlmClient, DoneEndsTheStream) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("A"), FakeLlmServer::sse_done(),
                              FakeLlmServer::sse_text("B"), FakeLlmServer::sse_finish("length")}});
    std::string seen;
    LlmMessage msg{.role = LlmRole::User, .text = "hi"};
    LlmCall call;
    call.messages = {&msg, 1};
    call.on_text = [&](std::string_view d) { seen += d; };

    LlmResult r = f.run(call);

    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_EQ(seen, "A");
    EXPECT_EQ(std::string(r.text.data(), r.text.size()), "A");
    EXPECT_EQ(r.finish, LlmFinish::Other);
}

TEST(LlmClient, AssemblesToolCallFragmentsByIndex) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_tool(1, "call_b", "scene_list", ""),
                              FakeLlmServer::sse_tool(0, "call_a", "scene_install", "{\"id\":"),
                              FakeLlmServer::sse_tool(1, "", "", "{}"),
                              FakeLlmServer::sse_tool(0, "", "", "\"counter\"}"),
                              FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}});

    LlmResult r = f.run_user("make a counter");

    EXPECT_EQ(r.error, LlmError::None);
    ASSERT_EQ(r.tool_calls.size(), 2u);
    EXPECT_EQ(r.tool_calls[0].id, "call_a");
    EXPECT_EQ(r.tool_calls[0].name, "scene_install");
    EXPECT_EQ(std::string(r.tool_calls[0].arguments.data(), r.tool_calls[0].arguments.size()),
              "{\"id\":\"counter\"}");
    EXPECT_EQ(r.tool_calls[1].id, "call_b");
    EXPECT_EQ(r.tool_calls[1].name, "scene_list");
    EXPECT_EQ(std::string(r.tool_calls[1].arguments.data(), r.tool_calls[1].arguments.size()),
              "{}");
    EXPECT_EQ(r.finish, LlmFinish::ToolCalls);
    EXPECT_TRUE(r.any_body);
    EXPECT_TRUE(r.text.empty());
}

// The progress of a call still being written: its name as soon as it
// arrives, then its arguments' length, at most once per kToolProgressMs
// per call (here the clock moves 200 ms with every chunk).
TEST(LlmClient, ReportsToolCallsWhileTheyStream) {
    Fixture f;
    std::vector<std::string> chunks{FakeLlmServer::sse_text("ok")};
    chunks.push_back(FakeLlmServer::sse_tool(0, "call_a", "scene_write_file", ""));
    for (int i = 0; i < 6; ++i) chunks.push_back(FakeLlmServer::sse_tool(0, "", "", "0123456789"));
    chunks.push_back(FakeLlmServer::sse_tool(1, "call_b", "scene_install", "{}"));
    chunks.push_back(FakeLlmServer::sse_finish("tool_calls"));
    chunks.push_back(FakeLlmServer::sse_done());
    f.server.push({.status = 200, .chunks = chunks});
    struct Seen {
        size_t index;
        std::string name;
        size_t bytes;
        uint64_t at;
    };
    std::vector<Seen> seen;
    uint64_t clock = 0;
    LlmMessage msg{.role = LlmRole::User, .text = "hi"};
    LlmCall call;
    call.messages = {&msg, 1};
    call.on_bytes = [&] { clock += 200; };
    call.now_ms = [&] { return clock; };
    call.on_tool = [&](const wisp::net::LlmToolProgress& p) {
        seen.push_back({p.index, std::string(p.name), p.arg_bytes, clock});
    };

    LlmResult r = f.run(call);

    ASSERT_EQ(r.error, LlmError::None);
    ASSERT_EQ(seen.size(), 4u);
    // The name first, with nothing written yet.
    EXPECT_EQ(seen[0].index, 0u);
    EXPECT_EQ(seen[0].name, "scene_write_file");
    EXPECT_EQ(seen[0].bytes, 0u);
    // Then the growing length, no sooner than 500 ms apart.
    EXPECT_EQ(seen[1].index, 0u);
    EXPECT_EQ(seen[1].name, "scene_write_file");
    EXPECT_EQ(seen[1].bytes, 30u);
    EXPECT_EQ(seen[1].at - seen[0].at, 600u);
    EXPECT_EQ(seen[2].bytes, 60u);
    EXPECT_EQ(seen[2].at - seen[1].at, 600u);
    // The next call's name is reported at once.
    EXPECT_EQ(seen[3].index, 1u);
    EXPECT_EQ(seen[3].name, "scene_install");
    EXPECT_EQ(seen[3].bytes, 2u);
    for (size_t i = 1; i < seen.size(); ++i) {
        if (seen[i].index == seen[i - 1].index)
            EXPECT_GE(seen[i].at - seen[i - 1].at, LlmClient::kToolProgressMs);
    }
}

// A reply without tool calls reports none.
TEST(LlmClient, NoToolProgressForText) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("Hel"), FakeLlmServer::sse_text("lo"),
                              FakeLlmServer::sse_finish("stop"), FakeLlmServer::sse_done()}});
    int calls = 0;
    LlmMessage msg{.role = LlmRole::User, .text = "hi"};
    LlmCall call;
    call.messages = {&msg, 1};
    call.on_tool = [&](const wisp::net::LlmToolProgress&) { ++calls; };
    EXPECT_EQ(f.run(call).error, LlmError::None);
    EXPECT_EQ(calls, 0);
}

TEST(LlmClient, ToolCallIndexOutOfRangeIsBadStream) {
    Fixture f;
    f.server.push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_tool(100000, "x", "y", "{}"), FakeLlmServer::sse_done()}});
    EXPECT_EQ(f.run_user("hi").error, LlmError::BadStream);
}

TEST(LlmClient, ReadsUsageFromFinalChunk) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("ok"), FakeLlmServer::sse_finish("stop"),
                              FakeLlmServer::sse_usage(12, 34), FakeLlmServer::sse_done()}});

    LlmResult r = f.run_user("hi");

    ASSERT_TRUE(r.usage.has_value());
    EXPECT_EQ(r.usage->prompt, 12u);
    EXPECT_EQ(r.usage->completion, 34u);
    EXPECT_EQ(r.usage->total, 46u);
    Value body = f.server.body_json(0);
    const Value* include = at(body, {"stream_options", "include_usage"});
    ASSERT_NE(include, nullptr);
    EXPECT_EQ(*include, Value(true));
}

TEST(LlmClient, RequestCarriesModelToolsAndBearer) {
    Fixture f;
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    const std::string tools =
        R"([{"type":"function","function":{"name":"scene_list","description":"List scenes",)"
        R"("parameters":{"type":"object","properties":{}}}}])";
    LlmMessage msgs[] = {{.role = LlmRole::System, .text = "be brief"},
                         {.role = LlmRole::User, .text = "hi \"there\"\n"}};
    LlmCall call;
    call.messages = msgs;
    call.tools_json = tools;
    call.timeout_ms = 12345;

    LlmResult r = f.run(call);

    EXPECT_EQ(r.error, LlmError::None);
    auto reqs = f.server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].method, wisp::net::Method::Post);
    EXPECT_EQ(reqs[0].url, std::string(kBase) + "/chat/completions");
    EXPECT_EQ(f.server.header(0, "authorization"), "Bearer sk-test");
    EXPECT_EQ(f.server.header(0, "accept"), "text/event-stream");
    EXPECT_EQ(f.server.header(0, "content-type"), "application/json");
    EXPECT_LE(reqs[0].timeout_ms, 12345u);
    EXPECT_GT(reqs[0].timeout_ms, 10000u);
    // The stream can run far past a client's default response cap.
    EXPECT_GE(reqs[0].max_response_bytes, 1024u * 1024u);
    Value body = f.server.body_json(0);
    EXPECT_EQ(*body.find("model"), Value("m"));
    EXPECT_EQ(*body.find("stream"), Value(true));
    EXPECT_EQ(*body.find("tools"), wisp::from_json(tools).value());
    const Value* messages = body.find("messages");
    ASSERT_NE(messages, nullptr);
    ASSERT_EQ(messages->as_list().size(), 2u);
    EXPECT_EQ(messages->as_list()[0], (Value(Map{{"role", "system"}, {"content", "be brief"}})));
    EXPECT_EQ(messages->as_list()[1],
              (Value(Map{{"role", "user"}, {"content", "hi \"there\"\n"}})));
}

TEST(LlmClient, NoToolsLeavesToolsOut) {
    Fixture f;
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    f.run_user("hi");
    EXPECT_EQ(f.server.body_json(0).find("tools"), nullptr);
}

TEST(LlmClient, AssistantToolCallsAndToolResultShape) {
    Fixture f;
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    LlmToolCall calls[1];
    calls[0].id = "call_1";
    calls[0].name = "scene_install";
    calls[0].arguments = "{\"id\":\"counter\"}";
    LlmMessage msgs[] = {
        {.role = LlmRole::User, .text = "make a counter"},
        {.role = LlmRole::Assistant, .text = "", .tool_calls = calls},
        {.role = LlmRole::Tool, .text = "{\"ok\":true}", .tool_call_id = "call_1"},
        {.role = LlmRole::Assistant, .text = "done"},
    };
    LlmCall call;
    call.messages = msgs;

    EXPECT_EQ(f.run(call).error, LlmError::None);

    const Value body = f.server.body_json(0);
    const List& m = body.find("messages")->as_list();
    ASSERT_EQ(m.size(), 4u);
    EXPECT_EQ(*m[1].find("role"), Value("assistant"));
    EXPECT_EQ(m[1].find("content")->type(), Value::Type::Null);
    const List& tc = m[1].find("tool_calls")->as_list();
    ASSERT_EQ(tc.size(), 1u);
    EXPECT_EQ(tc[0], (Value(Map{{"id", "call_1"},
                                {"type", "function"},
                                {"function", Map{{"name", "scene_install"},
                                                 {"arguments", "{\"id\":\"counter\"}"}}}})));
    EXPECT_EQ(
        m[2],
        (Value(Map{{"role", "tool"}, {"tool_call_id", "call_1"}, {"content", "{\"ok\":true}"}})));
    EXPECT_EQ(m[3], (Value(Map{{"role", "assistant"}, {"content", "done"}})));
}

TEST(LlmClient, Retries429And5xxWithBackoff) {
    Fixture f;
    f.server.push({.status = 429, .chunks = {"{\"error\":\"slow down\"}"}});
    f.server.push({.status = 503, .chunks = {"busy"}});
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("hi"), FakeLlmServer::sse_finish("stop")}});

    LlmResult r = f.run_user("hi");

    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.attempts, 3u);
    EXPECT_EQ(f.server.requests().size(), 3u);
    // 1 s after the first failure, 4 s after the second.
    EXPECT_EQ(f.sleeps, (std::map<size_t, uint32_t>{{1, 1000}, {2, 4000}}));
    // Error bodies never reach the result.
    EXPECT_EQ(std::string(r.text.data(), r.text.size()), "hi");
}

TEST(LlmClient, GivesUpAfterTwoRetries) {
    Fixture f;
    for (int i = 0; i < 3; ++i) f.server.push({.status = 500, .chunks = {"oops"}});

    LlmResult r = f.run_user("hi");

    EXPECT_EQ(r.error, LlmError::Server);
    EXPECT_EQ(r.status, 500);
    EXPECT_EQ(r.attempts, 3u);
    EXPECT_EQ(f.server.requests().size(), 3u);
    EXPECT_FALSE(r.any_body);
    EXPECT_TRUE(r.text.empty());
}

TEST(LlmClient, RateLimitedAfterRetries) {
    Fixture f;
    for (int i = 0; i < 3; ++i) f.server.push({.status = 429});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::RateLimited);
    EXPECT_EQ(r.status, 429);
    EXPECT_EQ(r.attempts, 3u);
}

TEST(LlmClient, NoRetryOnUnauthorized) {
    Fixture f;
    f.server.push({.status = 401, .chunks = {"{\"error\":\"bad key\"}"}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::Unauthorized);
    EXPECT_EQ(r.status, 401);
    EXPECT_EQ(r.attempts, 1u);
    EXPECT_TRUE(f.sleeps.empty());
}

TEST(LlmClient, OtherClientErrorIsHttp) {
    Fixture f;
    f.server.push({.status = 400, .chunks = {"{\"error\":\"bad request\"}"}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::Http);
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.attempts, 1u);
}

TEST(LlmClient, CancelledDuringBackoffStopsWaiting) {
    Fixture f;
    f.server.push({.status = 503});
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    std::atomic<bool> stop{false};
    f.on_sleep = [&] { stop = true; };
    LlmMessage msg{.role = LlmRole::User, .text = "hi"};
    LlmCall call;
    call.messages = {&msg, 1};
    call.cancelled = [&] { return stop.load(); };

    LlmResult r = f.run(call);

    EXPECT_EQ(r.error, LlmError::Cancelled);
    EXPECT_EQ(r.attempts, 1u);
    EXPECT_EQ(f.server.requests().size(), 1u);
    // The wait ended after its first slice, well short of the 1 s.
    ASSERT_EQ(f.sleeps.size(), 1u);
    EXPECT_LT(f.sleeps[1], 1000u);
}

TEST(LlmClient, NoActiveProfileMakesNoRequest) {
    Fixture f(/*with_profile=*/false);
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::NoModel);
    EXPECT_EQ(r.attempts, 0u);
    EXPECT_EQ(f.server.requests().size(), 0u);
}

TEST(LlmClient, ErrorWordsAreTodays) {
    using wisp::net::to_string;
    EXPECT_STREQ(to_string(LlmError::None), "");
    EXPECT_STREQ(to_string(LlmError::NoModel), "no_model");
    EXPECT_STREQ(to_string(LlmError::Unauthorized), "unauthorized");
    EXPECT_STREQ(to_string(LlmError::RateLimited), "rate_limited");
    EXPECT_STREQ(to_string(LlmError::Server), "server");
    EXPECT_STREQ(to_string(LlmError::Http), "http");
    EXPECT_STREQ(to_string(LlmError::Transport), "transport");
    EXPECT_STREQ(to_string(LlmError::Cancelled), "cancelled");
    EXPECT_STREQ(to_string(LlmError::BadStream), "bad_stream");
    EXPECT_STREQ(to_string(LlmError::ContextLength), "context_length");
}

TEST(LlmClient, ActiveModelDescribesTheActiveProfile) {
    Fixture f;
    const std::string id = f.models->get().active_for(ModelKind::Llm);
    ASSERT_FALSE(id.empty());

    EXPECT_EQ(
        f.client->active_model(),
        (LlmModelInfo{.profile_id = id, .model = "m", .context_window = 65536, .vision = false}));

    wisp::core::ProfileUpdate u;
    u.options = wisp::core::ModelOptions{{"context_window", "32768"}, {"vision", "true"}};
    ASSERT_TRUE(f.models->update(id, u));
    EXPECT_EQ(
        f.client->active_model(),
        (LlmModelInfo{.profile_id = id, .model = "m", .context_window = 32768, .vision = true}));
    EXPECT_TRUE(f.server.requests().empty());
}

TEST(LlmClient, NoActiveProfileHasNoModel) {
    Fixture f(/*with_profile=*/false);
    EXPECT_EQ(f.client->active_model(), std::nullopt);
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::NoModel);
    EXPECT_EQ(r.profile_id, "");
}

TEST(LlmClient, ResultNamesTheProfileThatAnswered) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("ok"), FakeLlmServer::sse_finish("stop"),
                              FakeLlmServer::sse_done()}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_EQ(r.model, "m");
    EXPECT_EQ(r.profile_id, f.models->get().active_for(ModelKind::Llm));
    EXPECT_FALSE(r.profile_id.empty());
}

TEST(LlmClient, IsAnLlmProvider) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("h"), FakeLlmServer::sse_text("i"),
                              FakeLlmServer::sse_finish("stop"), FakeLlmServer::sse_done()}});
    LlmProvider& p = *f.client;
    std::vector<std::string> deltas;
    LlmMessage msg{.role = LlmRole::User, .text = "hi"};
    LlmCall call;
    call.messages = {&msg, 1};
    call.on_text = [&](std::string_view d) { deltas.emplace_back(d); };

    LlmResult r = p.complete(call);

    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_EQ(deltas, (std::vector<std::string>{"h", "i"}));
    EXPECT_EQ(std::string(r.text.data(), r.text.size()), "hi");
    EXPECT_EQ(r.finish, LlmFinish::Stop);
}

TEST(LlmClient, CancelMidStreamKeepsPartialText) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("Hel"), FakeLlmServer::sse_text("lo"),
                              FakeLlmServer::sse_done()},
                   .hold_after = 1});
    auto watch = wisp::net::RequestWatch::create();
    ASSERT_NE(watch, nullptr);
    std::atomic<uint32_t> ticket{0};
    LlmMessage msg{.role = LlmRole::User, .text = "hi"};
    LlmCall call;
    call.messages = {&msg, 1};
    call.watch = watch.get();
    call.on_armed = [&](uint32_t t) { ticket = t; };

    LlmResult r;
    std::thread t([&] { r = f.run(call); });
    ASSERT_TRUE(wisp::test::wait_until([&] { return f.server.held() == 1; }, 2000));
    ASSERT_NE(ticket.load(), 0u);
    watch->abort(ticket.load());
    t.join();

    EXPECT_EQ(r.error, LlmError::Cancelled);
    EXPECT_EQ(r.transport, TransportError::Cancelled);
    EXPECT_EQ(std::string(r.text.data(), r.text.size()), "Hel");
    EXPECT_TRUE(r.any_body);
    EXPECT_EQ(r.attempts, 1u);
}

TEST(LlmClient, BodilessTransportErrorIsRetried) {
    Fixture f;
    f.server.push({.fail = TransportError::Connect});
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("hi"), FakeLlmServer::sse_finish("stop"),
                              FakeLlmServer::sse_done()}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_EQ(r.transport, TransportError::None);
    EXPECT_EQ(std::string(r.text.data(), r.text.size()), "hi");
    EXPECT_EQ(r.attempts, 2u);
    EXPECT_EQ(f.sleeps, (std::map<size_t, uint32_t>{{1, 1000}}));
}

// Part of the reply already reached the caller: a retry would repeat it.
TEST(LlmClient, TransportErrorAfterADeltaIsNotRetried) {
    Fixture f;
    f.server.push(
        {.status = 200, .chunks = {FakeLlmServer::sse_text("he")}, .fail = TransportError::Tls});
    f.server.push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_text("never"), FakeLlmServer::sse_finish("stop")}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::Transport);
    EXPECT_EQ(r.transport, TransportError::Tls);
    EXPECT_TRUE(r.any_body);
    EXPECT_EQ(r.attempts, 1u);
    EXPECT_EQ(std::string(r.text.data(), r.text.size()), "he");
    EXPECT_TRUE(f.sleeps.empty());
}

TEST(LlmClient, TransportRetriesShareTheBudgetWith5xx) {
    Fixture f;
    f.server.push({.fail = TransportError::Connect});
    f.server.push({.status = 503});
    f.server.push({.fail = TransportError::Dns});
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::Transport);
    EXPECT_EQ(r.transport, TransportError::Dns);
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.attempts, 3u);
    EXPECT_EQ(f.server.requests().size(), 3u);
    EXPECT_EQ(f.sleeps, (std::map<size_t, uint32_t>{{1, 1000}, {2, 4000}}));
}

TEST(LlmClient, RetryAfterSecondsReplaceTheBackoff) {
    Fixture f;
    f.server.push({.status = 429, .headers = {{"retry-after", "7"}}});
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("ok"), FakeLlmServer::sse_finish("stop")}});
    int slices = 0;
    f.on_sleep = [&] { ++slices; };
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_EQ(r.attempts, 2u);
    EXPECT_EQ(f.sleeps, (std::map<size_t, uint32_t>{{1, 7000}}));
    EXPECT_EQ(slices, static_cast<int>(7000 / LlmClient::kWaitSliceMs));
}

TEST(LlmClient, RetryAfterIsCapped) {
    Fixture f;
    f.server.push({.status = 503, .headers = {{"retry-after", "600"}}});
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_EQ(f.sleeps, (std::map<size_t, uint32_t>{{1, LlmClient::kMaxRetryAfterMs}}));
}

// Only delta-seconds is honoured; a date falls back to the schedule.
TEST(LlmClient, RetryAfterDateUsesTheSchedule) {
    Fixture f;
    f.server.push({.status = 429, .headers = {{"retry-after", "Wed, 21 Oct 2015 07:28:00 GMT"}}});
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    EXPECT_EQ(f.run_user("hi").error, LlmError::None);
    EXPECT_EQ(f.sleeps, (std::map<size_t, uint32_t>{{1, 1000}}));
}

TEST(LlmClient, ContextLengthCodeIsReported) {
    Fixture f;
    f.server.push(
        {.status = 400,
         .chunks = {R"({"error":{"code":"context_length_exceeded","message":"too long"}})"}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::ContextLength);
    EXPECT_EQ(r.code, "context_length_exceeded");
    EXPECT_EQ(r.status, 400);
    EXPECT_EQ(r.attempts, 1u);
    EXPECT_TRUE(r.text.empty());
}

TEST(LlmClient, ContextLengthIsRecognisedFromTheMessage) {
    Fixture f;
    f.server.push(
        {.status = 400,
         .chunks = {R"({"error":{"type":"invalid_request_error",)",
                    R"("message":"This model's maximum context length is 32768 tokens"}})"}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::ContextLength);
    EXPECT_EQ(r.code, "invalid_request_error");
    EXPECT_EQ(r.attempts, 1u);
}

// A message that names the limit on another status is not a refusal of
// the prompt's size.
TEST(LlmClient, ContextLengthMessageNeeds400Or413) {
    Fixture f;
    f.server.push(
        {.status = 422, .chunks = {R"({"error":{"message":"Too many tokens requested"}})"}});
    EXPECT_EQ(f.run_user("hi").error, LlmError::Http);
    f.server.push(
        {.status = 413, .chunks = {R"({"error":{"message":"Too Many Tokens in the prompt"}})"}});
    EXPECT_EQ(f.run_user("hi").error, LlmError::ContextLength);
}

TEST(LlmClient, ErrorCodeOutsideTheCharacterSetIsDropped) {
    Fixture f;
    f.server.push({.status = 400, .chunks = {R"({"error":{"code":"bad code\n","type":"x"}})"}});
    f.server.push(
        {.status = 400, .chunks = {R"({"error":{"code":")" + std::string(65, 'a') + R"("}})"}});
    f.server.push({.status = 503, .chunks = {R"({"error":{"code":"overloaded"}})"}});
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    EXPECT_EQ(f.run_user("hi").code, "");
    EXPECT_EQ(f.run_user("hi").code, "");
    // A successful retry reports no earlier attempt's code.
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_EQ(r.code, "");
}

TEST(LlmClient, ErrorBodyNeverLeaksTheKey) {
    Fixture f(/*with_profile=*/true, "sk-test-key");
    f.server.push(
        {.status = 400,
         .chunks = {R"({"error":{"code":"sk-test-key","message":"bad key sk-test-key"}})"}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::Http);
    EXPECT_EQ(r.code.find("sk-test"), std::string::npos);
    EXPECT_FALSE(mentions_key(r));
    for (const auto& e : wisp::log::ring_entries()) {
        EXPECT_EQ(e.msg.find("sk-test"), std::string::npos) << e.msg;
    }
}

// A provider that echoes a prefix of the key as its code (a "key
// sk-proj-abcd... is invalid" kind of error) gives that piece no way out.
TEST(LlmClient, ErrorCodeThatIsAPieceOfTheKeyIsDropped) {
    const std::string key = "sk-proj-abcdefghijklmnop";
    Fixture f(/*with_profile=*/true, key.c_str());
    f.server.push(
        {.status = 400,
         .chunks = {R"({"error":{"code":")" + key.substr(0, 12) + R"(","message":"bad"}})"}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::Http);
    EXPECT_EQ(r.code, "");
}

// A code that holds 8 bytes or more of the key anywhere in it (here 10,
// wrapped in other characters, so neither holds the other whole) is
// dropped from the result and the log alike.
TEST(LlmClient, ErrorCodeHoldingAPieceOfTheKeyIsDropped) {
    const std::string key = "sk-proj-abcdefghijklmnop";
    const std::string piece = key.substr(8, 10);  // "abcdefghij"
    Fixture f(/*with_profile=*/true, key.c_str());
    f.server.push({.status = 400,
                   .chunks = {R"({"error":{"code":"bad_)" + piece + R"(_key","message":"bad"}})"}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::Http);
    EXPECT_EQ(r.code, "");
    for (const auto& e : wisp::log::ring_entries()) {
        EXPECT_EQ(e.msg.find(piece), std::string::npos) << e.msg;
    }
}

// The metadata line of a failed request carries the provider's code, so
// the gateway's error.code can be read off the device's log.
TEST(LlmClient, FailedRequestLogsTheProviderCode) {
    Fixture f;
    f.server.push(
        {.status = 400, .chunks = {R"({"error":{"code":"model_not_found","message":"no such"}})"}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::Http);
    EXPECT_EQ(r.code, "model_not_found");
    bool logged = false;
    for (const auto& e : wisp::log::ring_entries()) {
        if (e.msg.find("error=http code=model_not_found") != std::string::npos) logged = true;
    }
    EXPECT_TRUE(logged);
}

TEST(LlmClient, BrokenEventIsBadStream) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("Hi"), "data: {not json\n\n",
                              FakeLlmServer::sse_text("never"), FakeLlmServer::sse_done()}});
    std::string seen;
    LlmMessage msg{.role = LlmRole::User, .text = "hi"};
    LlmCall call;
    call.messages = {&msg, 1};
    call.on_text = [&](std::string_view d) { seen += d; };

    LlmResult r = f.run(call);

    EXPECT_EQ(r.error, LlmError::BadStream);
    EXPECT_EQ(seen, "Hi");
    EXPECT_EQ(r.attempts, 1u);
}

TEST(LlmClient, StreamEndingWithoutFinishIsBadStream) {
    Fixture f;
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("Hal")}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::BadStream);
    EXPECT_EQ(std::string(r.text.data(), r.text.size()), "Hal");
}

TEST(LlmClient, ErrorEventInStreamIsBadStream) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {"data: {\"error\":{\"message\":\"overloaded\"}}\n\n",
                              FakeLlmServer::sse_done()}});
    EXPECT_EQ(f.run_user("hi").error, LlmError::BadStream);
}

TEST(LlmClient, KeyNeverInLogsOrResult) {
    Fixture f;
    f.server.push(
        {.status = 401,
         .chunks = {"{\"error\":{\"message\":\"Incorrect API key provided: sk-test\"}}"}});
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("Hello"), FakeLlmServer::sse_finish("stop"),
                              FakeLlmServer::sse_done()}});
    f.server.push({.fail = TransportError::Tls});

    for (int i = 0; i < 3; ++i) EXPECT_FALSE(mentions_key(f.run_user("hi"))) << i;

    for (const auto& e : wisp::log::ring_entries()) {
        EXPECT_EQ(e.msg.find(kKey), std::string::npos) << e.msg;
    }
    // The key goes in the header and nowhere else in the request.
    for (const auto& req : f.server.requests()) {
        EXPECT_EQ(req.url.find(kKey), std::string::npos);
        EXPECT_EQ(req.body.find(kKey), std::string::npos);
    }
}

TEST(LlmClient, LogsOnlyMetadata) {
    Fixture f;
    f.server.push({.status = 200,
                   .chunks = {FakeLlmServer::sse_text("Hello"), FakeLlmServer::sse_finish("stop"),
                              FakeLlmServer::sse_usage(12, 34), FakeLlmServer::sse_done()}});

    EXPECT_EQ(f.run_user("secret question").error, LlmError::None);

    std::vector<std::string> lines;
    for (const auto& e : wisp::log::ring_entries()) {
        EXPECT_EQ(e.msg.find("Hello"), std::string::npos) << e.msg;
        EXPECT_EQ(e.msg.find("secret question"), std::string::npos) << e.msg;
        if (e.tag == "llm") lines.push_back(e.msg);
    }
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].rfind("model=m status=200 ms=", 0), 0u) << lines[0];
    EXPECT_NE(lines[0].find(" tokens=46"), std::string::npos) << lines[0];
}

TEST(LlmClient, BuildChatBodyIsValidJson) {
    LlmMessage msg{.role = LlmRole::User, .text = "bad \xC3 utf8 \x01"};
    auto body = wisp::net::build_chat_body("m\"x", {&msg, 1}, "");
    auto v = wisp::from_json(std::string_view(body.data(), body.size()));
    ASSERT_TRUE(v);
    EXPECT_EQ(*v.value().find("model"), Value("m\"x"));
}

// LlmBodyGolden: today's request bytes, compared as raw strings. A gateway's
// prefix cache and a strict parser both see the bytes, not the parsed JSON,
// so field order, null content and escaping are pinned here.

std::string str(const wisp::os::LargeString& s) { return {s.data(), s.size()}; }

const std::string kGoldenTools =
    R"([{"type":"function","function":{"name":"a_b","parameters":{}}}])";

// Every message shape build_chat_body writes. The arrays are static so that
// the views in the messages outlive the call.
std::vector<LlmMessage> every_shape() {
    static LlmToolCall two[2];
    static LlmToolCall one[1];
    two[0].id = "c1";
    two[0].name = "a_b";
    two[0].arguments = "{\"x\":1}";
    two[1].id = "c2";
    two[1].name = "c_d";
    two[1].arguments = "";
    one[0].id = "c3";
    one[0].name = "a_b";
    one[0].arguments = "{}";
    return {
        {.role = LlmRole::System, .text = "S"},
        {.role = LlmRole::User, .text = "q\"\n\xC3\xBC"},
        {.role = LlmRole::Assistant, .text = "A"},
        {.role = LlmRole::Assistant, .text = "", .tool_calls = two},
        {.role = LlmRole::Assistant, .text = "T", .tool_calls = one},
        {.role = LlmRole::Tool, .text = "{\"ok\":true}", .tool_call_id = "c1"},
    };
}

TEST(LlmBodyGolden, EveryMessageShape) {
    const std::vector<LlmMessage> msgs = every_shape();
    EXPECT_EQ(
        str(wisp::net::build_chat_body("m", msgs, kGoldenTools)),
        R"({"model":"m","stream":true,"stream_options":{"include_usage":true},"messages":[)"
        R"({"role":"system","content":"S"},{"role":"user","content":"q\"\nü"},)"
        R"({"role":"assistant","content":"A"},)"
        R"({"role":"assistant","content":null,"tool_calls":[{"id":"c1","type":"function","function":{"name":"a_b","arguments":"{\"x\":1}"}},)"
        R"({"id":"c2","type":"function","function":{"name":"c_d","arguments":""}}]},)"
        R"({"role":"assistant","content":"T","tool_calls":[{"id":"c3","type":"function","function":{"name":"a_b","arguments":"{}"}}]},)"
        R"({"role":"tool","tool_call_id":"c1","content":"{\"ok\":true}"}],"tools":)" +
            kGoldenTools + "}");
}

TEST(LlmBodyGolden, NoToolsLeavesTheKeyOut) {
    LlmMessage msg{.role = LlmRole::User, .text = "hi"};
    EXPECT_EQ(
        str(wisp::net::build_chat_body("m", {&msg, 1}, "")),
        R"({"model":"m","stream":true,"stream_options":{"include_usage":true},"messages":[{"role":"user","content":"hi"}]})");
}

TEST(LlmBodyGolden, ClientSendsExactlyTheBuiltBody) {
    Fixture f;
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    const std::vector<LlmMessage> msgs = every_shape();
    LlmCall call;
    call.messages = msgs;
    call.tools_json = kGoldenTools;

    f.run(call);

    auto reqs = f.server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].body, str(wisp::net::build_chat_body("m", msgs, kGoldenTools)));
    EXPECT_EQ(reqs[0].url, std::string(kBase) + "/chat/completions");
    using Headers = std::vector<std::pair<std::string, std::string>>;
    EXPECT_EQ(reqs[0].headers, (Headers{{"Content-Type", "application/json"},
                                        {"Accept", "text/event-stream"},
                                        {"Authorization", std::string("Bearer ") + kKey}}));
}

// LlmDialectBody: the body each dialect option writes (plan decisions 3-5).

using wisp::core::LlmDialect;
using wisp::core::LlmOutField;
using wisp::core::LlmReasoning;
using wisp::core::LlmSystemMsgs;

std::string body_of(std::span<const LlmMessage> msgs, const LlmDialect& d,
                    std::string_view tools = kGoldenTools) {
    return str(wisp::net::build_chat_body("m", msgs, tools, d));
}

// Today's body for `msgs` with `tail` in place of its closing '}'.
std::string todays_with(std::span<const LlmMessage> msgs, std::string_view tail) {
    std::string base = str(wisp::net::build_chat_body("m", msgs, kGoldenTools));
    base.pop_back();
    return base + std::string(tail);
}

TEST(LlmDialectBody, DefaultDialectIsTodaysBody) {
    const std::vector<LlmMessage> msgs = every_shape();
    EXPECT_EQ(
        body_of(msgs, LlmDialect{}),
        R"({"model":"m","stream":true,"stream_options":{"include_usage":true},"messages":[)"
        R"({"role":"system","content":"S"},{"role":"user","content":"q\"\nü"},)"
        R"({"role":"assistant","content":"A"},)"
        R"({"role":"assistant","content":null,"tool_calls":[{"id":"c1","type":"function","function":{"name":"a_b","arguments":"{\"x\":1}"}},)"
        R"({"id":"c2","type":"function","function":{"name":"c_d","arguments":""}}]},)"
        R"({"role":"assistant","content":"T","tool_calls":[{"id":"c3","type":"function","function":{"name":"a_b","arguments":"{}"}}]},)"
        R"({"role":"tool","tool_call_id":"c1","content":"{\"ok\":true}"}],"tools":)" +
            kGoldenTools + "}");
}

TEST(LlmDialectBody, OutTokensUsesTheNamedField) {
    const std::vector<LlmMessage> msgs = every_shape();
    EXPECT_EQ(body_of(msgs, {.out_tokens = 1024}), todays_with(msgs, R"(,"max_tokens":1024})"));
    EXPECT_EQ(body_of(msgs, {.out_tokens = 1024, .out_field = LlmOutField::MaxCompletionTokens}),
              todays_with(msgs, R"(,"max_completion_tokens":1024})"));
    EXPECT_EQ(body_of(msgs, {.out_field = LlmOutField::MaxCompletionTokens}),
              todays_with(msgs, "}"));
}

TEST(LlmDialectBody, ParallelToolsOnlyWithTools) {
    const std::vector<LlmMessage> msgs = every_shape();
    const std::string body = body_of(msgs, {.out_tokens = 512, .parallel_tools = false});
    const std::string tail =
        R"(,"tools":)" + kGoldenTools + R"(,"parallel_tool_calls":false,"max_tokens":512})";
    ASSERT_GE(body.size(), tail.size());
    EXPECT_EQ(body.substr(body.size() - tail.size()), tail);
    EXPECT_EQ(body, todays_with(msgs, R"(,"parallel_tool_calls":false,"max_tokens":512})"));

    const std::string no_tools = body_of(msgs, {.parallel_tools = true}, "");
    EXPECT_EQ(no_tools.find("parallel_tool_calls"), std::string::npos);
    EXPECT_EQ(no_tools, str(wisp::net::build_chat_body("m", msgs, "")));
}

TEST(LlmDialectBody, ReasoningSentOnlyUnderEcho) {
    const std::vector<LlmMessage> shapes = every_shape();
    std::vector<LlmMessage> msgs = {
        {.role = LlmRole::Assistant, .text = "A", .reasoning = "r\"1"},
        shapes[4],  // "T" with the call c3
        {.role = LlmRole::Assistant, .text = "B"},
    };
    msgs[1].reasoning = "r2";

    const std::string drop = body_of(msgs, LlmDialect{});
    EXPECT_EQ(drop.find("reasoning_content"), std::string::npos);
    EXPECT_EQ(drop, todays_with(msgs, "}"));

    const std::string echo = body_of(msgs, {.reasoning = LlmReasoning::Echo});
    EXPECT_NE(echo.find(R"({"role":"assistant","content":"A","reasoning_content":"r\"1"})"),
              std::string::npos)
        << echo;
    EXPECT_NE(
        echo.find(
            R"({"role":"assistant","content":"T","tool_calls":[{"id":"c3","type":"function","function":{"name":"a_b","arguments":"{}"}}],"reasoning_content":"r2"})"),
        std::string::npos)
        << echo;
    EXPECT_NE(echo.find(R"({"role":"assistant","content":"B"}])"), std::string::npos) << echo;
}

const std::string kBodyHead =
    R"({"model":"m","stream":true,"stream_options":{"include_usage":true},"messages":[)";

TEST(LlmDialectBody, FirstMergesMovablesIntoUserMessages) {
    const std::vector<LlmMessage> msgs = {
        {.role = LlmRole::System, .text = "P"},
        {.role = LlmRole::System, .text = "S1", .movable = true},
        {.role = LlmRole::User, .text = "u1"},
        {.role = LlmRole::Assistant, .text = "a1"},
        {.role = LlmRole::User, .text = "u2"},
        {.role = LlmRole::System, .text = "D", .movable = true},
    };
    EXPECT_EQ(
        body_of(msgs, {.system_msgs = LlmSystemMsgs::First}, ""),
        kBodyHead +
            R"({"role":"system","content":"P"},)"
            R"({"role":"user","content":[{"type":"text","text":"<context>\nS1\n</context>"},{"type":"text","text":"u1"}]},)"
            R"({"role":"assistant","content":"a1"},)"
            R"({"role":"user","content":[{"type":"text","text":"<context>\nD\n</context>"},{"type":"text","text":"u2"}]}]})");
}

TEST(LlmDialectBody, FirstPutsTheDynamicInfoFirst) {
    const std::vector<LlmMessage> shapes = every_shape();
    const std::vector<LlmMessage> msgs = {
        {.role = LlmRole::System, .text = "P"},
        {.role = LlmRole::User, .text = "u1"},
        {.role = LlmRole::System, .text = "S", .movable = true},
        {.role = LlmRole::User, .text = "u2"},
        shapes[4],
        {.role = LlmRole::Tool, .text = "{}", .tool_call_id = "c3"},
        {.role = LlmRole::System, .text = "D\"", .movable = true},
    };
    EXPECT_EQ(
        body_of(msgs, {.system_msgs = LlmSystemMsgs::First}, ""),
        kBodyHead +
            R"({"role":"system","content":"P"},{"role":"user","content":"u1"},)"
            R"({"role":"user","content":[{"type":"text","text":"<context>\nD\"\n</context>"},)"
            R"({"type":"text","text":"<context>\nS\n</context>"},{"type":"text","text":"u2"}]},)"
            R"({"role":"assistant","content":"T","tool_calls":[{"id":"c3","type":"function","function":{"name":"a_b","arguments":"{}"}}]},)"
            R"({"role":"tool","tool_call_id":"c3","content":"{}"}]})");
}

TEST(LlmDialectBody, FirstWithoutAUserKeepsSystem) {
    const std::vector<LlmMessage> msgs = {
        {.role = LlmRole::System, .text = "P"},
        {.role = LlmRole::System, .text = "S", .movable = true},
    };
    EXPECT_EQ(body_of(msgs, {.system_msgs = LlmSystemMsgs::First}), body_of(msgs, LlmDialect{}));
    EXPECT_EQ(body_of(msgs, LlmDialect{}),
              kBodyHead +
                  R"({"role":"system","content":"P"},{"role":"system","content":"S"}],)"
                  R"("tools":)" +
                  kGoldenTools + "}");
}

// Movable blocks in a row keep their order inside the user message that
// takes them, both before a user message and after the last one.
TEST(LlmDialectBody, FirstKeepsSeveralMovablesInOrder) {
    const std::vector<LlmMessage> msgs = {
        {.role = LlmRole::System, .text = "P"},
        {.role = LlmRole::System, .text = "S1", .movable = true},
        {.role = LlmRole::System, .text = "S2", .movable = true},
        {.role = LlmRole::User, .text = "u1"},
        {.role = LlmRole::Assistant, .text = "a1"},
        {.role = LlmRole::User, .text = "u2"},
        {.role = LlmRole::System, .text = "D1", .movable = true},
        {.role = LlmRole::System, .text = "D2", .movable = true},
    };
    EXPECT_EQ(
        body_of(msgs, {.system_msgs = LlmSystemMsgs::First}, ""),
        kBodyHead +
            R"({"role":"system","content":"P"},)"
            R"({"role":"user","content":[{"type":"text","text":"<context>\nS1\n</context>"},)"
            R"({"type":"text","text":"<context>\nS2\n</context>"},{"type":"text","text":"u1"}]},)"
            R"({"role":"assistant","content":"a1"},)"
            R"({"role":"user","content":[{"type":"text","text":"<context>\nD1\n</context>"},)"
            R"({"type":"text","text":"<context>\nD2\n</context>"},{"type":"text","text":"u2"}]}]})");
}

// A movable block after the last user message, past the assistant's reply,
// goes back into that user message ahead of the blocks before it; the
// messages between keep their place.
TEST(LlmDialectBody, FirstMovesATrailingMovableBackPastTheReply) {
    const std::vector<LlmMessage> msgs = {
        {.role = LlmRole::System, .text = "P"},
        {.role = LlmRole::System, .text = "S", .movable = true},
        {.role = LlmRole::User, .text = "u1"},
        {.role = LlmRole::Assistant, .text = "a1"},
        {.role = LlmRole::System, .text = "D", .movable = true},
    };
    EXPECT_EQ(
        body_of(msgs, {.system_msgs = LlmSystemMsgs::First}, ""),
        kBodyHead +
            R"({"role":"system","content":"P"},)"
            R"({"role":"user","content":[{"type":"text","text":"<context>\nD\n</context>"},)"
            R"({"type":"text","text":"<context>\nS\n</context>"},{"type":"text","text":"u1"}]},)"
            R"({"role":"assistant","content":"a1"}]})");
}

TEST(LlmDialectBody, AnywhereIgnoresMovable) {
    std::vector<LlmMessage> msgs = {
        {.role = LlmRole::System, .text = "P"},
        {.role = LlmRole::System, .text = "S1", .movable = true},
        {.role = LlmRole::User, .text = "u1"},
        {.role = LlmRole::Assistant, .text = "a1"},
        {.role = LlmRole::User, .text = "u2"},
        {.role = LlmRole::System, .text = "D", .movable = true},
    };
    const std::string movable = body_of(msgs, LlmDialect{});
    for (LlmMessage& m : msgs) m.movable = false;
    EXPECT_EQ(movable, body_of(msgs, LlmDialect{}));
    EXPECT_EQ(movable, str(wisp::net::build_chat_body("m", msgs, kGoldenTools)));
}

// LlmClient with dialect options: the resolved dialect reaches the body,
// and reasoning is read only under echo.

std::string sse_reasoning(std::string_view s) {
    return FakeLlmServer::sse_event(
        Map{{"choices", List{Map{{"index", 0},
                                 {"delta", Map{{"reasoning_content", std::string(s)}}},
                                 {"finish_reason", Value()}}}}});
}

void set_options(Fixture& f, wisp::core::ModelOptions options,
                 std::optional<std::string> preset = std::nullopt) {
    wisp::core::ProfileUpdate u;
    u.options = std::move(options);
    u.preset = std::move(preset);
    ASSERT_TRUE(f.models->update(f.models->get().active_for(ModelKind::Llm), u));
}

std::string ok_body_tail(Fixture& f) {
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    f.run_user("hi");
    const auto reqs = f.server.requests();
    return reqs.empty() ? std::string() : reqs.back().body;
}

bool ends_with(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

TEST(LlmClient, CustomProfileWithoutOptionsSendsTodaysBody) {
    Fixture f;
    f.server.push({.status = 200, .chunks = {FakeLlmServer::sse_done()}});
    const std::vector<LlmMessage> msgs = every_shape();
    LlmCall call;
    call.messages = msgs;
    call.tools_json = kGoldenTools;
    f.run(call);
    const auto reqs = f.server.requests();
    ASSERT_EQ(reqs.size(), 1u);
    EXPECT_EQ(reqs[0].body, str(wisp::net::build_chat_body("m", msgs, kGoldenTools)));
}

TEST(LlmClient, SendsTheResolvedDialect) {
    Fixture f;
    set_options(f, {{"out_tokens", "512"}});
    EXPECT_TRUE(ends_with(ok_body_tail(f), R"("max_tokens":512})"));

    set_options(f, {{"out_tokens", "512"}}, "openai");
    EXPECT_TRUE(ends_with(ok_body_tail(f), R"("max_completion_tokens":512})"));

    set_options(f, {{"out_tokens", "512"}, {"out_field", "max_tokens"}});
    EXPECT_TRUE(ends_with(ok_body_tail(f), R"(,"max_tokens":512})"));
    EXPECT_EQ(f.server.requests().size(), 3u);
}

TEST(LlmClient, ActiveModelResolvesTheDialect) {
    Fixture f;
    EXPECT_EQ(f.client->active_model()->dialect, LlmDialect{});
    set_options(f, {}, "anthropic");
    EXPECT_EQ(f.client->active_model()->dialect.system_msgs, LlmSystemMsgs::First);
    set_options(f, {{"system_msgs", "anywhere"}});
    EXPECT_EQ(f.client->active_model()->dialect.system_msgs, LlmSystemMsgs::Anywhere);
}

struct ReasoningRun {
    LlmResult r;
    std::vector<std::string> texts;
    int bytes = 0;
};

ReasoningRun run_reasoning_stream(Fixture& f) {
    f.server.push(
        {.status = 200,
         .chunks = {sse_reasoning("th"), sse_reasoning("ink"), FakeLlmServer::sse_text("hi"),
                    FakeLlmServer::sse_finish("stop"), FakeLlmServer::sse_done()}});
    ReasoningRun out;
    LlmMessage msg{.role = LlmRole::User, .text = "q"};
    LlmCall call;
    call.messages = {&msg, 1};
    call.on_text = [&](std::string_view d) { out.texts.emplace_back(d); };
    call.on_bytes = [&] { ++out.bytes; };
    out.r = f.run(call);
    return out;
}

TEST(LlmClient, ReasoningKeptUnderEcho) {
    Fixture f;
    set_options(f, {{"reasoning", "echo"}});
    const ReasoningRun run = run_reasoning_stream(f);
    EXPECT_EQ(run.r.error, LlmError::None);
    EXPECT_EQ(str(run.r.reasoning), "think");
    EXPECT_EQ(str(run.r.text), "hi");
    EXPECT_EQ(run.texts, (std::vector<std::string>{"hi"}));
}

TEST(LlmClient, ReasoningIgnoredUnderDrop) {
    Fixture echo_f;
    set_options(echo_f, {{"reasoning", "echo"}});
    const ReasoningRun echo = run_reasoning_stream(echo_f);

    Fixture f;
    const ReasoningRun drop = run_reasoning_stream(f);
    EXPECT_EQ(drop.r.error, LlmError::None);
    EXPECT_TRUE(drop.r.reasoning.empty());
    EXPECT_EQ(str(drop.r.text), "hi");
    EXPECT_EQ(drop.texts, (std::vector<std::string>{"hi"}));
    EXPECT_GT(drop.bytes, 0);
    EXPECT_EQ(drop.bytes, echo.bytes);
}

TEST(LlmClient, ReasoningIsNotDuplicatedByARetry) {
    Fixture f;
    set_options(f, {{"reasoning", "echo"}});
    f.server.push({.status = 200, .chunks = {sse_reasoning("r")}, .fail = TransportError::Tls});
    f.server.push({.status = 200,
                   .chunks = {sse_reasoning("r"), FakeLlmServer::sse_text("ok"),
                              FakeLlmServer::sse_finish("stop"), FakeLlmServer::sse_done()}});
    LlmResult r = f.run_user("hi");
    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_EQ(r.attempts, 2u);
    EXPECT_EQ(str(r.reasoning), "r");
    EXPECT_EQ(str(r.text), "ok");
    EXPECT_TRUE(r.any_body);
}

TEST(LlmClient, ReasoningOverTheCapIsDropped) {
    Fixture f;
    set_options(f, {{"reasoning", "echo"}});
    const std::string half(LlmClient::kMaxReasoningBytes / 2, 'x');
    f.server.push({.status = 200,
                   .chunks = {sse_reasoning(half), sse_reasoning(half), sse_reasoning("y"),
                              FakeLlmServer::sse_text("hi"), FakeLlmServer::sse_finish("stop"),
                              FakeLlmServer::sse_done()}});
    LlmResult r = f.run_user("q");
    EXPECT_EQ(r.error, LlmError::None);
    EXPECT_TRUE(r.reasoning.empty());
    EXPECT_EQ(str(r.text), "hi");
}

}  // namespace
