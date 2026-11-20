#include "wisp/media/library_sink.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/os/flash_guard.hpp"

namespace wisp::media {

namespace {

constexpr const char* kTag = "media";
constexpr uint32_t kWaitSliceMs = 50;
constexpr uint32_t kSubmitRetryMs = 10;

}  // namespace

void LibrarySink::open(MediaLibrary& lib, std::string path, Opened opened) {
    // shared so that a refused job leaves `opened` with the caller.
    auto cb = std::make_shared<Opened>(std::move(opened));
    const bool queued = lib.submit(core::IoLane::Bulk, [&lib, path = std::move(path), cb] {
        auto w = lib.begin_write(path);
        if (!w)
            return (*cb)(
                Result<std::shared_ptr<LibrarySink>>::err(w.error().code, w.error().message));
        std::shared_ptr<LibrarySink> sink(new LibrarySink(lib.token(), std::move(w.value())));
        (*cb)(Result<std::shared_ptr<LibrarySink>>::ok(std::move(sink)));
    });
    if (!queued)
        (*cb)(Result<std::shared_ptr<LibrarySink>>::err(ErrorCode::Busy,
                                                        "media library cannot take the write"));
}

LibrarySink::LibrarySink(std::shared_ptr<MediaLibrary::Token> token,
                         std::unique_ptr<MediaLibrary::Writer> writer)
    : token_(std::move(token)), writer_(std::move(writer)) {}

// A writer still here (finish() never ran) is dropped by its own
// destructor, which aborts on the worker, elsewhere leaves the ".part"
// for the next listing of its folder, and after the library is gone does
// nothing.
LibrarySink::~LibrarySink() = default;

std::optional<Error> LibrarySink::error() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return error_;
}

void LibrarySink::cancel() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        cancelled_ = true;
    }
    wake_.give();
}

bool LibrarySink::dispatch(os::LargeBuffer buf) {
    auto shared = std::make_shared<os::LargeBuffer>(std::move(buf));
    std::function<void()> job = [self = shared_from_this(), shared] { self->append(*shared); };
    // Every chunk and the finish go to the same lane, so they run in
    // order. A full lane is mostly this sink's own appends or another
    // stream's, which drain; worth waiting for rather than failing the
    // transfer, until cancel() or a failure.
    for (uint32_t waited = 0;; waited += kSubmitRetryMs) {
        if (token_->submit(core::IoLane::Bulk, job)) return true;
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if (error_ || cancelled_ || waited >= kSubmitGiveUpMs || !token_->alive()) {
                queued_ -= std::min(queued_, kChunk);
                if (!error_) error_ = Error{ErrorCode::Busy, "media library cannot take the write"};
                return false;
            }
        }
        os::sleep_ms(kSubmitRetryMs);
    }
}

void LibrarySink::append(const os::LargeBuffer& buf) {
    bool skip = false;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        // Not finished_: the appends queued before finish() still count.
        skip = error_.has_value() || cancelled_ || !writer_;
    }
    Result<void> r = Result<void>::ok();
    if (!skip) r = writer_->append(buf.span());
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!r && !error_) error_ = r.error();
        queued_ -= std::min(queued_, buf.size());
    }
    wake_.give();
}

bool LibrarySink::push(std::span<const uint8_t> chunk) {
    while (!chunk.empty()) {
        os::LargeBuffer full;
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if (stopped_locked()) return false;
            if (!staging_.ok()) {
                staging_ = os::LargeBuffer::allocate(kChunk);
                staged_ = 0;
                if (!staging_.ok()) {
                    error_ = Error{ErrorCode::NoMemory, "no memory for a download buffer"};
                    return false;
                }
            }
            const size_t n = std::min(chunk.size(), kChunk - staged_);
            std::memcpy(staging_.data() + staged_, chunk.data(), n);
            staged_ += n;
            chunk = chunk.subspan(n);
            if (staged_ == kChunk) {
                full = std::move(staging_);
                staged_ = 0;
                queued_ += kChunk;
            }
        }
        if (full.ok() && !dispatch(std::move(full))) return false;
        // The producer waits for the worker, never the other way round.
        for (;;) {
            {
                std::lock_guard<os::Mutex> lock(mu_);
                if (stopped_locked()) return false;
                if (queued_ <= kMaxQueued) break;
            }
            wake_.take(kWaitSliceMs);
        }
    }
    std::lock_guard<os::Mutex> lock(mu_);
    return !stopped_locked();
}

void LibrarySink::run_finish(bool ok, const Finished& done) {
    os::LargeBuffer rest;
    size_t rest_n = 0;
    std::optional<Error> err;
    bool cancelled = false;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        rest = std::move(staging_);
        rest_n = staged_;
        staged_ = 0;
        err = error_;
        cancelled = cancelled_;
    }
    wake_.give();
    std::unique_ptr<MediaLibrary::Writer> w = std::move(writer_);
    if (!w) return done(Result<uint64_t>::ok(0));
    if (!ok || cancelled || err) {
        w->abort();
        if (err && ok && !cancelled) return done(Result<uint64_t>::err(err->code, err->message));
        return done(Result<uint64_t>::ok(0));
    }
    if (rest_n > 0) {
        if (auto r = w->append(std::span<const uint8_t>(rest.data(), rest_n)); !r)
            return done(Result<uint64_t>::err(r.error().code, r.error().message));
    }
    if (auto r = w->commit(); !r)
        return done(Result<uint64_t>::err(r.error().code, r.error().message));
    done(Result<uint64_t>::ok(w->written()));
}

bool LibrarySink::finish(bool ok, Finished done, uint32_t wait_ms) {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (finished_) return false;
        finished_ = true;
    }
    wake_.give();
    std::function<void()> job = [self = shared_from_this(), ok, done = std::move(done)] {
        self->run_finish(ok, done);
    };
    if (os::sd_io_allowed()) wait_ms = 0;
    for (uint32_t waited = 0;; waited += kSubmitRetryMs) {
        if (token_->submit(core::IoLane::Bulk, job)) return true;
        if (waited >= wait_ms || !token_->alive()) break;
        os::sleep_ms(kSubmitRetryMs);
    }
    // Not queued: a later call may try again (the header).
    {
        std::lock_guard<os::Mutex> lock(mu_);
        finished_ = false;
    }
    WLOG_W(kTag, "a download's end could not be queued");
    return false;
}

}  // namespace wisp::media
