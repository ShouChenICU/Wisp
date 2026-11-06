// /agent (agent-core spec 7.1): thin adapters over AdminDeps::agent.
//
// POST /agent/messages is streamed (AdminServer::stream_handler()) so its
// body may reach kMaxMessageBodyBytes, past the 8 KiB every other API body
// is held to: 4000 characters of CJK text are 12000 bytes before JSON
// escaping. The body is collected in PSRAM and handed over once complete.
// A short one sent to handle() takes the same path.
//
// Clearing every conversation needs the user's yes on the device: DELETE
// /agent/conversations only asks and answers 202; the outcome arrives on
// the WebSocket as {"t":"agent","k":"cleared","ok"}.
//
// Large documents (the conversation, an archive, one whole message) come
// from AgentApi as JSON text in Mem::Large and are sent from there.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/base/json.hpp"
#include "wisp/os/large.hpp"

namespace wisp::admin::detail {

namespace {

// The largest POST /agent/messages body.
constexpr uint64_t kMaxMessageBodyBytes = 16 * 1024;
// GET /agent/conversation's last messages.
constexpr size_t kConversationMessages = 100;

net::HttpResponse unavailable() {
    return net::HttpResponse::error(503, "unavailable", "the agent is not available");
}

// AgentApi's errors: its messages are the codes ("busy", "too_long", ...).
net::HttpResponse agent_error(const Error& e) {
    switch (e.code) {
        case ErrorCode::InvalidArgument:
            return net::HttpResponse::error(400, e.message.empty() ? "invalid_argument" : e.message,
                                            e.message);
        case ErrorCode::Busy:
            if (e.message == "not_running") return unavailable();
            return net::HttpResponse::error(409, "busy", "the agent is busy");
        case ErrorCode::NotFound:
            return net::HttpResponse::error(404, "not_found", "no such message or archive");
        default:
            return service_error(e);
    }
}

net::HttpResponse json_doc(int status, JsonDoc doc) {
    net::HttpResponse res;
    res.status = status;
    if (!doc || doc->empty()) {
        res.body = "{}";
        return res;
    }
    res.static_body =
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(doc->data()), doc->size());
    res.body_owner = std::move(doc);
    return res;
}

template <typename T>
std::optional<T> parse_number(std::string_view s) {
    T v{};
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc() || end != s.data() + s.size() || s.empty()) return std::nullopt;
    return v;
}

net::HttpResponse submit(AgentApi& agent, const Value& body) {
    const Value* text = body.find("text");
    if (text == nullptr || text->type() != Value::Type::String) {
        return bad_request("body must be {\"text\": string}");
    }
    auto queued = agent.submit(text->as_string());
    if (!queued) return agent_error(queued.error());
    return net::HttpResponse::json(202, Value(Map{{"queued", Value(queued.value())}}));
}

net::HttpResponse get_conversation(AdminDeps& deps, const RequestContext&) {
    if (deps.agent == nullptr) return unavailable();
    return json_doc(200, deps.agent->conversation(kConversationMessages));
}

net::HttpResponse post_message(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.agent == nullptr) return unavailable();
    return submit(*deps.agent, ctx.body);
}

net::HttpResponse post_stop(AdminDeps& deps, const RequestContext&) {
    if (deps.agent == nullptr) return unavailable();
    deps.agent->stop();
    return ok_json();
}

net::HttpResponse post_new(AdminDeps& deps, const RequestContext&) {
    if (deps.agent == nullptr) return unavailable();
    if (auto r = deps.agent->new_conversation(); !r) return agent_error(r.error());
    return ok_json();
}

net::HttpResponse get_archives(AdminDeps& deps, const RequestContext&) {
    if (deps.agent == nullptr) return unavailable();
    auto list = deps.agent->archives();
    if (!list) return agent_error(list.error());
    return net::HttpResponse::json(200, list.value());
}

net::HttpResponse get_archive(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.agent == nullptr) return unavailable();
    auto n = parse_number<uint32_t>(ctx.param);
    if (!n) return net::HttpResponse::error(404, "not_found", "no such archive");
    auto doc = deps.agent->archive(*n);
    if (!doc) return agent_error(doc.error());
    return json_doc(200, std::move(doc.value()));
}

net::HttpResponse delete_conversations(AdminDeps& deps, const RequestContext&) {
    if (deps.agent == nullptr) return unavailable();
    if (auto r = deps.agent->request_clear(); !r) return agent_error(r.error());
    return net::HttpResponse::json(202, Value(Map{{"pending", Value(true)}}));
}

net::HttpResponse get_message(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.agent == nullptr) return unavailable();
    auto seq = parse_number<uint64_t>(ctx.param);
    if (!seq) return net::HttpResponse::error(404, "not_found", "no such message");
    auto doc = deps.agent->message(*seq);
    if (!doc) return agent_error(doc.error());
    return json_doc(200, std::move(doc.value()));
}

// Collects the body of POST /agent/messages (Content-Length bytes, at most
// kMaxMessageBodyBytes) in PSRAM.
class MessageSink final : public net::BodySink {
public:
    MessageSink(AgentApi& agent, os::LargeBuffer buffer)
        : agent_(agent), buffer_(std::move(buffer)) {}

    std::optional<net::HttpResponse> on_data(std::span<const uint8_t> chunk) override {
        if (chunk.size() > buffer_.size() - received_) {
            buffer_ = os::LargeBuffer();
            return with_no_store(bad_request("body longer than its Content-Length"));
        }
        std::memcpy(buffer_.data() + received_, chunk.data(), chunk.size());
        received_ += chunk.size();
        return std::nullopt;
    }

    net::HttpResponse on_end() override {
        net::HttpResponse res = finish();
        buffer_ = os::LargeBuffer();
        return with_no_store(std::move(res));
    }

    void on_abort() override { buffer_ = os::LargeBuffer(); }

private:
    static net::HttpResponse with_no_store(net::HttpResponse res) {
        res.headers.emplace_back("Cache-Control", "no-store");
        return res;
    }

    net::HttpResponse finish() {
        if (received_ != buffer_.size()) return bad_request("body shorter than its Content-Length");
        auto parsed = from_json(
            std::string_view(reinterpret_cast<const char*>(buffer_.data()), buffer_.size()));
        if (!parsed) {
            return net::HttpResponse::error(400, "bad_json", "request body is not valid JSON");
        }
        return submit(agent_, parsed.value());
    }

    AgentApi& agent_;
    os::LargeBuffer buffer_;
    size_t received_ = 0;
};

}  // namespace

std::optional<net::StreamStart> agent_stream(AdminDeps& deps, const net::HttpRequest& head) {
    if (head.method != net::Method::Post || head.path != kAgentMessagesPath) return std::nullopt;
    if (auto refused = stream_refusal(deps, head)) return stream_answer(std::move(*refused));
    if (deps.agent == nullptr) return stream_answer(unavailable());
    if (head.content_length == 0) {
        return stream_answer(net::HttpResponse::error(411, "length_required",
                                                      "send the message with a Content-Length"));
    }
    if (head.content_length > kMaxMessageBodyBytes) {
        return stream_answer(
            net::HttpResponse::error(413, "payload_too_large", "request body over 16 KiB"));
    }
    os::LargeBuffer buffer = os::LargeBuffer::allocate(static_cast<size_t>(head.content_length));
    if (!buffer.ok()) {
        return stream_answer(
            net::HttpResponse::error(503, "no_memory", "no memory for the message"));
    }
    return net::StreamStart{std::make_unique<MessageSink>(*deps.agent, std::move(buffer)), {}};
}

void add_agent_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/agent/conversation", false, get_conversation});
    routes.push_back({Method::Post, "/agent/messages", false, post_message});
    routes.push_back({Method::Post, "/agent/stop", false, post_stop});
    routes.push_back({Method::Post, "/agent/conversation/new", false, post_new});
    routes.push_back({Method::Get, "/agent/archive", false, get_archives});
    routes.push_back({Method::Get, "/agent/archive/*", false, get_archive});
    routes.push_back({Method::Delete, "/agent/conversations", false, delete_conversations});
    routes.push_back({Method::Get, "/agent/messages/*", false, get_message});
}

}  // namespace wisp::admin::detail
