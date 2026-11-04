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

// A response body growing in Mem::Large up to `max` bytes.
class BodyBuffer {
public:
    explicit BodyBuffer(size_t max) : max_(max) {}
    ~BodyBuffer() { os::dealloc(data_); }
    BodyBuffer(const BodyBuffer&) = delete;
    BodyBuffer& operator=(const BodyBuffer&) = delete;

    enum class Add { Ok, TooLarge, NoMemory };

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
    Method method = Method::Get;
    os::LargeString url;
    os::LargeVector<std::pair<os::LargeString, os::LargeString>> headers;
    os::LargeString body;
    std::function<void(SceneHttpResult)> done;
    // Set under mu_ by cancel_scene()/stop(); read by the client between
    // pieces and by the worker under deliver_mu_.
    std::atomic<bool> cancelled{false};
    // Its ticket for watch_, set under mu_ when the worker takes it.
    uint32_t ticket = 0;
};

SceneHttp::SceneHttp(HttpClient& client, SceneHttpLimits limits, std::function<uint64_t()> now_ms)
    : client_(client),
      limits_(limits),
      now_ms_(std::move(now_ms)),
      // The worker drains the whole queue on each wake, so surplus gives
      // past the maximum are harmless.
      wake_(1024, 0) {}

SceneHttp::~SceneHttp() { stop(); }

os::TaskConfig SceneHttp::task_config() {
    os::TaskConfig t;
    t.name = "wisp.shttp";
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
    // Made once and reused by every request, so a request allocates no
    // timer; without it the proxy stays off (submit() answers "other").
    watch_ = RequestWatch::create();
    if (!watch_) return Result<void>::err(ErrorCode::NoMemory, "cannot make the scene HTTP watch");
    {
        std::lock_guard<os::Mutex> lock(mu_);
        running_ = true;
    }
    task_ = os::Task::spawn(task_config(), [this] { run(); });
    if (!task_) {
        std::lock_guard<os::Mutex> lock(mu_);
        running_ = false;
        return Result<void>::err(ErrorCode::NoMemory, "cannot start the scene HTTP task");
    }
    return Result<void>::ok();
}

void SceneHttp::stop() {
    std::deque<os::LargeUnique<Job>> dropped;
    uint32_t ticket = 0;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!running_) return;
        running_ = false;
        dropped.swap(queue_);
        if (current_ != nullptr) {
            current_->cancelled = true;
            ticket = current_->ticket;
        }
    }
    if (ticket != 0) watch_->abort(ticket);
    wake_.give();
    if (task_) {
        task_->join();
        task_.reset();
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
    if (done) done(std::move(r));
}

void SceneHttp::submit(SceneHttpRequest req, std::function<void(SceneHttpResult)> done) {
    // The order of the checks is the one the header documents.
    const char* code = nullptr;
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
        if (req.url.size() > kMaxUrlBytes || req.body.size() > kMaxRequestBody) code = "too_large";
        size_t total = 0;
        for (const auto& [name, value] : req.headers) {
            total += name.size() + value.size();
            if (name.size() + value.size() > kMaxHeaderBytes) code = "too_large";
        }
        if (total > kMaxHeadersTotal) code = "too_large";
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
    job->method = req.method;
    job->url.assign(req.url.data(), req.url.size());
    job->headers.reserve(req.headers.size());
    for (const auto& [name, value] : req.headers)
        job->headers.emplace_back(os::LargeString(name.data(), name.size()),
                                  os::LargeString(value.data(), value.size()));
    job->body = std::move(req.body);
    job->done = std::move(done);

    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!running_) {
            code = "other";
        } else {
            SceneState& st = scenes_[req.scene_id];
            const uint64_t now = now_ms_();
            while (!st.sent.empty() && st.sent.front() + kWindowMs <= now) st.sent.pop_front();
            if (st.sent.size() >= limits_.per_minute || queue_.size() >= limits_.queue) {
                code = "rate_limited";
            } else {
                st.sent.push_back(now);
                queue_.push_back(std::move(job));
            }
        }
    }
    if (code != nullptr) {
        reject(req, code, job->done);
        return;
    }
    wake_.give();
}

void SceneHttp::cancel_scene(const std::string& scene_id) {
    std::deque<os::LargeUnique<Job>> dropped;
    uint32_t ticket = 0;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        for (auto it = queue_.begin(); it != queue_.end();) {
            if ((*it)->scene_id == scene_id) {
                dropped.push_back(std::move(*it));
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
        if (current_ != nullptr && current_->scene_id == scene_id) {
            current_->cancelled = true;
            ticket = current_->ticket;
        }
    }
    // Outside mu_, and abort() only asks: the socket is shut down on the
    // watch's timer task, so a slow shutdown cannot hold up this (UI)
    // thread. A ticket the worker has moved past is ignored.
    if (ticket != 0) watch_->abort(ticket);
    // A done() that started before the mark finishes before this returns;
    // one that had not started sees the mark.
    std::lock_guard<os::Mutex> barrier(deliver_mu_);
}

void SceneHttp::run() {
    for (;;) {
        wake_.take(os::kForever);
        for (;;) {
            os::LargeUnique<Job> job;
            {
                std::lock_guard<os::Mutex> lock(mu_);
                if (!running_) return;
                if (queue_.empty()) break;
                job = std::move(queue_.front());
                queue_.pop_front();
                current_ = job.get();
                // arm() never blocks (the timer is only started), so it
                // can run under mu_, where cancel_scene() reads the ticket.
                job->ticket = watch_->arm(limits_.timeout_ms);
            }
            execute(*job);
            std::lock_guard<os::Mutex> lock(mu_);
            current_ = nullptr;
        }
    }
}

void SceneHttp::execute(Job& job) {
    // Ordinary heap copies of the URL and headers for the client, only
    // for the request being sent (their sizes are capped in submit()).
    HttpClientRequest req;
    req.method = job.method;
    req.url.assign(job.url.data(), job.url.size());
    req.headers.reserve(job.headers.size());
    for (const auto& [name, value] : job.headers)
        req.headers.emplace_back(std::string(name.data(), name.size()),
                                 std::string(value.data(), value.size()));
    req.body_ref = {reinterpret_cast<const uint8_t*>(job.body.data()), job.body.size()};
    req.timeout_ms = limits_.timeout_ms;
    req.max_response_bytes = limits_.max_response;
    // One deadline for the whole request (spec 5.3's 15 s). A client's own
    // timeout may apply to each low-level read (esp_http_client's does),
    // and a server that trickles its answer would then hold the one worker
    // for as long as it likes. So the watch interrupts a call blocked in
    // the client at the deadline (and on cancel), and between pieces the
    // deadline is checked here as well.
    req.watch = watch_.get();
    const uint64_t deadline = now_ms_() + limits_.timeout_ms;
    auto expired = [this, deadline] { return now_ms_() >= deadline; };

    BodyBuffer body(limits_.max_response);
    bool too_large = false;
    bool no_memory = false;
    bool timed_out = false;
    req.on_body = [&](std::span<const uint8_t> chunk) {
        if (job.cancelled.load()) return false;
        if (expired()) {
            timed_out = true;
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
    watch_->disarm();
    const bool watch_deadline = watch_->reason() == RequestWatch::Reason::Deadline;

    SceneHttpResult result;
    if (timed_out || (res.transport == TransportError::Cancelled && expired()) ||
        (watch_deadline && res.transport != TransportError::None)) {
        result.error = "timeout";
    } else if (too_large || res.transport == TransportError::TooLarge) {
        result.error = "too_large";
    } else if (no_memory) {
        WLOG_W(kTag, "%s: no memory for a response body", job.scene_id.c_str());
        result.error = "other";
    } else if (res.transport == TransportError::Cancelled) {
        // The scene was cancelled (nothing is delivered below) or a piece
        // was refused for a reason handled above.
        result.error = "other";
    } else if (res.transport != TransportError::None) {
        result.error = to_string(res.transport);
    } else {
        result.body = body.take();
        if (!result.body.ok()) {
            WLOG_W(kTag, "%s: no memory for a response body", job.scene_id.c_str());
            result.error = "other";
        } else {
            result.status = res.status;
            result.headers = std::move(res.headers);
        }
    }
    if (!result.error.empty()) result.body = os::LargeBuffer();

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

bool SceneHttp::header_allowed(std::string_view name, std::string_view value) {
    if (name.empty()) return false;
    for (char c : name)
        if (!is_tchar(c)) return false;
    for (char c : value)
        if (c == '\r' || c == '\n' || c == '\0') return false;
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
