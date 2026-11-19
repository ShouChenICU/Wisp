#include "wisp/media/item_reader.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <optional>
#include <utility>

#include "wisp/media/library.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::media {

bool split_scene_item(std::string_view item, std::string_view& scene, std::string_view& name) {
    if (item.substr(0, kSceneItemPrefix.size()) != kSceneItemPrefix) return false;
    const std::string_view rest = item.substr(kSceneItemPrefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == rest.size()) return false;
    if (rest.find('/', slash + 1) != std::string_view::npos) return false;
    scene = rest.substr(0, slash);
    name = rest.substr(slash + 1);
    return true;
}

// Everything a storage job touches lives here, not in the reader: a job
// may run after the reader is gone (it then sees `closed` and does no file
// work).
struct ItemReader::Shared {
    // Where the bytes come from: the library, or a scene's files.
    MediaLibrary* library = nullptr;
    core::SceneFiles* files = nullptr;
    core::StorageRouter* router = nullptr;
    std::string path;         // library path
    std::string scene, name;  // scene file

    os::Mutex mu;
    // Given when a job finishes, so a waiting read or the destructor
    // wakes at once instead of at its next poll.
    os::Semaphore wake{1, 0};
    bool closed = false;   // the reader is gone: jobs do no file work
    bool running = false;  // a job is inside its file call

    // The ring: file bytes [base, base + len) start at ring[head].
    os::LargeBuffer ring;
    uint64_t base = 0;
    size_t len = 0;
    size_t head = 0;
    uint64_t pos = 0;  // the next byte read() returns
    uint64_t size = 0;
    // Moves on every seek outside the ring, so a refill still in flight
    // for the old window is dropped when it lands.
    uint32_t gen = 0;
    bool inflight = false;
    std::optional<Error> error;  // sticky: every later read fails with it
    // The first refusal of the current run of refused submissions.
    std::optional<uint64_t> refused_since;

    Result<os::LargeBuffer> fetch(uint64_t off, size_t n) {
        if (library != nullptr) return library->read_now(path, off, n);
        return files->read(scene, name, off, n);
    }

    // Queues `job` on the worker for this item's medium.
    bool submit(std::function<void()> job) {
        if (library != nullptr) {
            if (router == nullptr) return false;
            return router->submit(core::Medium::Sd, std::move(job), core::IoLane::Interactive);
        }
        return files->submit(scene, std::move(job), core::IoLane::Interactive);
    }

    // Appends `data` read at `off` (with mu held). The refill was queued
    // with at most half the ring unread and is at most half of it, so
    // dropping read bytes always makes room; read bytes are kept as long
    // as possible, so a seek back to the start after probing stays in the
    // ring.
    void append_locked(uint64_t off, const os::LargeBuffer& data) {
        const size_t cap = ring.size();
        if (off != base + len) return;  // a stale window; cannot happen with gen checked
        const size_t n = std::min(data.size(), cap);
        if (len + n > cap) {
            const size_t drop = std::min<uint64_t>(len + n - cap, pos - base);
            base += drop;
            head = (head + drop) % cap;
            len -= drop;
        }
        const size_t room = std::min(n, cap - len);
        size_t at = (head + len) % cap;
        const size_t first = std::min(room, cap - at);
        std::memcpy(ring.data() + at, data.data(), first);
        std::memcpy(ring.data(), data.data() + first, room - first);
        len += room;
    }

    // Marks the reader closed and waits for a job inside its file call.
    void close_and_wait() {
        std::unique_lock<os::Mutex> lock(mu);
        closed = true;
        while (running) {
            lock.unlock();
            wake.take(kPollMs);
            lock.lock();
        }
    }
};

namespace {

using Shared = ItemReader::Shared;

// Wraps a job body: it runs only while the reader is open, with `running`
// set around the file call. `dropped` runs instead when the job is
// destroyed without having run (a refused submission, a worker that drops
// it), so the reader does not wait for it.
std::function<void()> guarded(std::shared_ptr<Shared> sh, std::function<void(Shared&)> body,
                              std::function<void(Shared&)> dropped) {
    struct Guard {
        std::shared_ptr<Shared> sh;
        std::function<void(Shared&)> dropped;
        std::atomic<bool> ran{false};
        ~Guard() {
            if (!ran.load() && dropped) dropped(*sh);
        }
    };
    auto g = std::make_shared<Guard>();
    g->sh = sh;
    g->dropped = std::move(dropped);
    return [g, body = std::move(body)] {
        g->ran = true;
        Shared& s = *g->sh;
        {
            std::lock_guard<os::Mutex> lock(s.mu);
            if (s.closed) return;
            s.running = true;
        }
        body(s);
        {
            std::lock_guard<os::Mutex> lock(s.mu);
            s.running = false;
        }
        s.wake.give();
    };
}

// Waits for `done` (checked under mu), polling `abort`. Timeout when aborted.
Result<void> wait_for(Shared& s, const std::function<bool()>& done,
                      const std::function<bool()>& abort) {
    for (;;) {
        {
            std::lock_guard<os::Mutex> lock(s.mu);
            if (done()) return Result<void>::ok();
        }
        s.wake.take(ItemReader::kPollMs);
        if (abort && abort()) {
            return Result<void>::err(ErrorCode::Timeout, "aborted");
        }
    }
}

}  // namespace

Result<std::unique_ptr<ItemReader>> ItemReader::open(const std::string& item,
                                                     const ItemReaderDeps& deps) {
    using R = Result<std::unique_ptr<ItemReader>>;
    auto sh = std::allocate_shared<Shared>(os::LargeAllocator<Shared>());
    std::string_view scene, name;
    if (split_scene_item(item, scene, name)) {
        if (deps.scene_files == nullptr) return R::err(ErrorCode::Unsupported, "no scene files");
        sh->files = deps.scene_files;
        sh->scene = std::string(scene);
        sh->name = std::string(name);
    } else if (item.substr(0, kSceneItemPrefix.size()) == kSceneItemPrefix) {
        return R::err(ErrorCode::InvalidArgument, "bad scene file reference");
    } else {
        if (deps.library == nullptr || deps.router == nullptr) {
            return R::err(ErrorCode::StorageUnavailable, "no media library");
        }
        sh->library = deps.library;
        sh->router = deps.router;
        sh->path = item;
    }
    // At least two reads fit, so a refill queued at half empty always has
    // room (Shared::append_locked()).
    const size_t read_bytes = std::clamp<size_t>(deps.read_bytes, 512, kMaxReadBytes);
    sh->ring = os::LargeBuffer::allocate(std::max(deps.ring_bytes, 2 * read_bytes));
    if (!sh->ring.ok()) return R::err(ErrorCode::NoMemory, "media ring");

    // The file's size, on the storage worker.
    struct Stat {
        bool done = false;
        std::optional<Result<uint64_t>> size;
    };
    auto st = std::make_shared<Stat>();
    if (sh->library != nullptr) {
        sh->library->stat(sh->path, [sh, st](Result<MediaEntry> r) {
            {
                std::lock_guard<os::Mutex> lock(sh->mu);
                if (!r) {
                    st->size.emplace(Result<uint64_t>::err(r.error().code, r.error().message));
                } else if (r.value().dir) {
                    st->size.emplace(Result<uint64_t>::err(ErrorCode::NotFound, "a folder"));
                } else {
                    st->size.emplace(Result<uint64_t>::ok(r.value().size));
                }
                st->done = true;
            }
            sh->wake.give();
        });
    } else {
        const uint64_t first_refusal = os::now_ms();
        for (;;) {
            auto job = guarded(
                sh,
                [st](Shared& s) {
                    auto r = s.files->stat(s.scene, s.name);
                    std::lock_guard<os::Mutex> lock(s.mu);
                    st->size.emplace(r ? Result<uint64_t>::ok(r.value().size)
                                       : Result<uint64_t>::err(r.error().code, r.error().message));
                    st->done = true;
                },
                nullptr);
            if (sh->submit(std::move(job))) break;
            if (os::now_ms() - first_refusal >= kRefusedGiveUpMs) {
                return R::err(ErrorCode::Busy, "storage refused the lookup");
            }
            if (deps.abort && deps.abort()) return R::err(ErrorCode::Timeout, "aborted");
            os::sleep_ms(kPollMs);
        }
    }
    Result<void> waited = wait_for(*sh, [&st] { return st->done; }, deps.abort);
    if (!waited) {
        sh->close_and_wait();
        return R::err(waited.error().code, waited.error().message);
    }
    Result<uint64_t> size = std::move(*st->size);
    if (!size) return R::err(size.error().code, size.error().message);
    sh->size = size.value();
    ItemReaderDeps d = deps;
    d.read_bytes = read_bytes;
    std::unique_ptr<ItemReader> reader(new ItemReader(d, sh, size.value()));
    reader->maybe_refill();
    return R::ok(std::move(reader));
}

ItemReader::ItemReader(const ItemReaderDeps& deps, std::shared_ptr<Shared> shared, uint64_t size)
    : deps_(deps), shared_(std::move(shared)), size_(size) {}

ItemReader::~ItemReader() { shared_->close_and_wait(); }

void ItemReader::maybe_refill() {
    Shared& s = *shared_;
    uint64_t off = 0;
    size_t n = 0;
    uint32_t gen = 0;
    {
        std::lock_guard<os::Mutex> lock(s.mu);
        if (s.closed || s.inflight || s.error) return;
        const uint64_t end = s.base + s.len;
        if (end >= s.size) return;
        const uint64_t unread = end > s.pos ? end - s.pos : 0;
        if (unread > s.ring.size() / 2) return;
        off = end;
        n = static_cast<size_t>(std::min<uint64_t>(deps_.read_bytes, s.size - end));
        gen = s.gen;
        s.inflight = true;
    }
    auto job = guarded(
        shared_,
        [off, n, gen](Shared& sh) {
            {
                std::lock_guard<os::Mutex> lock(sh.mu);
                if (gen != sh.gen) {
                    sh.inflight = false;
                    return;
                }
            }
            Result<os::LargeBuffer> r = sh.fetch(off, n);
            std::lock_guard<os::Mutex> lock(sh.mu);
            sh.inflight = false;
            if (gen != sh.gen) return;
            if (!r) {
                sh.error = r.error();
            } else if (r.value().size() == 0) {
                sh.error = Error{ErrorCode::IoError, "the file ended early"};
            } else {
                sh.append_locked(off, r.value());
            }
        },
        [](Shared& sh) {
            // Never ran: the reader queues the refill again.
            std::lock_guard<os::Mutex> lock(sh.mu);
            sh.inflight = false;
        });
    const bool queued = s.submit(std::move(job));
    std::lock_guard<os::Mutex> lock(s.mu);
    if (queued) {
        s.refused_since.reset();
        return;
    }
    s.inflight = false;
    const uint64_t now = os::now_ms();
    if (!s.refused_since) {
        s.refused_since = now;
    } else if (now - *s.refused_since >= kRefusedGiveUpMs) {
        s.error = Error{ErrorCode::Busy, "storage refused the read"};
    }
}

Result<size_t> ItemReader::read(uint8_t* dst, size_t len) {
    Shared& s = *shared_;
    if (len == 0) return Result<size_t>::ok(0);
    for (;;) {
        size_t got = 0;
        {
            std::lock_guard<os::Mutex> lock(s.mu);
            if (s.pos >= s.size) return Result<size_t>::ok(0);
            if (s.pos >= s.base && s.pos < s.base + s.len) {
                const size_t cap = s.ring.size();
                const size_t avail = static_cast<size_t>(s.base + s.len - s.pos);
                got = std::min(len, avail);
                const size_t at = (s.head + static_cast<size_t>(s.pos - s.base)) % cap;
                const size_t first = std::min(got, cap - at);
                std::memcpy(dst, s.ring.data() + at, first);
                std::memcpy(dst + first, s.ring.data(), got - first);
                s.pos += got;
            } else if (s.error) {
                return Result<size_t>::err(s.error->code, s.error->message);
            }
        }
        maybe_refill();
        if (got > 0) return Result<size_t>::ok(got);
        // An underrun: wait for the refill (media spec 4.4), retrying one
        // that was refused at every poll.
        s.wake.take(kPollMs);
        if (deps_.abort && deps_.abort()) {
            return Result<size_t>::err(ErrorCode::Timeout, "aborted");
        }
    }
}

Result<void> ItemReader::seek(uint64_t offset) {
    Shared& s = *shared_;
    {
        std::lock_guard<os::Mutex> lock(s.mu);
        if (offset > s.size) return Result<void>::err(ErrorCode::InvalidArgument, "past the end");
        if (offset >= s.base && offset <= s.base + s.len) {
            s.pos = offset;
        } else {
            ++s.gen;
            s.base = offset;
            s.len = 0;
            s.head = 0;
            s.pos = offset;
        }
    }
    maybe_refill();
    return Result<void>::ok();
}

}  // namespace wisp::media
