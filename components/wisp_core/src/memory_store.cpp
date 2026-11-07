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

Result<void> MemoryStore::load() {
    std::lock_guard<os::Mutex> lock(mutex_);
    notes_.clear();
    next_id_ = 1;

    Result<os::LargeBuffer> read = storage_.read_file_large(path_);
    if (!read) {
        if (read.error().code == ErrorCode::NotFound) return Result<void>::ok();
        WLOG_W(kTag, "reading memory notes failed: %s", read.error().message.c_str());
        return Result<void>::err(read.error().code, read.error().message);
    }

    const std::string_view text(reinterpret_cast<const char*>(read.value().data()),
                                read.value().size());
    Result<Value> doc = from_json(text);
    const Value* list = doc ? doc.value().find("notes") : nullptr;
    if (!doc || list == nullptr || list->type() != Value::Type::List) {
        WLOG_W(kTag, "memory notes file is malformed");
        return Result<void>::err(ErrorCode::InvalidArgument, "malformed");
    }

    uint32_t max_id = 0;
    for (const Value& item : list->as_list()) {
        const Value* id = item.find("id");
        const Value* text_v = item.find("text");
        const Value* updated = item.find("updated");
        if (id == nullptr || text_v == nullptr || updated == nullptr ||
            id->type() != Value::Type::Int || text_v->type() != Value::Type::String ||
            updated->type() != Value::Type::Int) {
            continue;  // Skip a record this version does not recognize.
        }
        MemoryNote note;
        note.id = static_cast<uint32_t>(id->as_int());
        const std::string& t = text_v->as_string();
        note.text = LStr(t.data(), t.size());
        note.updated = updated->as_int();
        max_id = std::max(max_id, note.id);
        notes_.push_back(std::move(note));
    }
    std::sort(notes_.begin(), notes_.end(),
              [](const MemoryNote& a, const MemoryNote& b) { return a.id < b.id; });

    const Value* next = doc.value().find("next_id");
    next_id_ = next != nullptr && next->type() == Value::Type::Int
                   ? static_cast<uint32_t>(next->as_int())
                   : max_id + 1;
    if (next_id_ <= max_id) next_id_ = max_id + 1;
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
    std::lock_guard<os::Mutex> lock(mutex_);
    os::LargeVector<MemoryNote> old = std::move(notes_);
    notes_.clear();

    if (Result<void> r = persist_and_publish_locked(); !r) {
        notes_ = std::move(old);
        return r;
    }
    return Result<void>::ok();
}

}  // namespace wisp::core
