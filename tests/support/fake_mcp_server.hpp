#pragma once

// net::HttpClient double that speaks just enough MCP over streamable HTTP
// (initialize, notifications/initialized, tools/list, tools/call) for
// net::McpClient's tests, run under a real SceneHttp. It parses each
// request's JSON-RPC body and records the method, id, headers and URL.
//
// Towards the streaming fields it behaves like FakeHttpClient (and the real
// clients): on_status gets the status before the body, on_body gets the body
// in 4 KB pieces (false: Cancelled, or with stop_is_end a success that keeps
// the status and headers), req.cancelled() is asked before each piece, a
// body over max_response_bytes is TooLarge, and `hold` blocks until it is
// cleared or req.watch shuts the "socket" (then `saw_cancel`).
//
// Knobs are read on SceneHttp's worker; set them before the call under test
// (submit()'s queue hands them over), except the atomics.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/request_watch.hpp"

namespace wisp::test {

struct FakeMcpRequest {
    std::string url{};
    std::vector<std::pair<std::string, std::string>> headers{};
    std::string body{};
    std::string method{};         // "" when the body was not a JSON-RPC message
    std::optional<int64_t> id{};  // absent for a notification
    Value params{};

    // The value of header `name` (case-insensitive), nullopt when absent.
    std::optional<std::string> header(std::string_view name) const {
        for (const auto& [k, v] : headers) {
            if (k.size() != name.size()) continue;
            bool same = true;
            for (size_t i = 0; i < k.size() && same; ++i) {
                auto lower = [](char c) {
                    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                };
                same = lower(k[i]) == lower(name[i]);
            }
            if (same) return v;
        }
        return std::nullopt;
    }
};

// What on_call answers: `result` (a JSON object's text) or, when `error` is
// set, a JSON-RPC error object's text.
struct FakeMcpAnswer {
    std::string result{};
    std::string error{};
};

class FakeMcpServer : public net::HttpClient {
public:
    // One tool's JSON for `tools`.
    static std::string tool(std::string_view name, std::string_view description = "",
                            std::string_view schema = R"({"type":"object"})") {
        std::string out = R"({"name":)";
        append_json_string(out, name);
        out += R"(,"description":)";
        append_json_string(out, description);
        if (!schema.empty()) {
            out += R"(,"inputSchema":)";
            out += schema;
        }
        out += "}";
        return out;
    }

    net::HttpClientResponse request(const net::HttpClientRequest& req) override {
        FakeMcpRequest rec;
        rec.url = req.url;
        rec.headers = req.headers;
        rec.body = req.body_ref.empty()
                       ? req.body
                       : std::string(reinterpret_cast<const char*>(req.body_ref.data()),
                                     req.body_ref.size());
        if (auto parsed = from_json(rec.body);
            parsed && parsed.value().type() == Value::Type::Map) {
            const Value& v = parsed.value();
            if (const Value* m = v.find("method"); m && m->type() == Value::Type::String)
                rec.method = m->as_string();
            if (const Value* i = v.find("id"); i && i->type() == Value::Type::Int)
                rec.id = i->as_int();
            if (const Value* p = v.find("params")) rec.params = *p;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(rec);
        }

        if (hold.load()) {
            auto shut = std::make_shared<std::atomic<bool>>(false);
            if (req.watch != nullptr) req.watch->attach([shut] { *shut = true; });
            ++held;
            while (hold.load() && !shut->load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            --held;
            if (req.watch != nullptr) req.watch->disarm();
            if (shut->load()) {
                saw_cancel = true;
                const bool deadline = req.watch->reason() == net::RequestWatch::Reason::Deadline;
                return {.transport = deadline ? net::TransportError::Timeout
                                              : net::TransportError::Cancelled};
            }
        }
        if (transport != net::TransportError::None) return {.transport = transport};
        if (transport_for) {
            if (const auto t = transport_for(rec.url); t != net::TransportError::None)
                return {.transport = t};
        }

        if (redirect_for) {
            if (std::string location = redirect_for(rec.url); !location.empty()) {
                net::HttpClientResponse moved;
                moved.status = 302;
                moved.headers.emplace_back("location", std::move(location));
                return deliver(req, std::move(moved), false);
            }
        }

        net::HttpClientResponse res;
        res.status = 200;
        const bool carries_session = rec.header("Mcp-Session-Id").has_value();
        if (status != 0) {
            res.status = status;
        } else if (carries_session && rec.id.has_value() && expire_sessions.load() > 0) {
            --expire_sessions;
            res.status = 404;
        } else if (!rec.id.has_value()) {
            res.status = 202;  // a notification (or a body that was not JSON-RPC)
        } else {
            std::string body = !raw_body.empty() ? raw_body : answer(rec);
            if (rec.method == "initialize" && !session_id.empty())
                res.headers.emplace_back("mcp-session-id", session_id);
            if (sse) {
                res.headers.emplace_back("content-type", "text/event-stream");
                res.body =
                    "event: message\ndata: "
                    R"({"jsonrpc":"2.0","method":"notifications/message","params":{"level":"info","data":"hi"}})"
                    "\n\n";
                res.body += "event: message\ndata: " + body + "\n\n";
            } else {
                res.headers.emplace_back("content-type", "application/json");
                res.body = std::move(body);
            }
        }
        return deliver(req, std::move(res), sse && keep_open && rec.id.has_value());
    }

    std::vector<FakeMcpRequest> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    std::vector<std::string> methods() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        for (const auto& r : requests_) out.push_back(r.method);
        return out;
    }

    size_t count(std::string_view method) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<size_t>(std::count_if(
            requests_.begin(), requests_.end(), [&](const auto& r) { return r.method == method; }));
    }

    size_t calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

    void clear_requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.clear();
    }

    // The protocol revision initialize answers.
    std::string protocol = "2025-06-18";
    // The mcp-session-id initialize answers; "" for none.
    std::string session_id = "s-1";
    // Answer as text/event-stream, one notifications/message event before
    // the response.
    bool sse = false;
    // With sse: after the response, keep sending ":\n\n" every 1 ms until
    // the reader stops (or the request is cancelled).
    bool keep_open = false;
    // Each tool's JSON (tool()); tools/list pages through them.
    std::vector<std::string> tools{};
    // Tools per tools/list page; 0: all on one page.
    size_t page_size = 0;
    std::function<FakeMcpAnswer(std::string_view name, const Value& args)> on_call;
    // How many requests carrying Mcp-Session-Id answer 404 next
    // (notifications are answered as usual).
    std::atomic<int> expire_sessions{0};
    // Non-zero: every request answers this status with an empty body.
    int status = 0;
    net::TransportError transport = net::TransportError::None;
    // Per URL, after `transport`: None lets the request through (one of
    // several servers failing).
    std::function<net::TransportError(std::string_view url)> transport_for;
    // Per URL, after the transport knobs: a non-empty Location answers 302
    // with it and an empty body.
    std::function<std::string(std::string_view url)> redirect_for;
    std::atomic<bool> hold{false};
    std::atomic<int> held{0};
    std::atomic<bool> saw_cancel{false};
    // Non-empty: every request with an id is answered with this body.
    std::string raw_body{};
    // Added to the id of every response (a response to another request).
    int64_t id_offset = 0;

private:
    std::string envelope(const FakeMcpRequest& rec, std::string_view member,
                         std::string_view json) const {
        std::string out = R"({"jsonrpc":"2.0","id":)";
        out += std::to_string(*rec.id + id_offset);
        out += ",\"";
        out += member;
        out += "\":";
        out += json;
        out += "}";
        return out;
    }

    std::string answer(const FakeMcpRequest& rec) const {
        if (rec.method == "initialize") {
            std::string result = R"({"protocolVersion":)";
            append_json_string(result, protocol);
            result += R"(,"capabilities":{"tools":{}},"serverInfo":{"name":"fake","version":"1"}})";
            return envelope(rec, "result", result);
        }
        if (rec.method == "tools/list") {
            size_t start = 0;
            if (const Value* c = rec.params.find("cursor");
                c != nullptr && c->type() == Value::Type::String)
                start = static_cast<size_t>(std::stoul(c->as_string()));
            const size_t n = page_size == 0 ? tools.size() : page_size;
            const size_t end = std::min(tools.size(), start + n);
            std::string result = R"({"tools":[)";
            for (size_t i = start; i < end; ++i) {
                if (i != start) result += ",";
                result += tools[i];
            }
            result += "]";
            if (end < tools.size()) result += R"(,"nextCursor":")" + std::to_string(end) + "\"";
            result += "}";
            return envelope(rec, "result", result);
        }
        if (rec.method == "tools/call" && on_call) {
            std::string name;
            if (const Value* v = rec.params.find("name"); v && v->type() == Value::Type::String)
                name = v->as_string();
            const Value* args = rec.params.find("arguments");
            FakeMcpAnswer a = on_call(name, args != nullptr ? *args : Value());
            if (!a.error.empty()) return envelope(rec, "error", a.error);
            return envelope(rec, "result", a.result);
        }
        return envelope(rec, "error", R"({"code":-32601,"message":"Method not found"})");
    }

    net::HttpClientResponse deliver(const net::HttpClientRequest& req, net::HttpClientResponse res,
                                    bool keep_open_after) {
        if (req.max_response_bytes != 0 && res.body.size() > req.max_response_bytes)
            return {.transport = net::TransportError::TooLarge};
        if (req.on_status) req.on_status(res.status);
        if (!req.on_body) return res;
        auto stopped = [&]() -> net::HttpClientResponse {
            if (!req.stop_is_end) return {.transport = net::TransportError::Cancelled};
            return {.transport = net::TransportError::None,
                    .status = res.status,
                    .headers = std::move(res.headers)};
        };
        const auto* p = reinterpret_cast<const uint8_t*>(res.body.data());
        for (size_t at = 0; at < res.body.size(); at += 4096) {
            if (req.cancelled && req.cancelled()) {
                saw_cancel = true;
                return {.transport = net::TransportError::Cancelled};
            }
            const size_t n = std::min<size_t>(4096, res.body.size() - at);
            if (!req.on_body({p + at, n})) return stopped();
        }
        if (keep_open_after) {
            static constexpr char kComment[] = ":\n\n";
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (req.cancelled && req.cancelled()) {
                    saw_cancel = true;
                    return {.transport = net::TransportError::Cancelled};
                }
                if (!req.on_body({reinterpret_cast<const uint8_t*>(kComment), 3})) return stopped();
            }
        }
        res.body.clear();
        return res;
    }

    mutable std::mutex mutex_;
    std::vector<FakeMcpRequest> requests_;
};

}  // namespace wisp::test
