#include "wisp/core/memory_store.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "memory";

using LStr = os::LargeString;

std::span<const uint8_t> bytes_of(const LStr& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// Unicode code points in `s` (assumed well-formed UTF-8): every byte that
// is not a continuation byte (10xxxxxx) starts one.
size_t codepoint_count(std::string_view s) {
    size_t n = 0;
    for (unsigned char c : s) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

}  // namespace

MemoryStore::MemoryStore(StorageManager& storage, std::string path, EventBus* bus,
                         std::function<int64_t()> wall_s)
    : storage_(storage), path_(std::move(path)), bus_(bus), wall_s_(std::move(wall_s)) {}

Result<void> MemoryStore::quarantine_locked() {
    const std::string bad_path = path_ + ".bad";
    // Clear any older quarantined file first: StorageManager::rename()
    // refuses an existing target (it must behave the same on FAT, whose
    // rename() can't replace one, as on LittleFS). A missing bad_path is
    // success.
    Result<void> cleared = storage_.remove_tree(bad_path);
    Result<void> renamed = cleared ? storage_.rename(path_, bad_path) : cleared;
    if (!renamed) {
        WLOG_W(kTag, "memory notes file is malformed and could not be moved aside: %s",
               renamed.error().message.c_str());
        load_failed_ = true;
        return Result<void>::err(renamed.error().code, renamed.error().message);
    }
    WLOG_W(kTag, "memory notes file was malformed; moved aside, starting empty");
    load_failed_ = false;
    return Result<void>::ok();
}

Result<void> MemoryStore::load() {
    std::lock_guard<os::Mutex> lock(mutex_);
    notes_.clear();
    next_id_ = 1;

    Result<os::LargeBuffer> read = storage_.read_file_large(path_);
    if (!read) {
        if (read.error().code == ErrorCode::NotFound) {
            load_failed_ = false;
            return Result<void>::ok();
        }
        // Anything else (a read error, or path_ turning out not to be a
        // regular file) means the file's actual contents are simply
        // unknown -- unlike the unparsable case below, there is nothing
        // safe to move aside, so the store refuses to write until a
        // later load() succeeds (see the file header).
        WLOG_W(kTag, "reading memory notes failed: %s", read.error().message.c_str());
        load_failed_ = true;
        return Result<void>::err(read.error().code, read.error().message);
    }

    const std::string_view text(reinterpret_cast<const char*>(read.value().data()),
                                read.value().size());
    Result<Value> doc = from_json(text);
    const Value* list = doc ? doc.value().find("notes") : nullptr;
    const Value* version = doc ? doc.value().find("version") : nullptr;
    const bool version_ok =
        version != nullptr && version->type() == Value::Type::Int && version->as_int() == 1;
    if (!doc || list == nullptr || list->type() != Value::Type::List || !version_ok) {
        return quarantine_locked();
    }

    // Re-validate every record, the same way ModelProfileService::load_models()
    // re-validates stored profiles: a hand-edited file must not be able to
    // smuggle in anything add()/update() would have refused.
    uint32_t max_id = 0;
    size_t dropped = 0;
    for (const Value& item : list->as_list()) {
        const Value* id = item.find("id");
        const Value* text_v = item.find("text");
        const Value* updated = item.find("updated");
        if (id == nullptr || text_v == nullptr || updated == nullptr ||
            id->type() != Value::Type::Int || text_v->type() != Value::Type::String ||
            updated->type() != Value::Type::Int) {
            ++dropped;  // Skip a record this version does not recognize.
            continue;
        }
        const uint32_t note_id = static_cast<uint32_t>(id->as_int());
        // The id counter must stay clear of every id the file ever named,
        // including one dropped just below for some other reason -- it
        // was still handed out once and must not be handed out again.
        max_id = std::max(max_id, note_id);

        const std::string& t = text_v->as_string();
        const bool duplicate =
            std::any_of(notes_.begin(), notes_.end(),
                        [note_id](const MemoryNote& n) { return n.id == note_id; });
        if (note_id == 0 || duplicate || t.empty() || codepoint_count(t) > kMaxChars ||
            notes_.size() >= kMaxNotes) {
            ++dropped;
            continue;
        }
        MemoryNote note;
        note.id = note_id;
        note.text = LStr(t.data(), t.size());
        note.updated = updated->as_int();
        notes_.push_back(std::move(note));
    }
    std::sort(notes_.begin(), notes_.end(),
              [](const MemoryNote& a, const MemoryNote& b) { return a.id < b.id; });
    if (dropped > 0) {
        WLOG_W(kTag, "memory notes: dropped %zu invalid record(s) on load",
               static_cast<size_t>(dropped));
    }

    const Value* next = doc.value().find("next_id");
    next_id_ = next != nullptr && next->type() == Value::Type::Int
                   ? static_cast<uint32_t>(next->as_int())
                   : max_id + 1;
    if (next_id_ <= max_id) next_id_ = max_id + 1;
    load_failed_ = false;
    return Result<void>::ok();
}

os::LargeVector<MemoryNote> MemoryStore::list() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return notes_;
}

size_t MemoryStore::count() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return notes_.size();
}

os::LargeString MemoryStore::serialize_locked() const {
    LStr out;
    out += "{\"version\":1,\"next_id\":";
    out += std::to_string(next_id_);
    out += ",\"notes\":[";
    bool first = true;
    for (const MemoryNote& n : notes_) {
        if (!first) out += ',';
        first = false;
        const Map m{{"id", Value(static_cast<int64_t>(n.id))},
                    {"text", Value(std::string(n.text.data(), n.text.size()))},
                    {"updated", Value(n.updated)}};
        const std::string one = to_json(Value(m));
        out.append(one.data(), one.size());
    }
    out += "]}";
    return out;
}

Result<void> MemoryStore::persist_and_publish_locked() {
    const LStr body = serialize_locked();
    Result<void> written = storage_.write_atomic(path_, bytes_of(body));
    if (!written) return written;
    if (bus_ != nullptr) {
        bus_->publish(Event{"memory.changed", "memory", 0,
                            Properties{{"count", Value(static_cast<int64_t>(notes_.size()))}}});
    }
    return Result<void>::ok();
}

Result<uint32_t> MemoryStore::add(std::string_view text) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (load_failed_ || forgotten_) {
        return Result<uint32_t>::err(ErrorCode::StorageUnavailable, "memory_unavailable");
    }
    if (text.empty()) return Result<uint32_t>::err(ErrorCode::InvalidArgument, "empty");
    if (codepoint_count(text) > kMaxChars) {
        return Result<uint32_t>::err(ErrorCode::InvalidArgument, "too_long");
    }
    if (notes_.size() >= kMaxNotes) {
        return Result<uint32_t>::err(ErrorCode::QuotaExceeded, "memory_full");
    }

    MemoryNote note;
    note.id = next_id_;
    note.text = LStr(text.data(), text.size());
    note.updated = wall_s_ ? wall_s_() : 0;
    notes_.push_back(std::move(note));
    const uint32_t id = next_id_;
    ++next_id_;  // Never reused, even if the write below fails.

    if (Result<void> r = persist_and_publish_locked(); !r) {
        notes_.pop_back();
        return Result<uint32_t>::err(r.error().code, r.error().message);
    }
    return Result<uint32_t>::ok(id);
}

Result<void> MemoryStore::update(uint32_t id, std::string_view text) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (load_failed_ || forgotten_) {
        return Result<void>::err(ErrorCode::StorageUnavailable, "memory_unavailable");
    }
    auto it = std::find_if(notes_.begin(), notes_.end(),
                           [id](const MemoryNote& n) { return n.id == id; });
    if (it == notes_.end()) return Result<void>::err(ErrorCode::NotFound, "not_found");
    if (text.empty()) return Result<void>::err(ErrorCode::InvalidArgument, "empty");
    if (codepoint_count(text) > kMaxChars) {
        return Result<void>::err(ErrorCode::InvalidArgument, "too_long");
    }

    const LStr old_text = it->text;
    const int64_t old_updated = it->updated;
    it->text = LStr(text.data(), text.size());
    it->updated = wall_s_ ? wall_s_() : 0;

    if (Result<void> r = persist_and_publish_locked(); !r) {
        it->text = old_text;
        it->updated = old_updated;
        return r;
    }
    return Result<void>::ok();
}

Result<void> MemoryStore::remove(uint32_t id) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (load_failed_ || forgotten_) {
        return Result<void>::err(ErrorCode::StorageUnavailable, "memory_unavailable");
    }
    auto it = std::find_if(notes_.begin(), notes_.end(),
                           [id](const MemoryNote& n) { return n.id == id; });
    if (it == notes_.end()) return Result<void>::err(ErrorCode::NotFound, "not_found");

    const size_t index = static_cast<size_t>(it - notes_.begin());
    MemoryNote removed = std::move(*it);
    notes_.erase(it);

    if (Result<void> r = persist_and_publish_locked(); !r) {
        notes_.insert(notes_.begin() + static_cast<std::ptrdiff_t>(index), std::move(removed));
        return r;
    }
    return Result<void>::ok();
}

Result<void> MemoryStore::clear() {
    // The explicit override: allowed even while load_failed_ is set, as
    // the file header documents -- it is the one call that still writes
    // (a fresh, empty file) rather than refusing. forget() is final,
    // though: once it ran, nothing (clear() included) may write again.
    std::lock_guard<os::Mutex> lock(mutex_);
    if (forgotten_) return Result<void>::err(ErrorCode::StorageUnavailable, "memory_unavailable");
    os::LargeVector<MemoryNote> old = std::move(notes_);
    notes_.clear();

    if (Result<void> r = persist_and_publish_locked(); !r) {
        notes_ = std::move(old);
        return r;
    }
    load_failed_ = false;
    return Result<void>::ok();
}

void MemoryStore::forget() {
    std::lock_guard<os::Mutex> lock(mutex_);
    notes_.clear();
    forgotten_ = true;
}

}  // namespace wisp::core
