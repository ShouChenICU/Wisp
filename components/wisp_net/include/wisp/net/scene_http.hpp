#pragma once

// SceneHttp: the scenes' outbound HTTP proxy (scene-runtime-core spec 5.3).
// wisp.http is a thin adapter over it; it holds the rules, not the binding.
//
// Rules, checked in this order when a request is submitted:
//   - the URL must start with https:// (else "not_https");
//   - its host, lowercased, must equal one of the hosts the scene declared
//     in scene.json (permissions.http) exactly: no wildcards, no
//     subdomains, no user info, only [a-z0-9.-] (else "permission_denied");
//   - at most kMaxHeaders request headers, each passing header_allowed()
//     (else "permission_denied");
//   - URL at most kMaxUrlBytes, each header's name and value together at
//     most kMaxHeaderBytes and all of them at most kMaxHeadersTotal, the
//     body at most kMaxRequestBody (else "too_large");
//   - at most SceneHttpLimits::per_minute accepted requests per scene in
//     any 60 s window, and room in the queue (else "rate_limited"). The
//     window is kept per scene id, so closing and reopening a scene does
//     not reset it;
//   - while deferring (set_deferring(true), deep standby with Wi-Fi off,
//     scene-device spec 4.4), room in the scene's deferred list of
//     max_deferred_per_scene (else "deferred_queue_full") instead of room
//     in the queue.
//
// Deferred requests wait apart from the run queue, in the order they were
// submitted; set_deferring(false) has the worker move them into the run
// queue as it has room, oldest first, and their results carry
// `deferred = true`. set_deferring(true) moves the run queue's requests
// that have not started back to the front of the deferred list (the one
// in flight finishes as it is). While deferred requests are draining, a
// new one lines up behind them -- in the deferred list, capped per scene
// at max_deferred_per_scene (else "rate_limited") -- and is not marked
// deferred unless deferring starts again before it runs. One that has
// waited longer than deferred_timeout_ms fails with "deferred_timeout"
// (the worker looks at least every deferred_check_ms while any wait).
// Accepted requests run one at a time on the "wisp.shttp" task, each with
// one deadline of SceneHttpLimits::timeout_ms for all of it (waiting for
// the TLS gate, connect, handshake, sending and every read; "timeout"
// when it passes), no redirects (a 3xx goes back as it is) and
// a response body cap of SceneHttpLimits::max_response bytes ("too_large"
// past it). The body is collected in PSRAM. Transport failures come back
// as "dns", "connect", "tls", "timeout" or "other".
//
// done: every submit() gets exactly one done() call -- immediate
// rejections from inside submit(), the rest from the worker task --
// unless cancel_scene() (or stop()) comes first: once it returns, done()
// is never called for that scene's earlier requests, queued ones are
// dropped and the one in flight is told to stop (HttpClientRequest::
// cancelled, and the worker's RequestWatch is aborted, which interrupts a
// blocked call from the watch's timer task: cancel_scene() never waits
// for that). done() must be quick and must not call back into SceneHttp:
// the worker runs it under the lock cancel_scene() waits on.
//
// Memory: a queued request keeps its URL, headers and body in PSRAM; only
// the request being sent has ordinary heap copies of the URL and headers
// (bounded by the limits above), so the queue costs no internal RAM.
//
// Flash: the worker task never touches flash (task_config():
// flash_access = false): no Settings, no files; the HTTPS certificate
// bundle is a constant linked into the image, read through the cache.
// done() runs on that task too and must keep to the same rule.
//
// Privacy: request bodies, header values and anything past the URL's host
// are never logged. A rejection logs the scene, the code and the host
// (tag "scene.<id>"); transport failures are logged by the HttpClient,
// which keeps to the same rule.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/request_watch.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::net {

struct SceneHttpLimits {
    size_t max_response = 256 * 1024;
    uint32_t per_minute = 30;
    uint32_t timeout_ms = 15000;
    size_t queue = 8;
    size_t max_deferred_per_scene = 4;
    uint64_t deferred_timeout_ms = 3600000;
    // How often the worker wakes to time out deferred requests while any
    // wait.
    uint32_t deferred_check_ms = 60000;
};

struct SceneHttpRequest {
    std::string scene_id;
    uint32_t instance = 0;
    Method method = Method::Get;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    // In PSRAM from the start: it can be up to kMaxRequestBody bytes and
    // waits in the queue.
    os::LargeString body;
};

struct SceneHttpResult {
    // "" or permission_denied not_https too_large rate_limited
    // deferred_queue_full deferred_timeout, or a transport error: dns
    // connect tls timeout other.
    std::string error;
    int status = 0;
    // Lowercase names (keep_response_header()'s limits).
    std::vector<std::pair<std::string, std::string>> headers;
    os::LargeBuffer body;
    // The request waited in the deferred list (deep standby).
    bool deferred = false;
};

class SceneHttp {
public:
    static constexpr size_t kMaxHeaders = 16;
    // A header line must fit the device client's 2 KB send buffer.
    static constexpr size_t kMaxHeaderBytes = 1024;
    static constexpr size_t kMaxHeadersTotal = 4096;
    static constexpr size_t kMaxUrlBytes = 2048;
    static constexpr size_t kMaxRequestBody = 64 * 1024;
    static constexpr uint64_t kWindowMs = 60000;

    SceneHttp(HttpClient& client, SceneHttpLimits limits = {},
              std::function<uint64_t()> now_ms = os::now_ms);
    ~SceneHttp();  // stop()

    SceneHttp(const SceneHttp&) = delete;
    SceneHttp& operator=(const SceneHttp&) = delete;

    // "wisp.shttp", 8192 bytes, priority 3, unpinned, flash_access =
    // false (see the file header), so the port may put its stack in PSRAM.
    static os::TaskConfig task_config();

    // Makes the request watch and spawns the worker. Busy if started
    // before, NoMemory if the watch's timer or the task cannot be made
    // (the proxy then stays off).
    Result<void> start();
    // Drops every queued request and stops the one in flight, all without
    // done(), and joins the worker. Idempotent.
    void stop();

    // The hosts `scene_id` declared (lowercase, as the manifest holds
    // them); replaces any earlier list.
    void set_hosts(const std::string& scene_id, std::vector<std::string> hosts);
    // Checks `req` and queues it. Rejections (and a stopped proxy:
    // "other") call done() before this returns.
    void submit(SceneHttpRequest req, std::function<void(SceneHttpResult)> done);
    // See the file header. Safe from any thread but done() itself; also
    // drops the scene's deferred requests.
    void cancel_scene(const std::string& scene_id);

    // On: accepted requests wait in the deferred list, and queued ones
    // that have not started go back to it (see the file header). Off: the
    // worker moves the waiting ones into the run queue in submission order
    // (those that do not fit stay until there is room). Any thread.
    void set_deferring(bool on);
    // Requests waiting in the deferred list.
    size_t deferred_count() const;
    // Nothing deferred, queued or in flight.
    bool idle() const;

    // "" when `url` may be fetched with `hosts` declared, else
    // "not_https" or "permission_denied".
    static std::string check_url(std::string_view url, const std::vector<std::string>& hosts);
    // False for a malformed name (empty, not an RFC 9110 token), a value
    // with CR, LF or NUL, and the names a scene may not set: cookie,
    // cookie2, proxy-authorization, host, connection, content-length,
    // transfer-encoding, upgrade, te, trailer, keep-alive, expect, x-wisp,
    // and any proxy-* or sec-* (case-insensitive). Authorization and
    // other ordinary headers pass.
    static bool header_allowed(std::string_view name, std::string_view value);

private:
    struct Job;
    // A std::deque allocates a block as soon as it is built, even empty,
    // and malloc would put it in internal RAM.
    template <class T>
    using Deque = std::deque<T, os::LargeAllocator<T>>;
    struct SceneState {
        std::vector<std::string> hosts;
        Deque<uint64_t> sent;  // when each request in the window was accepted
        size_t deferred = 0;   // its requests in deferred_
    };

    void run();
    // Moves deferred requests that waited too long into `out`. With mu_.
    void expire_locked(uint64_t now, Deque<os::LargeUnique<Job>>& out);
    // Moves deferred requests into the run queue while it has room and
    // deferring is off. With mu_.
    void promote_locked();
    void execute(Job& job);
    // Rejects without a request: logs and calls done().
    static void reject(const SceneHttpRequest& req, const char* code,
                       const std::function<void(SceneHttpResult)>& done);

    HttpClient& client_;
    SceneHttpLimits limits_;
    std::function<uint64_t()> now_ms_;
    std::unique_ptr<os::Task> task_;
    // The worker's one watch, for the request in flight.
    os::LargeUnique<RequestWatch> watch_;
    os::Semaphore wake_;
    // Guards scenes_, queue_, deferred_, current_ and the flags below.
    mutable os::Mutex mu_;
    // Held while the worker decides whether to call done() and calls it;
    // cancel_scene() takes it after marking, so no done() for a cancelled
    // request can still be running once it returns.
    os::Mutex deliver_mu_;
    std::map<std::string, SceneState, std::less<>> scenes_;
    Deque<os::LargeUnique<Job>> queue_;
    // Every scene's deferred requests, oldest first.
    Deque<os::LargeUnique<Job>> deferred_;
    // The worker's: deferred requests that timed out, being answered.
    // Under deliver_mu_; a member so the worker's loop allocates nothing.
    Deque<os::LargeUnique<Job>> expired_;
    bool deferring_ = false;
    Job* current_ = nullptr;
    bool started_ = false;
    bool running_ = false;
};

}  // namespace wisp::net
