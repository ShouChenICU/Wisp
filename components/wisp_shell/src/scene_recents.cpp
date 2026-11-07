#include "wisp/shell/scene_recents.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"

namespace wisp::shell {

namespace {
constexpr const char* kTag = "shell";
constexpr int64_t kVersion = 1;
}  // namespace

SceneRecents::SceneRecents(core::StorageManager& storage, std::string path,
                           std::function<bool(std::function<void()>)> submit_io)
    : storage_(storage), path_(std::move(path)), submit_io_(std::move(submit_io)) {
    // The timer only queues the write: its thread must not touch flash.
    std::shared_ptr<State> st = st_;
    core::StorageManager* storage_p = &storage_;
    std::string file = path_;
    auto submit = submit_io_;
    timer_ = os::Timer::create("wisp.recents", kSaveDelayMs, false, [st, storage_p, file, submit] {
        if (!submit) return;
        if (!submit([st, storage_p, file] {
                if (auto r = save(*storage_p, file, *st); !r)
                    WLOG_W(kTag, "scene order not saved: %s", r.error().message.c_str());
            })) {
            WLOG_W(kTag, "scene order not saved (storage queue full)");
        }
    });
    if (!timer_) WLOG_W(kTag, "no timer for the scene order; it is saved only at shutdown");
}

SceneRecents::~SceneRecents() {
    if (timer_) timer_->stop();
}

Result<void> SceneRecents::load() {
    auto read = storage_.read_file(path_);
    if (!read) {
        if (read.error().code == ErrorCode::NotFound) return Result<void>::ok();
        return Result<void>::err(read.error().code, read.error().message);
    }
    const std::string text(read.value().begin(), read.value().end());
    auto parsed = from_json(text);
    std::map<std::string, uint64_t> seq;
    uint64_t next = 1;
    bool ok = parsed && parsed.value().type() == Value::Type::Map;
    if (ok) {
        const Value* version = parsed.value().find("version");
        const Value* recent = parsed.value().find("recent");
        ok = version != nullptr && version->type() == Value::Type::Int &&
             version->as_int() == kVersion && recent != nullptr &&
             recent->type() == Value::Type::Map;
        if (ok) {
            for (const auto& [id, v] : recent->as_map()) {
                if (v.type() != Value::Type::Int || v.as_int() <= 0) continue;
                const auto n = static_cast<uint64_t>(v.as_int());
                seq[id] = n;
                next = std::max(next, n + 1);
            }
        }
    }
    if (!ok) {
        WLOG_W(kTag, "%s is malformed; starting a fresh scene order", path_.c_str());
        return Result<void>::ok();
    }
    std::lock_guard<os::Mutex> lock(st_->mutex);
    st_->seq = std::move(seq);
    st_->next = next;
    st_->dirty = false;
    return Result<void>::ok();
}

Result<void> SceneRecents::flush() { return save(storage_, path_, *st_); }

Result<void> SceneRecents::save(core::StorageManager& storage, const std::string& path, State& st) {
    std::string text;
    {
        std::lock_guard<os::Mutex> lock(st.mutex);
        if (!st.dirty || st.frozen) return Result<void>::ok();
        Map recent;
        for (const auto& [id, n] : st.seq) recent[id] = Value(static_cast<int64_t>(n));
        text =
            to_json(Value(Map{{"version", Value(kVersion)}, {"recent", Value(std::move(recent))}}));
        st.dirty = false;
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    Result<void> r = storage.write_atomic(path, std::span<const uint8_t>(bytes, text.size()));
    if (!r) {
        std::lock_guard<os::Mutex> lock(st.mutex);
        st.dirty = true;
    }
    return r;
}

void SceneRecents::changed() {
    // Called with the lock released: Timer::start() may wait for a
    // callback that is returning.
    if (timer_) timer_->start();
}

void SceneRecents::opened(const std::string& id) {
    {
        std::lock_guard<os::Mutex> lock(st_->mutex);
        if (st_->frozen) return;
        // Already the most recent: nothing to write.
        if (auto it = st_->seq.find(id); it != st_->seq.end() && it->second + 1 == st_->next)
            return;
        st_->seq[id] = st_->next++;
        st_->dirty = true;
    }
    changed();
}

void SceneRecents::forget(const std::string& id) {
    {
        std::lock_guard<os::Mutex> lock(st_->mutex);
        if (st_->frozen || st_->seq.erase(id) == 0) return;
        st_->dirty = true;
    }
    changed();
}

void SceneRecents::forget_all() {
    std::lock_guard<os::Mutex> lock(st_->mutex);
    st_->seq.clear();
    st_->dirty = false;
    st_->frozen = true;
}

std::vector<std::string> SceneRecents::order(std::span<const std::string> ids) const {
    std::vector<std::pair<uint64_t, std::string>> keyed;
    keyed.reserve(ids.size());
    {
        std::lock_guard<os::Mutex> lock(st_->mutex);
        for (const std::string& id : ids) {
            auto it = st_->seq.find(id);
            keyed.emplace_back(it != st_->seq.end() ? it->second : 0, id);
        }
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    std::vector<std::string> out;
    out.reserve(keyed.size());
    for (auto& k : keyed) out.push_back(std::move(k.second));
    return out;
}

}  // namespace wisp::shell
