// /api/v1/agent (agent-core spec 7.1, 7.2): the routes over the App's
// AgentApiAdapter, a real AgentService on the scripted FakeLlmServer, and
// its WebSocket frames through the admin server's own hub.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "admin/admin_fixture.hpp"
#include "agent_wiring.hpp"
#include "support/agent_fixture.hpp"
#include "support/confirm_harness.hpp"
#include "support/fake_http.hpp"
#include "support/wait.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/conversation.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/strings.hpp"

using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::agent::AgentEvent;
using wisp::agent::AgentService;
using wisp::agent::AgentState;
using wisp::agent::Message;
using wisp::agent::Role;
using wisp::agent::ToolCtx;
using wisp::agent::ToolOutput;
using wisp::app::AgentApiAdapter;
using wisp::app::AgentWsBridge;
using wisp::net::HttpRequest;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::AgentHarness;
using wisp::test::body_json;
using wisp::test::body_text;
using wisp::test::error_code;
using wisp::test::FakeLlmServer;
using wisp::test::FakeReply;
using wisp::test::make_req;
using wisp::test::wait_until;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

constexpr const char* kBase = "/api/v1/agent";

FakeReply text_reply(const std::string& text) {
    return {.status = 200,
            .chunks = {FakeLlmServer::sse_text(text), FakeLlmServer::sse_finish("stop"),
                       FakeLlmServer::sse_done()}};
}

FakeReply tool_reply(const std::string& id, const std::string& wire) {
    return {.status = 200,
            .chunks = {FakeLlmServer::sse_tool(0, id, wire, "{}"),
                       FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}};
}

std::string repeat(std::string_view s, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) out += s;
    return out;
}

// Records the frames the hub sends one client.
class Conn : public wisp::net::WsConnection {
public:
    explicit Conn(int id) : id_(id) {}
    int id() const override { return id_; }
    bool send_text(std::string_view text) override {
        auto v = wisp::from_json(text);
        std::lock_guard<std::mutex> lock(mu_);
        if (v) frames_.push_back(v.value());
        return true;
    }
    void close() override {}

    std::vector<Value> agent_frames(std::string_view kind = {}) {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Value> out;
        for (const Value& v : frames_) {
            const Value* t = v.find("t");
            const Value* k = v.find("k");
            if (t == nullptr || t->as_string() != "agent") continue;
            if (!kind.empty() && (k == nullptr || k->as_string() != kind)) continue;
            out.push_back(v);
        }
        return out;
    }

private:
    const int id_;
    std::mutex mu_;
    std::vector<Value> frames_;
};

class AgentRoutes : public ::testing::Test {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init();
        // Paired before the agent's flash thread starts: pairing writes
        // on the calling thread in this fixture.
        token = f.token();
    }

    void TearDown() override {
        if (h) h->server.release();
        if (svc) {
            svc->stop();
            svc->remove_listener(listener);
        }
        if (bridge) bridge->stop();
        f.srv.reset();
        bridge.reset();
        svc.reset();
        adapter.reset();
        h.reset();
        ch.reset();
        wisp::log::reset_for_test();
    }

    // The agent with one tool ("test.big", a 500-byte result), wired to a
    // fresh admin server. `seed` runs on the worker before the service
    // starts.
    void start(bool with_confirm = false, std::function<void(AgentHarness&)> seed = nullptr) {
        h = std::make_unique<AgentHarness>();
        wisp::agent::ToolDef big;
        big.name = "test.big";
        big.description = "test tool";
        big.run = [](const Value&, ToolCtx&) {
            return ToolOutput{.ok = true,
                              .result = Map{{"ok", true}, {"data", std::string(500, 'x')}}};
        };
        ASSERT_TRUE(h->tools->add(std::move(big)));
        if (seed) ASSERT_TRUE(h->worker.run_sync([&] { seed(*h); }, 5000));
        if (with_confirm) {
            ch = std::make_unique<wisp::test::ConfirmHarness>();
            ASSERT_TRUE(ch->ok);
        }
        svc = std::make_unique<AgentService>(h->deps());
        if (ch) svc->set_confirm(ch->confirm.get());
        adapter = std::make_unique<AgentApiAdapter>(AgentApiAdapter::Deps{
            .service = *svc,
            .store = *h->store,
            .models = *h->models,
            .run_io =
                [this](std::function<void()> job) {
                    return h->worker.run_sync(std::move(job), wisp::os::kForever);
                },
        });
        bridge = std::make_unique<AgentWsBridge>(
            [this](std::string json) {
                if (f.srv) f.srv->ws_hub().send_agent(std::move(json));
            },
            [this](uint64_t seq) { return h->store->find(seq); },
            [this] { return adapter->epoch(); });
        listener = svc->add_listener([this](const AgentEvent& e) {
            adapter->on_event(e);
            bridge->on_event(e);
        });
        ASSERT_TRUE(svc->start());
        serve(adapter.get());
    }

    void serve(wisp::admin::AgentApi* api) {
        wisp::admin::AdminDeps deps = f.make_deps();
        deps.agent = api;
        f.srv = std::make_unique<wisp::admin::AdminServer>(std::move(deps));
    }

    std::shared_ptr<Conn> open_ws() {
        auto c = std::make_shared<Conn>(1);
        EXPECT_TRUE(f.srv->ws_hub().open(c, "test"));
        f.srv->ws_hub().on_message(1, R"({"sub":["agent"]})");
        return c;
    }

    HttpResponse call(Method m, const std::string& path, const std::string& body = {}) {
        HttpRequest req = with_token(make_req(m, kBase + path, body), token);
        if (m != Method::Get) req = with_csrf(std::move(req));
        return f.srv->handle(req);
    }

    // POST /agent/messages the way the server sends it: streamed.
    HttpResponse post_body(const std::string& body, bool csrf = true) {
        HttpRequest head =
            with_token(make_req(Method::Post, std::string(kBase) + "/messages"), token);
        if (csrf) head = with_csrf(std::move(head));
        head.content_length = body.size();
        auto stream = f.srv->stream_handler();
        auto start = stream(head);
        EXPECT_TRUE(start.has_value()) << "not streamed";
        if (!start) return {};
        if (!start->sink) return start->response;
        std::span<const uint8_t> rest(reinterpret_cast<const uint8_t*>(body.data()), body.size());
        while (!rest.empty()) {
            const size_t n = std::min(rest.size(), wisp::net::kBodyChunkBytes);
            if (auto early = start->sink->on_data(rest.first(n))) return *early;
            rest = rest.subspan(n);
        }
        return start->sink->on_end();
    }

    HttpResponse post_message(const std::string& text) {
        return post_body(wisp::to_json(Value(Map{{"text", Value(text)}})));
    }

    Value conversation() { return body_json(call(Method::Get, "/conversation")); }

    const List& messages(const Value& conv) { return conv.find("messages")->as_list(); }

    bool wait_idle_with(size_t n_messages) {
        return wait_until(
            [&] {
                return svc->state() == AgentState::Idle && !svc->view().running_turn &&
                       svc->view().messages->size() >= n_messages;
            },
            5000);
    }

    // One finished turn: "hi" -> test.big -> "done".
    void run_tool_turn() {
        h->server.push(tool_reply("c1", "test_big"));
        h->server.push(text_reply("done"));
        ASSERT_EQ(post_message("hi").status, 202);
        // user, marker begin, assistant(call), tool, assistant, marker end
        ASSERT_TRUE(wait_idle_with(6));
    }

    std::optional<Value> find_role(const Value& conv, const std::string& role) {
        for (const Value& m : messages(conv)) {
            if (m.find("role")->as_string() == role) return m;
        }
        return std::nullopt;
    }

    AdminFixture f;
    std::string token;
    std::unique_ptr<AgentHarness> h;
    std::unique_ptr<wisp::test::ConfirmHarness> ch;
    std::unique_ptr<AgentService> svc;
    std::unique_ptr<AgentApiAdapter> adapter;
    std::unique_ptr<AgentWsBridge> bridge;
    uint32_t listener = 0;
};

}  // namespace

TEST_F(AgentRoutes, ConversationShape) {
    start();
    Value idle = conversation();
    for (const char* k :
         {"epoch", "state", "tool", "model", "messages", "turn", "queued", "confirm", "total"}) {
        EXPECT_NE(idle.find(k), nullptr) << k;
    }
    EXPECT_EQ(idle.find("state")->as_string(), "idle");
    EXPECT_EQ(idle.find("tool")->type(), Value::Type::Null);
    EXPECT_EQ(idle.find("turn")->type(), Value::Type::Null);
    EXPECT_EQ(idle.find("confirm")->type(), Value::Type::Null);
    EXPECT_FALSE(idle.find("queued")->as_bool());
    const Value* model = idle.find("model");
    ASSERT_EQ(model->type(), Value::Type::Map);
    EXPECT_EQ(model->find("name")->as_string(), "main");
    EXPECT_EQ(model->find("model")->as_string(), "m");
    EXPECT_FALSE(model->find("vision")->as_bool());

    run_tool_turn();
    Value conv = conversation();
    auto tool = find_role(conv, "tool");
    ASSERT_TRUE(tool);
    EXPECT_EQ(tool->find("name")->as_string(), "test.big");
    EXPECT_LE(tool->find("result")->as_string().size(), 200u);
    EXPECT_GT(tool->find("result_len")->as_int(), 500);
    EXPECT_EQ(conv.find("total")->as_int(), static_cast<int64_t>(messages(conv).size()));
}

TEST_F(AgentRoutes, WebMessagesAreMarkedWeb) {
    start();
    h->server.push(text_reply("hello"));
    ASSERT_EQ(post_message("hi").status, 202);
    ASSERT_TRUE(wait_idle_with(4));
    auto user = find_role(conversation(), "user");
    ASSERT_TRUE(user);
    EXPECT_EQ(user->find("text")->as_string(), "hi");
    EXPECT_EQ(user->find("source")->as_string(), "web");
}

TEST_F(AgentRoutes, SendQueuesAndRefusesWhenFull) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("slow")}, .hold_after = 1});
    HttpResponse first = post_message("one");
    ASSERT_EQ(first.status, 202);
    EXPECT_FALSE(body_json(first).find("queued")->as_bool());
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 5000));
    HttpResponse second = post_message("two");
    ASSERT_EQ(second.status, 202);
    EXPECT_TRUE(body_json(second).find("queued")->as_bool());
    EXPECT_TRUE(conversation().find("queued")->as_bool());
    HttpResponse third = post_message("three");
    EXPECT_EQ(third.status, 409);
    EXPECT_EQ(error_code(third), "busy");

    // Stop drops the waiting one and ends the turn.
    EXPECT_EQ(call(Method::Post, "/stop").status, 200);
    EXPECT_TRUE(wait_until([&] { return svc->state() == AgentState::Idle; }, 5000));
    EXPECT_FALSE(conversation().find("queued")->as_bool());
}

TEST_F(AgentRoutes, ConcurrentSendsQueueOneRefuseRest) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("slow")}, .hold_after = 1});
    std::atomic<int> accepted{0}, refused{0}, other{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&, i] {
            const int status = post_message("m" + std::to_string(i)).status;
            if (status == 202) {
                ++accepted;
            } else if (status == 409) {
                ++refused;
            } else {
                ++other;
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(accepted.load(), 2);
    EXPECT_EQ(refused.load(), 2);
    EXPECT_EQ(other.load(), 0);
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 5000));
    EXPECT_EQ(h->server.max_concurrent(), 1);
}

TEST_F(AgentRoutes, FourThousandCjkCharactersAccepted) {
    start();
    const std::string text = repeat("汉", 4000);
    ASSERT_EQ(text.size(), 12000u);
    HttpResponse ok = post_message(text);
    EXPECT_EQ(ok.status, 202) << body_text(ok);
    HttpResponse too_long = post_message(repeat("汉", 4001));
    EXPECT_EQ(too_long.status, 400);
    EXPECT_EQ(error_code(too_long), "too_long");
    HttpResponse empty = post_message("");
    EXPECT_EQ(empty.status, 400);
    EXPECT_EQ(error_code(empty), "empty");
    HttpResponse huge = post_body(std::string(16385, ' '));
    EXPECT_EQ(huge.status, 413);
    EXPECT_EQ(post_body("{not json").status, 400);
    EXPECT_EQ(post_body(R"({"text":5})").status, 400);
}

TEST_F(AgentRoutes, WritesNeedCsrf) {
    start();
    EXPECT_EQ(post_body(R"({"text":"hi"})", /*csrf=*/false).status, 403);
    HttpRequest req = with_token(make_req(Method::Post, std::string(kBase) + "/stop"), token);
    EXPECT_EQ(f.srv->handle(req).status, 403);
    // Nothing was sent.
    EXPECT_TRUE(h->server.requests().empty());
}

TEST_F(AgentRoutes, ClearIsAsyncAndConfirmedOnDevice) {
    start(/*with_confirm=*/true);
    h->server.push(text_reply("hello"));
    ASSERT_EQ(post_message("hi").status, 202);
    ASSERT_TRUE(wait_idle_with(4));
    const size_t before = messages(conversation()).size();
    const int64_t epoch = conversation().find("epoch")->as_int();
    auto ws = open_ws();

    HttpResponse res = call(Method::Delete, "/conversations");
    ASSERT_EQ(res.status, 202);
    EXPECT_TRUE(body_json(res).find("pending")->as_bool());
    ASSERT_TRUE(ch->wait_showing(wisp::ui::tr(wisp::ui::StrId::AgentConfirmClear)));
    ASSERT_TRUE(wait_until([&] { return !ws->agent_frames("confirm").empty(); }, 3000));
    EXPECT_EQ(conversation().find("confirm")->type(), Value::Type::String);
    ASSERT_TRUE(ch->press(false));

    ASSERT_TRUE(wait_until([&] { return !ws->agent_frames("cleared").empty(); }, 5000));
    const Value cleared = ws->agent_frames("cleared").back();
    EXPECT_FALSE(cleared.find("ok")->as_bool());
    EXPECT_EQ(cleared.find("epoch")->as_int(), epoch);
    EXPECT_EQ(messages(conversation()).size(), before);
    EXPECT_EQ(conversation().find("epoch")->as_int(), epoch);

    // Allowed: everything goes, and the epoch moves on.
    ASSERT_EQ(call(Method::Delete, "/conversations").status, 202);
    ASSERT_TRUE(ch->wait_showing(wisp::ui::tr(wisp::ui::StrId::AgentConfirmClear)));
    ASSERT_TRUE(ch->press(true));
    ASSERT_TRUE(wait_until([&] { return ws->agent_frames("cleared").size() == 2; }, 5000));
    EXPECT_TRUE(ws->agent_frames("cleared").back().find("ok")->as_bool());
    EXPECT_GT(ws->agent_frames("cleared").back().find("epoch")->as_int(), epoch);
    EXPECT_TRUE(messages(conversation()).empty());
}

TEST_F(AgentRoutes, ClearIsBusyDuringATurn) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("slow")}, .hold_after = 1});
    ASSERT_EQ(post_message("one").status, 202);
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 5000));
    HttpResponse res = call(Method::Delete, "/conversations");
    EXPECT_EQ(res.status, 409);
    EXPECT_EQ(error_code(res), "busy");
    HttpResponse fresh = call(Method::Post, "/conversation/new");
    EXPECT_EQ(fresh.status, 409);
    EXPECT_EQ(error_code(fresh), "busy");
}

TEST_F(AgentRoutes, ArchiveListAndRead) {
    start();
    h->server.push(text_reply("hello"));
    ASSERT_EQ(post_message("first question").status, 202);
    ASSERT_TRUE(wait_idle_with(4));
    const Value before = conversation();
    auto ws = open_ws();

    ASSERT_EQ(call(Method::Post, "/conversation/new").status, 200);
    // A new conversation: numbers start over, and the epoch says so.
    const Value after = conversation();
    EXPECT_TRUE(messages(after).empty());
    EXPECT_NE(after.find("epoch")->as_int(), before.find("epoch")->as_int());
    ASSERT_TRUE(wait_until([&] { return !ws->agent_frames("cleared").empty(); }, 3000));
    EXPECT_EQ(ws->agent_frames("cleared").back().find("epoch")->as_int(),
              after.find("epoch")->as_int());

    HttpResponse list = call(Method::Get, "/archive");
    ASSERT_EQ(list.status, 200) << body_text(list);
    const Value listed = body_json(list);
    const List& archives = listed.find("archives")->as_list();
    ASSERT_EQ(archives.size(), 1u);
    const int64_t n = archives[0].find("n")->as_int();
    EXPECT_EQ(archives[0].find("first")->as_string(), "first question");
    EXPECT_NE(archives[0].find("ts"), nullptr);

    HttpResponse one = call(Method::Get, "/archive/" + std::to_string(n));
    ASSERT_EQ(one.status, 200) << body_text(one);
    const Value doc = body_json(one);
    EXPECT_EQ(doc.find("n")->as_int(), n);
    const List& msgs = doc.find("messages")->as_list();
    auto user = std::find_if(msgs.begin(), msgs.end(),
                             [](const Value& m) { return m.find("role")->as_string() == "user"; });
    ASSERT_NE(user, msgs.end());
    EXPECT_EQ(user->find("text")->as_string(), "first question");

    EXPECT_EQ(call(Method::Get, "/archive/" + std::to_string(n + 7)).status, 404);
    EXPECT_EQ(call(Method::Get, "/archive/abc").status, 404);
}

TEST_F(AgentRoutes, FullMessageBySeq) {
    start();
    run_tool_turn();
    auto tool = find_role(conversation(), "tool");
    ASSERT_TRUE(tool);
    const int64_t seq = tool->find("seq")->as_int();
    HttpResponse res = call(Method::Get, "/messages/" + std::to_string(seq));
    ASSERT_EQ(res.status, 200) << body_text(res);
    const Value full = body_json(res);
    EXPECT_EQ(full.find("seq")->as_int(), seq);
    const std::string result = full.find("result")->as_string();
    EXPECT_GT(result.size(), 500u);
    EXPECT_TRUE(wisp::from_json(result)) << result;
    EXPECT_EQ(call(Method::Get, "/messages/999").status, 404);
    EXPECT_EQ(call(Method::Get, "/messages/x").status, 404);
}

// A stored summary stands in for older turns; it is not dialogue (and an
// empty one means those turns were dropped).
TEST_F(AgentRoutes, SummariesAreHidden) {
    start(false, [](AgentHarness& harness) {
        Message user;
        user.role = Role::User;
        user.turn = 1;
        user.text.assign("old");
        ASSERT_TRUE(harness.store->append(std::move(user)));
        Message summary;
        summary.role = Role::Summary;
        summary.covers_first = 1;
        summary.covers_last = 1;
        ASSERT_TRUE(harness.store->append(std::move(summary)));
    });
    auto summary = find_role(conversation(), "summary");
    ASSERT_TRUE(summary);
    EXPECT_TRUE(summary->find("hidden")->as_bool());
    EXPECT_TRUE(summary->find("dropped")->as_bool());
    auto user = find_role(conversation(), "user");
    ASSERT_TRUE(user);
    EXPECT_EQ(user->find("hidden"), nullptr);
}

TEST_F(AgentRoutes, WsCarriesTheTurn) {
    start();
    auto ws = open_ws();
    h->server.push(text_reply("hello there"));
    ASSERT_EQ(post_message("hi").status, 202);
    ASSERT_TRUE(wait_idle_with(4));
    ASSERT_TRUE(wait_until([&] { return ws->agent_frames("msg").size() >= 4; }, 3000));
    std::string text;
    for (const Value& v : ws->agent_frames("delta")) text += v.find("text")->as_string();
    EXPECT_EQ(text, "hello there");
    EXPECT_FALSE(ws->agent_frames("state").empty());
}

TEST_F(AgentRoutes, NoKeyInAnyResponse) {
    start();
    run_tool_turn();
    ASSERT_EQ(call(Method::Post, "/conversation/new").status, 200);
    std::vector<HttpResponse> all = {
        call(Method::Get, "/conversation"), call(Method::Get, "/archive"),
        call(Method::Get, "/archive/1"),    call(Method::Get, "/messages/1"),
        call(Method::Post, "/stop"),        post_message("again"),
    };
    for (const HttpResponse& r : all) {
        EXPECT_EQ(body_text(r).find("sk-test"), std::string::npos) << body_text(r);
    }
}

TEST_F(AgentRoutes, UnavailableWithoutAgent) {
    serve(nullptr);
    for (const auto& [m, path] : std::vector<std::pair<Method, std::string>>{
             {Method::Get, "/conversation"},
             {Method::Post, "/messages"},
             {Method::Post, "/stop"},
             {Method::Post, "/conversation/new"},
             {Method::Get, "/archive"},
             {Method::Get, "/archive/1"},
             {Method::Delete, "/conversations"},
             {Method::Get, "/messages/1"},
         }) {
        HttpResponse res = call(m, path, m == Method::Post ? R"({"text":"x"})" : "");
        EXPECT_EQ(res.status, 503) << path;
        EXPECT_EQ(error_code(res), "unavailable") << path;
    }
    HttpResponse streamed = post_message("hi");
    EXPECT_EQ(streamed.status, 503);
}
