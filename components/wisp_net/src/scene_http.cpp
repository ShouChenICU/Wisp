#include "wisp/net/scene_http.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "wisp/core/log.hpp"
#include "wisp/net/request_watch.hpp"

namespace wisp::net {

namespace {

constexpr const char* kTag = "scene_http";
// Where the response body starts before it grows (doubling) towards the cap.
constexpr size_t kBodyStartBytes = 4096;

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

bool istarts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && iequals(s.substr(0, prefix.size()), prefix);
}

// RFC 9110 tchar.
bool is_tchar(char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
    return std::strchr("!#$%&'*+-.^_`|~", c) != nullptr && c != '\0';
}

// The URL's host for logs: lowercase, only host characters, cut short.
// Never the path or query, which can carry secrets.
std::string host_for_log(std::string_view url) {
    const size_t scheme = url.find("://");
    std::string_view rest = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
    std::string out;
    for (char c : rest) {
        c = lower(c);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) break;
        if (out.size() == 64) break;
        out.push_back(c);
    }
    return out.empty() ? std::string("?") : out;
}

std::string scene_tag(std::string_view id) {
    std::string tag = "scene.";
    tag.append(id);
    return tag;
}

// For the Agent principal's one log line per request (file header,
// agent-tools spec 3.2): never the URL past the host.
const char* method_name(Method m) {
    switch (m) {
        case Method::Get:
            return "GET";
        case Method::Post:
            return "POST";
        case Method::Put:
            return "PUT";
        case Method::Delete:
            return "DELETE";
        case Method::Patch:
            return "PATCH";
        case Method::Head:
            return "HEAD";
        case Method::Other:
            break;
    }
    return "?";
}

bool is_hop_by_hop_header(std::string_view name) {
    static constexpr std::string_view kHopByHop[] = {"host", "content-length", "transfer-encoding",
                                                     "connection"};
    for (std::string_view h : kHopByHop)
        if (iequals(name, h)) return true;
    return false;
}

// The Agent principal's URL check (file header): http:// or https://, a
// parseable host, nothing a client could read as a line break or a second
// token. No host allowlist, no https-only rule.
std::string agent_url_error(std::string_view url) {
    std::string_view rest;
    if (istarts_with(url, "https://"))
        rest = url.substr(8);
    else if (istarts_with(url, "http://"))
        rest = url.substr(7);
    else
        return "bad_url";
    for (char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f || c == '\\') return "bad_url";
    }
    std::string_view authority = rest.substr(0, rest.find_first_of("/?#"));
    if (authority.find('@') != std::string_view::npos) return "bad_url";
    std::string_view host = authority;
    if (size_t colon = authority.find(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        std::string_view port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5) return "bad_url";
        uint32_t n = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return "bad_url";
            n = n * 10 + static_cast<uint32_t>(c - '0');
        }
        if (n == 0 || n > 65535) return "bad_url";
    }
    if (host.empty()) return "bad_url";
    for (char c : host) {
        c = lower(c);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return "bad_url";
    }
    return "";
}

// Splits a path-and-tail string (a Location header's absolute path, or a
// relative path still carrying its own query/fragment) at the first '?' or
// '#'. The tail is returned untouched -- it is never a path and
// remove_dot_segments must never look inside it (an app's own query value
// can itself look like a path, "/cb?u=https://a.com/x").
std::pair<std::string_view, std::string_view> split_path_tail(std::string_view s) {
    const size_t p = s.find_first_of("?#");
    if (p == std::string_view::npos) return {s, {}};
    return {s.substr(0, p), s.substr(p)};
}

// Collapses "." and ".." path segments (RFC 3986 5.2.4): `path` always
// starts with '/' and holds only the path -- no query or fragment (callers
// split those off with split_path_tail() first and reattach the tail
// verbatim). Segments are not otherwise touched: an empty segment ("//")
// survives, and a trailing "." or ".." leaves a trailing "/" (RFC 3986's
// own worked example, "/a/b/.." -> "/a/").
std::string normalize_dot_segments(std::string_view path) {
    std::vector<std::string_view> out;
    bool trailing_slash = false;
    size_t i = 1;
    while (true) {
        const size_t next = path.find('/', i);
        const std::string_view seg =
            next == std::string_view::npos ? path.substr(i) : path.substr(i, next - i);
        if (seg == ".") {
            trailing_slash = true;
        } else if (seg == "..") {
            if (!out.empty()) out.pop_back();
            trailing_slash = true;
        } else {
            out.push_back(seg);
            trailing_slash = false;
        }
        if (next == std::string_view::npos) break;
        i = next + 1;
    }
    std::string result = "/";
    for (size_t k = 0; k < out.size(); ++k) {
        result.append(out[k]);
        if (k + 1 < out.size()) result.push_back('/');
    }
    if (trailing_slash && !out.empty()) result.push_back('/');
    return result;
}

// Resolves a Location header against the request URL: an absolute URL, a
// scheme-relative "//host/path" (the base's own scheme), an absolute path,
// a query-only "?a=1" (the base's own path) or a relative path (merged
// with the base path's directory, "." and ".." collapsed). "" for
// anything else (a bare fragment, or a base with no scheme), meaning the
// redirect is not followed.
std::string resolve_location(const std::string& base, const std::string& location) {
    if (location.empty() || location[0] == '#') return {};
    if (istarts_with(location, "http://") || istarts_with(location, "https://")) return location;
    // Any other "scheme://..." is an absolute URL too, just in a scheme
    // the Agent principal does not support: returned as it is, so the
    // caller's own URL check rejects it (and the hop is then not
    // followed) instead of this merging it into the path as if it were a
    // relative reference.
    if (const size_t colon = location.find(':');
        colon != std::string::npos && colon > 0 && location.compare(colon, 3, "://") == 0) {
        bool scheme_ok = true;
        for (size_t i = 0; scheme_ok && i < colon; ++i) {
            const char c = location[i];
            scheme_ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
        }
        if (scheme_ok) return location;
    }
    const size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) return {};
    if (location.size() >= 2 && location[0] == '/' && location[1] == '/')
        return base.substr(0, scheme_end) + ":" + location;

    const size_t authority_start = scheme_end + 3;
    const size_t path_start = base.find_first_of("/?#", authority_start);
    const std::string origin = path_start == std::string::npos ? base : base.substr(0, path_start);
    const std::string_view rest = path_start == std::string::npos
                                      ? std::string_view()
                                      : std::string_view(base).substr(path_start);
    const size_t q = rest.find_first_of("?#");
    std::string_view base_path = q == std::string_view::npos ? rest : rest.substr(0, q);
    if (base_path.empty()) base_path = "/";

    if (location[0] == '/') {
        const auto [loc_path, loc_tail] = split_path_tail(location);
        return origin + normalize_dot_segments(loc_path) + std::string(loc_tail);
    }
    if (location[0] == '?') return origin + std::string(base_path) + location;
    const size_t slash = base_path.rfind('/');
    const std::string dir =
        slash == std::string_view::npos ? "/" : std::string(base_path.substr(0, slash + 1));
    const auto [loc_path, loc_tail] = split_path_tail(location);
    return origin + normalize_dot_segments(dir + std::string(loc_path)) + std::string(loc_tail);
}

// The scheme and host:port a redirect hop is at, lowercased, for deciding
// whether it crossed origins (file header): everything past the URL's
// authority is irrelevant to that, and is dropped.
std::string origin_key(std::string_view url) {
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) return std::string(url);
    const size_t authority_start = scheme_end + 3;
    const size_t authority_end = url.find_first_of("/?#", authority_start);
    std::string key(
        url.substr(0, authority_end == std::string_view::npos ? url.size() : authority_end));
    for (char& c : key) c = lower(c);
    return key;
}

// Authorization, Cookie and Proxy-Authorization: dropped from a redirect
// hop that crosses to a different scheme, host or port (file header).
void drop_cross_origin_credentials(std::vector<std::pair<std::string, std::string>>& headers) {
    static constexpr std::string_view kCredentials[] = {"authorization", "cookie",
                                                        "proxy-authorization"};
    headers.erase(std::remove_if(headers.begin(), headers.end(),
                                 [](const auto& h) {
                                     for (std::string_view c : kCredentials)
                                         if (iequals(h.first, c)) return true;
                                     return false;
                                 }),
                  headers.end());
}

// A response body growing in Mem::Large up to `max` bytes.
class BodyBuffer {
public:
    explicit BodyBuffer(size_t max) : max_(max) {}
    ~BodyBuffer() { os::dealloc(data_); }
    BodyBuffer(const BodyBuffer&) = delete;
    BodyBuffer& operator=(const BodyBuffer&) = delete;

    enum class Add { Ok, TooLarge, NoMemory };

    size_t remaining() const { return max_ - size_; }

    // Keeps the allocation, drops the content: a fresh buffer for the next
    // hop of a redirect chain without reallocating.
    void reset() { size_ = 0; }

    Add add(std::span<const uint8_t> chunk) {
        if (chunk.size() > max_ - size_) return Add::TooLarge;
        if (size_ + chunk.size() > cap_) {
            size_t cap = std::max(cap_ * 2, kBodyStartBytes);
            while (cap < size_ + chunk.size()) cap *= 2;
            cap = std::min(cap, max_);
            void* p = os::realloc(data_, cap, os::Mem::Large);
            if (p == nullptr) return Add::NoMemory;
            data_ = static_cast<uint8_t*>(p);
            cap_ = cap;
        }
        if (!chunk.empty()) std::memcpy(data_ + size_, chunk.data(), chunk.size());
        size_ += chunk.size();
        return Add::Ok;
    }

    // The body in a buffer of its own size (so a result waiting in a queue
    // holds no slack); !ok() if that cannot be allocated.
    os::LargeBuffer take() {
        os::LargeBuffer out = os::LargeBuffer::allocate(size_);
        if (out.ok() && size_ != 0) std::memcpy(out.data(), data_, size_);
        return out;
    }

private:
    size_t max_;
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t cap_ = 0;
};

}  // namespace

// A queued request, in PSRAM with its strings (os::make_large).
struct SceneHttp::Job {
    std::string scene_id;
    uint32_t instance = 0;
    HttpPrincipal principal = HttpPrincipal::Scene;
    Method method = Method::Get;
    os::LargeString url;
    os::LargeVector<std::pair<os::LargeString, os::LargeString>> headers;
    os::LargeString body;
    std::function<bool(std::span<const uint8_t>)> on_body;
    uint64_t max_body = 0;
    bool download = false;
    size_t max_response = 0;
    uint32_t timeout_ms = 0;
    uint8_t max_redirects = 0;
    std::function<void(SceneHttpResult)> done;
    // Set under mu_ by cancel_scene()/stop(); read by the client between
    // pieces and by the worker under deliver_mu_.
    std::atomic<bool> cancelled{false};
    // Its ticket for its lane's watch, set under mu_ when the worker takes
    // it.
    uint32_t ticket = 0;
    // When it was accepted, across both lanes: set_deferring(true) puts
    // both lanes' waiting jobs back in this order. Under mu_.
    uint64_t seq = 0;
    // It went through deferred_; when it was put there.
    bool deferred = false;
    uint64_t deferred_at = 0;
};

SceneHttp::SceneHttp(HttpClient& client, SceneHttpLimits limits, std::function<uint64_t()> now_ms)
    : client_(client), limits_(limits), now_ms_(std::move(now_ms)) {}

SceneHttp::~SceneHttp() { stop(); }

uint32_t SceneHttp::timeout_of(const Job& job) const {
    if (job.download) return SceneHttpRequest::kDownloadTimeoutMs;
    uint32_t t = job.timeout_ms != 0 ? job.timeout_ms : limits_.timeout_ms;
    if (job.principal == HttpPrincipal::Agent) t = std::min(t, AgentHttpLimits::kMaxTimeoutMs);
    return t;
}

HttpLane SceneHttp::lane_of(const Job& job) {
    return job.download ? HttpLane::Download : HttpLane::Interactive;
}

size_t SceneHttp::queue_cap(HttpLane l) const {
    return l == HttpLane::Download ? limits_.download_queue : limits_.queue;
}

os::TaskConfig SceneHttp::task_config(HttpLane lane) {
    os::TaskConfig t;
    t.name = lane == HttpLane::Download ? "wisp.shttp.dl" : "wisp.shttp";
    t.stack_bytes = 8192;
    t.priority = 3;
    t.core = -1;
    t.flash_access = false;
    return t;
}

Result<void> SceneHttp::start() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (started_) return Result<void>::err(ErrorCode::Busy, "scene HTTP already started");
        started_ = true;
    }
    // Made once per lane and reused by every request, so a request
    // allocates no timer; without them the proxy stays off (submit()
    // answers "other").
    for (Lane& l : lanes_) {
        l.watch = RequestWatch::create();
        if (!l.watch)
            return Result<void>::err(ErrorCode::NoMemory, "cannot make the scene HTTP watch");
    }
    {
        std::lock_guard<os::Mutex> lock(mu_);
        running_ = true;
    }
    for (HttpLane l : {HttpLane::Interactive, HttpLane::Download}) {
        lane(l).task = os::Task::spawn(task_config(l), [this, l] { run(l); });
        if (!lane(l).task) {
            // Joins the one already running; the proxy stays off.
            stop();
            return Result<void>::err(ErrorCode::NoMemory, "cannot start a scene HTTP task");
        }
    }
    return Result<void>::ok();
}

void SceneHttp::stop() {
    Deque<os::LargeUnique<Job>> dropped;
    std::array<uint32_t, 2> tickets{};
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!running_) return;
        running_ = false;
        for (size_t i = 0; i < lanes_.size(); ++i) {
            Lane& l = lanes_[i];
            for (auto& job : l.queue) dropped.push_back(std::move(job));
            l.queue.clear();
            if (l.current != nullptr) {
                l.current->cancelled = true;
                tickets[i] = l.current->ticket;
            }
        }
        for (auto& job : deferred_) dropped.push_back(std::move(job));
        deferred_.clear();
        for (auto& [id, st] : scenes_) st.deferred = 0;
    }
    for (size_t i = 0; i < lanes_.size(); ++i) {
        Lane& l = lanes_[i];
        if (tickets[i] != 0) l.watch->abort(tickets[i]);
        l.wake.give();
    }
    for (Lane& l : lanes_) {
        if (l.task) {
            l.task->join();
            l.task.reset();
        }
    }
}

void SceneHttp::set_hosts(const std::string& scene_id, std::vector<std::string> hosts) {
    std::lock_guard<os::Mutex> lock(mu_);
    scenes_[scene_id].hosts = std::move(hosts);
}

void SceneHttp::reject(const SceneHttpRequest& req, const char* code,
                       const std::function<void(SceneHttpResult)>& done) {
    WLOG_W(scene_tag(req.scene_id).c_str(), "http: %s (host %s)", code,
           host_for_log(req.url).c_str());
    SceneHttpResult r;
    r.error = code;
    r.final_url = req.url;
    if (done) done(std::move(r));
}

void SceneHttp::submit(SceneHttpRequest req, std::function<void(SceneHttpResult)> done) {
    const bool is_agent = req.principal == HttpPrincipal::Agent;
    if (is_agent) req.scene_id = kAgentHttpId;

    // The order of the checks is the one the header documents.
    const char* code = nullptr;
    if (is_agent) {
        std::string url_code = agent_url_error(req.url);
        if (!url_code.empty()) {
            reject(req, "bad_url", done);
            return;
        }
        // Hop-by-hop headers are dropped, not rejected and not forwarded
        // (the client sets them itself); the rest skip header_allowed()'s
        // policy but not its syntax (header_syntax_ok(), file header).
        std::vector<std::pair<std::string, std::string>> kept;
        kept.reserve(req.headers.size());
        for (auto& [name, value] : req.headers)
            if (!is_hop_by_hop_header(name)) kept.emplace_back(std::move(name), std::move(value));
        req.headers = std::move(kept);
        for (const auto& [name, value] : req.headers)
            if (!header_syntax_ok(name, value)) {
                reject(req, "bad_header", done);
                return;
            }
        if (req.headers.size() > AgentHttpLimits::kMaxHeaders) code = "too_large";
        if (req.url.size() > AgentHttpLimits::kMaxUrlBytes ||
            req.body.size() > AgentHttpLimits::kMaxRequestBody)
            code = "too_large";
        size_t total = 0;
        for (const auto& [name, value] : req.headers) {
            total += name.size() + value.size();
            if (name.size() + value.size() > AgentHttpLimits::kMaxHeaderBytes) code = "too_large";
        }
        if (total > AgentHttpLimits::kMaxHeadersTotal) code = "too_large";
    } else {
        std::vector<std::string> hosts;
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if (auto it = scenes_.find(req.scene_id); it != scenes_.end()) hosts = it->second.hosts;
        }
        std::string url_code = check_url(req.url, hosts);
        if (!url_code.empty()) {
            reject(req, url_code == "not_https" ? "not_https" : "permission_denied", done);
            return;
        }
        if (req.headers.size() > kMaxHeaders) code = "permission_denied";
        for (const auto& [name, value] : req.headers)
            if (code == nullptr && !header_allowed(name, value)) code = "permission_denied";
        if (code == nullptr) {
            if (req.url.size() > kMaxUrlBytes || req.body.size() > kMaxRequestBody)
                code = "too_large";
            size_t total = 0;
            for (const auto& [name, value] : req.headers) {
                total += name.size() + value.size();
                if (name.size() + value.size() > kMaxHeaderBytes) code = "too_large";
            }
            if (total > kMaxHeadersTotal) code = "too_large";
        }
    }
    if (code != nullptr) {
        reject(req, code, done);
        return;
    }

    // Copied into PSRAM before the lock: the queue holds no internal RAM.
    os::LargeUnique<Job> job = os::make_large<Job>();
    if (!job) {
        reject(req, "other", done);
        return;
    }
    job->scene_id = req.scene_id;
    job->instance = req.instance;
    job->principal = req.principal;
    job->method = req.method;
    job->url.assign(req.url.data(), req.url.size());
    job->headers.reserve(req.headers.size());
    for (const auto& [name, value] : req.headers)
        job->headers.emplace_back(os::LargeString(name.data(), name.size()),
                                  os::LargeString(value.data(), value.size()));
    job->body = std::move(req.body);
    job->on_body = std::move(req.on_body);
    job->max_body = req.max_body;
    job->download = req.download;
    job->max_response = req.max_response;
    job->timeout_ms = req.timeout_ms;
    job->max_redirects = req.max_redirects;
    job->done = std::move(done);

    const HttpLane target = lane_of(*job);
    Lane& lane_to = lane(target);
    {
        std::lock_guard<os::Mutex> lock(mu_);
        job->seq = next_seq_++;
        if (!running_) {
            code = "other";
        } else if (is_agent) {
            // Never rate limited or deferred (file header): while
            // deferring it fails at once instead of waiting, and
            // otherwise it always goes straight into its lane's queue.
            if (deferring_) {
                code = "offline";
            } else {
                lane_to.queue.push_back(std::move(job));
            }
        } else {
            SceneState& st = scenes_[req.scene_id];
            const uint64_t now = now_ms_();
            while (!st.sent.empty() && st.sent.front() + kWindowMs <= now) st.sent.pop_front();
            if (st.sent.size() >= limits_.per_minute) {
                code = "rate_limited";
            } else if (deferring_) {
                if (st.deferred >= limits_.max_deferred_per_scene) {
                    code = "deferred_queue_full";
                } else {
                    st.sent.push_back(now);
                    ++st.deferred;
                    job->deferred = true;
                    job->deferred_at = now;
                    deferred_.push_back(std::move(job));
                }
            } else if (std::any_of(deferred_.begin(), deferred_.end(),
                                   [target](const auto& j) { return lane_of(*j) == target; })) {
                // Waiting requests of this lane are still draining into its
                // queue: line up behind them, in submission order. (The
                // other lane's backlog is no reason to wait.)
                if (st.deferred >= limits_.max_deferred_per_scene) {
                    code = "rate_limited";
                } else {
                    st.sent.push_back(now);
                    ++st.deferred;
                    job->deferred_at = now;
                    deferred_.push_back(std::move(job));
                }
            } else if (lane_to.queue.size() >= queue_cap(target)) {
                code = "rate_limited";
            } else {
                st.sent.push_back(now);
                lane_to.queue.push_back(std::move(job));
            }
        }
    }
    if (code != nullptr) {
        reject(req, code, job->done);
        return;
    }
    // A job that went to deferred_ is moved on by this lane's worker once
    // the lane has room.
    lane_to.wake.give();
}

void SceneHttp::cancel_scene(const std::string& scene_id) {
    Deque<os::LargeUnique<Job>> dropped;
    std::array<uint32_t, 2> tickets{};
    const auto drop_from = [&](Deque<os::LargeUnique<Job>>& from) {
        for (auto it = from.begin(); it != from.end();) {
            if ((*it)->scene_id == scene_id) {
                dropped.push_back(std::move(*it));
                it = from.erase(it);
            } else {
                ++it;
            }
        }
    };
    {
        std::lock_guard<os::Mutex> lock(mu_);
        for (size_t i = 0; i < lanes_.size(); ++i) {
            Lane& l = lanes_[i];
            drop_from(l.queue);
            if (l.current != nullptr && l.current->scene_id == scene_id) {
                l.current->cancelled = true;
                tickets[i] = l.current->ticket;
            }
        }
        drop_from(deferred_);
        if (auto st = scenes_.find(scene_id); st != scenes_.end()) st->second.deferred = 0;
    }
    // Outside mu_, and abort() only asks: the socket is shut down on the
    // watch's timer task, so a slow shutdown cannot hold up this (UI)
    // thread. A ticket the worker has moved past is ignored.
    for (size_t i = 0; i < lanes_.size(); ++i)
        if (tickets[i] != 0) lanes_[i].watch->abort(tickets[i]);
    // A done() (from either worker) that started before the mark finishes
    // before this returns; one that had not started sees the mark.
    std::lock_guard<os::Mutex> barrier(deliver_mu_);
}

void SceneHttp::set_deferring(bool on) {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (deferring_ == on) return;
        deferring_ = on;
        if (on) {
            // Whatever has not started goes back to wait (Wi-Fi is going
            // off), ahead of the rest, in the order it was accepted. The
            // per-scene cap is not applied to these.
            const uint64_t now = now_ms_();
            Deque<os::LargeUnique<Job>> back;
            for (Lane& l : lanes_) {
                for (auto& job : l.queue) back.push_back(std::move(job));
                l.queue.clear();
            }
            std::sort(back.begin(), back.end(),
                      [](const auto& a, const auto& b) { return a->seq < b->seq; });
            for (auto it = back.rbegin(); it != back.rend(); ++it) {
                ++scenes_[(*it)->scene_id].deferred;
                deferred_.push_front(std::move(*it));
            }
            for (auto& job : deferred_) {
                if (job->deferred) continue;
                job->deferred = true;
                job->deferred_at = now;
            }
        }
    }
    // The workers move the waiting requests (off), or the interactive one
    // starts timing them out (on).
    for (Lane& l : lanes_) l.wake.give();
}

size_t SceneHttp::deferred_count() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return deferred_.size();
}

bool SceneHttp::idle() const {
    std::lock_guard<os::Mutex> lock(mu_);
    if (!deferred_.empty()) return false;
    for (const Lane& l : lanes_)
        if (!l.queue.empty() || l.current != nullptr) return false;
    return true;
}

void SceneHttp::expire_locked(uint64_t now, Deque<os::LargeUnique<Job>>& out) {
    for (auto it = deferred_.begin(); it != deferred_.end();) {
        Job& job = **it;
        if (now >= job.deferred_at && now - job.deferred_at > limits_.deferred_timeout_ms) {
            if (auto st = scenes_.find(job.scene_id);
                st != scenes_.end() && st->second.deferred > 0)
                --st->second.deferred;
            out.push_back(std::move(*it));
            it = deferred_.erase(it);
        } else {
            ++it;
        }
    }
}

void SceneHttp::promote_locked(HttpLane which) {
    if (deferring_) return;
    Lane& to = lane(which);
    // Only this lane's jobs, oldest first: each lane's worker moves its
    // own, so one lane's order is decided by one thread, as with a single
    // worker; the other lane's jobs stay in place for their worker.
    for (auto it = deferred_.begin();
         it != deferred_.end() && to.queue.size() < queue_cap(which);) {
        if (lane_of(**it) != which) {
            ++it;
            continue;
        }
        if (auto st = scenes_.find((*it)->scene_id); st != scenes_.end() && st->second.deferred > 0)
            --st->second.deferred;
        to.queue.push_back(std::move(*it));
        it = deferred_.erase(it);
    }
}

void SceneHttp::run(HttpLane which) {
    Lane& self = lane(which);
    // Only one worker times out deferred requests: the interactive one,
    // which a download never holds for minutes.
    const bool expires = which == HttpLane::Interactive;
    uint32_t wait = os::kForever;
    for (;;) {
        self.wake.take(wait);
        for (;;) {
            // Timed-out deferred requests are answered under deliver_mu_,
            // taken before they leave deferred_: a cancel_scene() that
            // missed them in deferred_ then waits for these done() calls.
            if (expires) {
                std::lock_guard<os::Mutex> deliver(deliver_mu_);
                {
                    std::lock_guard<os::Mutex> lock(mu_);
                    if (!running_) return;
                    expire_locked(now_ms_(), expired_);
                }
                for (auto& job : expired_) {
                    WLOG_W(
                        scene_tag(job->scene_id).c_str(), "http: deferred_timeout (host %s)",
                        host_for_log(std::string_view(job->url.data(), job->url.size())).c_str());
                    SceneHttpResult r;
                    r.error = "deferred_timeout";
                    r.deferred = true;
                    if (job->done) job->done(std::move(r));
                }
                expired_.clear();
            }
            os::LargeUnique<Job> job;
            {
                std::lock_guard<os::Mutex> lock(mu_);
                if (!running_) return;
                promote_locked(which);
                if (self.queue.empty()) {
                    wait = expires && !deferred_.empty() ? limits_.deferred_check_ms : os::kForever;
                    break;
                }
                job = std::move(self.queue.front());
                self.queue.pop_front();
                self.current = job.get();
                // arm() never blocks (the timer is only started), so it
                // can run under mu_, where cancel_scene() reads the ticket.
                job->ticket = self.watch->arm(timeout_of(*job));
            }
            execute(*job, self);
            std::lock_guard<os::Mutex> lock(mu_);
            self.current = nullptr;
        }
    }
}

void SceneHttp::execute(Job& job, Lane& own) {
    const bool is_agent = job.principal == HttpPrincipal::Agent;
    const uint32_t timeout_ms = timeout_of(job);
    const bool streamed = static_cast<bool>(job.on_body);
    const uint64_t started_at = now_ms_();

    // The response cap: SceneHttpLimits/AgentHttpLimits unless the job
    // asked for a smaller one (never a bigger one than the Agent
    // principal's hard cap).
    size_t response_cap = is_agent ? AgentHttpLimits::kMaxResponse : limits_.max_response;
    if (job.max_response != 0) response_cap = std::min(response_cap, job.max_response);

    // Redirects are an Agent-only feature (file header): a Scene request
    // ignores max_redirects outright, whatever it is.
    uint8_t max_redirects =
        is_agent ? std::min<uint8_t>(job.max_redirects, AgentHttpLimits::kMaxRedirects) : 0;

    // One deadline for the whole request, every hop included (spec 5.3's
    // 15 s, or the Agent principal's own timeout_ms). A client's own
    // timeout may apply to each low-level read (esp_http_client's does),
    // and a server that trickles its answer would then hold this lane's
    // worker for as long as it likes. So the watch interrupts a call blocked in
    // the client at the deadline (and on cancel), and between pieces the
    // deadline is checked here as well. Each later hop re-arms the watch
    // (and job.ticket, under mu_, so cancel_scene() still aborts the right
    // request) with only the time left of it, never a fresh full timeout.
    const uint64_t deadline = now_ms_() + timeout_ms;
    auto expired = [this, deadline] { return now_ms_() >= deadline; };

    std::string url(job.url.data(), job.url.size());
    Method method = job.method;
    const uint8_t* body_data = reinterpret_cast<const uint8_t*>(job.body.data());
    size_t body_size = job.body.size();
    std::vector<std::pair<std::string, std::string>> headers;
    headers.reserve(job.headers.size());
    for (const auto& [name, value] : job.headers)
        headers.emplace_back(std::string(name.data(), name.size()),
                             std::string(value.data(), value.size()));

    BodyBuffer body(response_cap);
    SceneHttpResult result;
    result.deferred = job.deferred;
    bool final_truncated = false;

    for (uint8_t attempt = 0;; ++attempt) {
        const uint64_t now = now_ms_();
        if (now >= deadline) {
            result.error = "timeout";
            break;
        }
        const uint32_t remaining = static_cast<uint32_t>(deadline - now);
        if (attempt != 0) {
            std::lock_guard<os::Mutex> lock(mu_);
            job.ticket = own.watch->arm(remaining);
        }

        // Ordinary heap copies of the URL and headers for the client, only
        // for the request being sent (their sizes are capped in submit()).
        HttpClientRequest req;
        req.method = method;
        req.url = url;
        req.headers = headers;
        req.body_ref = {body_data, body_size};
        req.timeout_ms = remaining;
        req.max_response_bytes = response_cap;
        if (streamed) {
            const uint64_t cap =
                job.max_body != 0 ? job.max_body : SceneHttpRequest::kMaxDownloadBody;
            req.max_response_bytes = static_cast<size_t>(cap);
        } else if (is_agent) {
            // The Agent principal's cap is a truncation, not an error
            // (file header): the client itself stops at it instead of
            // failing the request.
            req.truncate_at_limit = true;
        }
        req.watch = own.watch.get();

        body.reset();
        size_t streamed_bytes = 0;
        // A download's body goes to on_body only with a 2xx status; any
        // other answer's body (an error page) is read and dropped, up to
        // the plain response cap, after which the transfer stops with the
        // status kept.
        int status = 0;
        bool error_body_cut = false;
        bool too_large = false;
        bool no_memory = false;
        bool timed_out = false;
        req.on_status = [&status](int st) { status = st; };
        req.on_body = [&](std::span<const uint8_t> chunk) {
            if (job.cancelled.load()) return false;
            if (expired()) {
                timed_out = true;
                return false;
            }
            if (streamed && status != 0 && (status < 200 || status >= 300)) {
                if (chunk.size() > limits_.max_response - streamed_bytes) {
                    error_body_cut = true;
                    return false;
                }
                streamed_bytes += chunk.size();
                return true;
            }
            if (streamed) {
                if (chunk.size() > req.max_response_bytes - streamed_bytes) {
                    too_large = true;
                    return false;
                }
                streamed_bytes += chunk.size();
                return job.on_body(chunk);
            }
            if (chunk.size() > body.remaining()) {
                too_large = true;
                return false;
            }
            switch (body.add(chunk)) {
                case BodyBuffer::Add::Ok:
                    return true;
                case BodyBuffer::Add::TooLarge:
                    too_large = true;
                    return false;
                case BodyBuffer::Add::NoMemory:
                    no_memory = true;
                    return false;
            }
            return false;
        };
        req.cancelled = [&job, &expired] { return job.cancelled.load() || expired(); };

        HttpClientResponse res = client_.request(req);
        const bool watch_deadline = own.watch->reason() == RequestWatch::Reason::Deadline;

        if (timed_out || (res.transport == TransportError::Cancelled && expired()) ||
            (watch_deadline && res.transport != TransportError::None)) {
            result.error = "timeout";
            break;
        }
        if (too_large || res.transport == TransportError::TooLarge) {
            result.error = "too_large";
            break;
        }
        if (error_body_cut) {
            result.status = status;
            result.headers = std::move(res.headers);
            break;
        }
        if (no_memory) {
            WLOG_W(kTag, "%s: no memory for a response body", job.scene_id.c_str());
            result.error = "other";
            break;
        }
        if (res.transport == TransportError::Cancelled) {
            // The scene was cancelled (nothing is delivered below) or a
            // piece was refused for a reason handled above.
            result.error = "other";
            break;
        }
        if (res.transport != TransportError::None) {
            result.error = to_string(res.transport);
            break;
        }

        // A real response came back. Follow it if it is a redirect this
        // job still has hops left for, that resolves to somewhere, and
        // whose resolved target is itself a valid Agent URL (file header:
        // redirect targets are re-checked exactly like a fresh request's
        // URL); otherwise this is the final answer, as it stands.
        bool follow = false;
        std::string resolved;
        if (status >= 300 && status < 400 && attempt < max_redirects && !res.truncated) {
            std::string location;
            for (const auto& [name, value] : res.headers)
                if (iequals(name, "location")) {
                    location = value;
                    break;
                }
            if (!location.empty()) {
                resolved = resolve_location(url, location);
                if (!resolved.empty() && resolved.size() <= AgentHttpLimits::kMaxUrlBytes &&
                    agent_url_error(resolved).empty()) {
                    follow = true;
                }
            }
        }
        if (follow) {
            if (origin_key(resolved) != origin_key(url)) drop_cross_origin_credentials(headers);
            // POST becomes a bodyless GET on 303; the other redirect
            // statuses keep the method and body (file header).
            if (status == 303) {
                method = Method::Get;
                body_data = nullptr;
                body_size = 0;
            }
            url = std::move(resolved);
            continue;
        }

        if (streamed) {
            result.status = status;
            result.headers = std::move(res.headers);
        } else {
            result.body = body.take();
            if (!result.body.ok()) {
                WLOG_W(kTag, "%s: no memory for a response body", job.scene_id.c_str());
                result.error = "other";
            } else {
                result.status = status;
                result.headers = std::move(res.headers);
                final_truncated = res.truncated;
            }
        }
        break;
    }
    result.final_url = url;
    result.body_truncated = final_truncated && result.error.empty();
    if (!result.error.empty()) result.body = os::LargeBuffer();
    own.watch->disarm();

    if (is_agent) {
        // agent-tools spec 3.2: method, host, final status and elapsed
        // time, nothing past the host.
        WLOG_I(kTag, "agent http: %s %s -> %d (%llu ms)", method_name(job.method),
               host_for_log(url).c_str(), result.status,
               static_cast<unsigned long long>(now_ms_() - started_at));
    }

    std::lock_guard<os::Mutex> lock(deliver_mu_);
    if (job.cancelled.load() || !job.done) return;
    job.done(std::move(result));
}

std::string SceneHttp::check_url(std::string_view url, const std::vector<std::string>& hosts) {
    constexpr std::string_view kScheme = "https://";
    if (!istarts_with(url, kScheme)) return "not_https";
    // Nothing a client could read as a line break or a second token.
    for (char c : url) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7f || c == '\\') return "permission_denied";
    }
    std::string_view authority = url.substr(kScheme.size());
    authority = authority.substr(0, authority.find_first_of("/?#"));
    // User info is refused outright: it hides the real host from a reader.
    if (authority.find('@') != std::string_view::npos) return "permission_denied";
    std::string_view host = authority;
    if (size_t colon = authority.find(':'); colon != std::string_view::npos) {
        host = authority.substr(0, colon);
        std::string_view port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5) return "permission_denied";
        uint32_t n = 0;
        for (char c : port) {
            if (c < '0' || c > '9') return "permission_denied";
            n = n * 10 + static_cast<uint32_t>(c - '0');
        }
        if (n == 0 || n > 65535) return "permission_denied";
    }
    if (host.empty()) return "permission_denied";
    std::string lowered;
    lowered.reserve(host.size());
    for (char c : host) {
        c = lower(c);
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
            return "permission_denied";
        lowered.push_back(c);
    }
    for (const std::string& h : hosts)
        if (h == lowered) return "";
    return "permission_denied";
}

bool SceneHttp::header_syntax_ok(std::string_view name, std::string_view value) {
    if (name.empty()) return false;
    for (char c : name)
        if (!is_tchar(c)) return false;
    for (char c : value)
        if (c == '\r' || c == '\n' || c == '\0') return false;
    return true;
}

bool SceneHttp::header_allowed(std::string_view name, std::string_view value) {
    if (!header_syntax_ok(name, value)) return false;
    static constexpr std::string_view kDenied[] = {
        "cookie",     "cookie2",        "proxy-authorization", "host",
        "connection", "content-length", "transfer-encoding",   "upgrade",
        "te",         "trailer",        "keep-alive",          "expect",
        "x-wisp",
    };
    for (std::string_view d : kDenied)
        if (iequals(name, d)) return false;
    return !istarts_with(name, "proxy-") && !istarts_with(name, "sec-");
}

}  // namespace wisp::net
