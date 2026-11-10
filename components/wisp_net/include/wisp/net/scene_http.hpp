#pragma once

// SceneHttp: the scenes' outbound HTTP proxy (scene-runtime-core spec 5.3),
// now shared with the agent's own http.fetch tool (agent-tools spec 3.2).
// wisp.http (and, for the agent, the http.fetch tool) are thin adapters
// over it; it holds the rules, not the binding.
//
// Two principals submit requests, told apart by SceneHttpRequest::principal:
//   - Scene (the default): the rules below, unchanged. max_redirects is
//     ignored (redirects are an Agent-only feature): a 3xx always comes
//     back as it is.
//   - Agent: the device's own agent core. Its requests skip the https-only,
//     host-allowlist and per-scene rate limit checks, and header_allowed()'s
//     policy (allowlist) half, but not its syntax half: a header name that
//     is not an RFC 9110 token, or a value with CR, LF or NUL, is still
//     "bad_header" (the agent is trusted code, not a sandboxed scene, but a
//     malformed header could still inject a second one past it). The URL
//     just needs a http:// or https:// scheme and a parseable host (no
//     IPv6 literals; else "bad_url"), and Host/Content-Length/
//     Transfer-Encoding/Connection headers (any case) are dropped rather
//     than forwarded or rejected. Its size limits are AgentHttpLimits,
//     always, not SceneHttpLimits; it also bypasses limits_.queue (an agent
//     call waits for its own result before making another, so this never
//     grows unbounded in practice). It is never rate limited or deferred:
//     while deferring (Wi-Fi going off) it fails at once with "offline"
//     instead of waiting. Its requests are recorded under the scene id
//     kAgentHttpId ("@agent", which no real scene id can collide with);
//     cancel_scene(kAgentHttpId) cancels them. A response past its cap is
//     truncated (SceneHttpResult::body_truncated = true, error ""), not an
//     error -- the request is sent with HttpClientRequest::
//     truncate_at_limit, so the client itself stops at the cap instead of
//     failing it; a 3xx body is truncated exactly the same way if huge,
//     and (being a redirect's own body, not what was asked for) is then
//     just not followed.
//
//     The worker follows redirects for an Agent request up to
//     max_redirects hops, one deadline for the whole chain (the watch is
//     re-armed, with the time left, before every later hop, so
//     cancellation and the deadline both still reach it): 301/302/307/308
//     keep the method and body, 303 switches to a bodyless GET. Location
//     is resolved against the request URL as an absolute URL, a
//     scheme-relative "//host/path", an absolute path, a query ("?a=1",
//     keeping the current path) or a relative path (merged with the
//     current path's directory, with "." and ".." segments collapsed);
//     any other form is not understood. Every hop's resolved target is
//     re-checked exactly as a fresh URL would be (agent_url_error(),
//     AgentHttpLimits::kMaxUrlBytes); a hop that does not resolve, or
//     resolves to something invalid or past the limit, ends the chain
//     with that 3xx returned as it is, not followed. A hop that crosses
//     to a different scheme, host or port drops Authorization, Cookie and
//     Proxy-Authorization (case-insensitive) from then on, so a
//     credential for the first host is never sent to another one.
//     final_url is the URL that was actually answered.
//
//     Privacy: besides the rule below, every Agent request logs one line
//     with its method, host, final status (0 on a transport failure) and
//     elapsed time, and nothing else (agent-tools spec 3.2).
//
// Rules for the Scene principal, checked in this order when a request is
// submitted:
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
// past it). The body is collected in PSRAM. A download
// (SceneHttpRequest::on_body, wisp.http.download) streams its body to
// on_body instead (only a 2xx body; others are dropped), without that
// cap, under a deadline of
// kDownloadTimeoutMs; it takes the one worker for as long as it runs, so
// the scenes' other requests wait behind it. Transport failures come back
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
#include <span>
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

// Which code submitted a request (see the file header).
enum class HttpPrincipal : uint8_t { Scene, Agent };

// The scene id the Agent principal's requests are recorded under; no real
// scene id contains '@', so this can never collide with one.
inline constexpr const char* kAgentHttpId = "@agent";

// The Agent principal's size and redirect limits (agent-tools spec 3.1),
// used instead of SceneHttpLimits regardless of what SceneHttp was built
// with.
struct AgentHttpLimits {
    static constexpr size_t kMaxUrlBytes = 8192;
    static constexpr size_t kMaxHeaders = 32;
    static constexpr size_t kMaxHeaderBytes = 4096;
    static constexpr size_t kMaxHeadersTotal = 16384;
    static constexpr size_t kMaxRequestBody = 256 * 1024;
    static constexpr size_t kMaxResponse = 1024 * 1024;
    static constexpr uint32_t kMaxTimeoutMs = 120000;
    static constexpr uint8_t kMaxRedirects = 5;
};

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
    // A download's deadline for all of it (scene-device spec 7.1): five
    // minutes instead of SceneHttpLimits::timeout_ms.
    static constexpr uint32_t kDownloadTimeoutMs = 300000;
    // The body cap of a download that sets no max_body.
    static constexpr uint64_t kMaxDownloadBody = 1ull << 30;

    std::string scene_id;
    uint32_t instance = 0;
    HttpPrincipal principal = HttpPrincipal::Scene;
    Method method = Method::Get;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    // 0 = use SceneHttpLimits (Agent: AgentHttpLimits) instead. max_redirects
    // 0 means do not follow (a 3xx comes back as it is, the Scene
    // principal's unchanged default).
    size_t max_response = 0;
    uint32_t timeout_ms = 0;
    uint8_t max_redirects = 0;
    // In PSRAM from the start: it can be up to kMaxRequestBody bytes and
    // waits in the queue.
    os::LargeString body;
    // Set: the response body is streamed to it piece by piece on the
    // worker task (which must not touch flash) instead of being collected,
    // so no max_response applies and SceneHttpResult::body stays empty.
    // false stops the transfer ("other"). Only a 2xx answer's body reaches
    // it: any other body is read and dropped (at most max_response bytes
    // of it; past that the transfer stops, and the result carries just
    // the status, without headers).
    std::function<bool(std::span<const uint8_t>)> on_body;
    // With on_body: a body over this many bytes fails with "too_large";
    // 0 is kMaxDownloadBody.
    uint64_t max_body = 0;
    // The request's deadline is kDownloadTimeoutMs.
    bool download = false;
};

struct SceneHttpResult {
    // "" or permission_denied not_https too_large rate_limited
    // deferred_queue_full deferred_timeout bad_url bad_header offline, or a
    // transport error: dns connect tls timeout other.
    std::string error;
    int status = 0;
    // Lowercase names (keep_response_header()'s limits).
    std::vector<std::pair<std::string, std::string>> headers;
    os::LargeBuffer body;
    // The request waited in the deferred list (deep standby).
    bool deferred = false;
    // The URL that was actually answered: the request URL, or, once
    // redirects were followed, the last one.
    std::string final_url;
    // The Agent principal only: the response was cut off at its cap, kept
    // (not an error) up to that many bytes.
    bool body_truncated = false;
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
    // other ordinary headers pass. header_allowed() is this and
    // header_syntax_ok() together.
    static bool header_allowed(std::string_view name, std::string_view value);
    // The syntax half of header_allowed(), with none of its policy: false
    // for a malformed name or a value with CR, LF or NUL. What the Agent
    // principal is held to (else "bad_header"): CRLF injection is refused
    // even for trusted code, but none of the allowlist is.
    static bool header_syntax_ok(std::string_view name, std::string_view value);

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
    // The request's whole deadline.
    uint32_t timeout_of(const Job& job) const;
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
