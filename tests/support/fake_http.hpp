#pragma once

// Test-only builders for in-memory net::HttpRequest values, for driving
// wisp::admin::AdminServer::handle() without a socket. Header names are
// written lowercase, as every HttpServer implementation delivers them.
// Each helper takes and returns the request by value so calls nest:
//   srv.handle(with_csrf(with_token(make_req(Method::Post, "/x"), t)))

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

#include "wisp/base/error.hpp"
#include "wisp/base/result.hpp"
#include "wisp/net/http.hpp"

namespace wisp::test {

// A request from a browser on the home network (or, with Iface::Ap, on
// the setup hotspot) addressed to the device's IP.
inline net::HttpRequest make_req(net::Method method, std::string path, std::string body = {},
                                 net::Iface iface = net::Iface::Sta) {
    net::HttpRequest req;
    req.method = method;
    req.path = std::move(path);
    req.body = std::move(body);
    req.iface = iface;
    req.remote_ip = iface == net::Iface::Ap ? "192.168.4.2" : "192.168.1.23";
    req.headers["host"] = "192.168.1.50";
    req.headers["user-agent"] =
        "Mozilla/5.0 (Linux; Android 14; Pixel 8) AppleWebKit/537.36 (KHTML, like Gecko) "
        "Chrome/130.0.0.0 Mobile Safari/537.36";
    return req;
}

inline net::HttpRequest host(net::HttpRequest req, std::string value) {
    req.headers["host"] = std::move(value);
    return req;
}

// Appends wisp_token=<token> to the Cookie header, keeping any pairs
// already there.
inline net::HttpRequest with_token(net::HttpRequest req, std::string_view token) {
    std::string& cookie = req.headers["cookie"];
    if (!cookie.empty()) cookie += "; ";
    cookie += "wisp_token=";
    cookie += token;
    return req;
}

inline net::HttpRequest with_csrf(net::HttpRequest req) {
    req.headers["x-wisp"] = "1";
    return req;
}

// Value of the first response header named `name` (case-insensitive), or
// "" when absent.
inline std::string response_header(const net::HttpResponse& res, std::string_view name) {
    auto lower = [](std::string_view s) {
        std::string out(s);
        for (char& c : out) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return out;
    };
    const std::string want = lower(name);
    for (const auto& [k, v] : res.headers) {
        if (lower(k) == want) return v;
    }
    return {};
}

inline bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

inline std::string set_cookie(const net::HttpResponse& res) {
    return response_header(res, "set-cookie");
}

// net::HttpClient double: records every request and answers each with
// `next`. Tests set `next` before the call under test and read the
// recorded requests afterwards; the mutex makes the recording safe when
// the call comes from another thread (a route, SceneHttp's worker).
//
// It behaves like the real clients towards the streaming fields: a body
// over max_response_bytes is TooLarge (unless `ignore_limit`), on_body
// gets it in 4 KB pieces (false: Cancelled) and `next.body` is then not
// returned; req.cancelled() is asked before each piece.
//
// With `hold` set, request() blocks like a library call stuck in one read
// on a silent or trickling server: it does not poll req.cancelled(), and
// returns only when `hold` is cleared or its "socket" is shut down
// through req.watch (the stop it attaches). Then `saw_cancel` is set and
// it reports Timeout (the watch's deadline) or Cancelled (an abort).
// `held` counts the calls blocked.
class FakeHttpClient : public net::HttpClient {
public:
    net::HttpClientResponse request(const net::HttpClientRequest& req) override {
        net::HttpClientResponse res;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(req);
            res = next;
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
        if (res.transport != net::TransportError::None) return res;
        if (req.max_response_bytes != 0 && res.body.size() > req.max_response_bytes &&
            !ignore_limit.load())
            return {.transport = net::TransportError::TooLarge};
        if (req.on_body) {
            const auto* p = reinterpret_cast<const uint8_t*>(res.body.data());
            for (size_t at = 0; at < res.body.size(); at += 4096) {
                if (before_piece) before_piece();
                if (req.cancelled && req.cancelled()) {
                    saw_cancel = true;
                    return {.transport = net::TransportError::Cancelled};
                }
                ++pieces;
                const size_t n = std::min<size_t>(4096, res.body.size() - at);
                if (!req.on_body({p + at, n})) return {.transport = net::TransportError::Cancelled};
            }
            res.body.clear();
        }
        return res;
    }

    std::optional<net::HttpClientRequest> last() const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (requests_.empty()) return std::nullopt;
        return requests_.back();
    }

    size_t calls() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

    // The body a recorded request carried (body_ref when set, else body).
    static std::string body_of(const net::HttpClientRequest& req) {
        if (req.body_ref.empty()) return req.body;
        return {reinterpret_cast<const char*>(req.body_ref.data()), req.body_ref.size()};
    }

    net::HttpClientResponse next{.transport = net::TransportError::None, .status = 200, .body = {}};
    std::atomic<bool> hold{false};
    std::atomic<int> held{0};
    std::atomic<bool> saw_cancel{false};
    std::atomic<bool> ignore_limit{false};
    // Called before each 4 KB piece goes to on_body (a server that trickles
    // the body: a test advances its clock here). Set before the request.
    std::function<void()> before_piece;
    std::atomic<int> pieces{0};

private:
    mutable std::mutex mutex_;
    std::vector<net::HttpClientRequest> requests_;
};

// net::HttpServer double: start() succeeds (unless `fail_start` is set) and
// only records what it was given. The counters and running() are atomics
// because App drives the server from its "wisp.net" task while the test
// thread polls them.
class FakeHttpServer : public net::HttpServer {
public:
    Result<void> start(uint16_t port, net::HttpHandler handler, net::WsHandlers ws,
                       net::StreamHandler stream = {}) override {
        ++start_calls;
        if (fail_start.load()) {
            return Result<void>::err(ErrorCode::IoError, "fake start failure");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            handler_ = std::move(handler);
            ws_ = std::move(ws);
            stream_ = std::move(stream);
        }
        port_.store(port == 0 ? 4242 : port);
        running_.store(true);
        return Result<void>::ok();
    }

    void stop() override {
        ++stop_calls;
        running_.store(false);
        port_.store(0);
        std::lock_guard<std::mutex> lock(mutex_);
        handler_ = nullptr;
        ws_ = {};
        stream_ = nullptr;
    }

    bool running() const override { return running_.load(); }
    uint16_t port() const override { return port_.load(); }

    // Calls the handler start() received, as a server thread would; a 503
    // when not running.
    net::HttpResponse handle(const net::HttpRequest& req) {
        net::HttpHandler h;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            h = handler_;
        }
        if (!h) return net::HttpResponse::error(503, "unavailable", "not running");
        return h(req);
    }

    // The StreamHandler start() received (empty when none or not running).
    net::StreamHandler stream_handler() {
        std::lock_guard<std::mutex> lock(mutex_);
        return stream_;
    }

    std::atomic<int> start_calls{0};
    std::atomic<int> stop_calls{0};
    std::atomic<bool> fail_start{false};

private:
    mutable std::mutex mutex_;
    net::HttpHandler handler_;
    net::WsHandlers ws_;
    net::StreamHandler stream_;
    std::atomic<bool> running_{false};
    std::atomic<uint16_t> port_{0};
};

}  // namespace wisp::test
