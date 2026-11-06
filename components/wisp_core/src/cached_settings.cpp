#include "wisp/core/cached_settings.hpp"

#include <memory>
#include <mutex>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "settings";
// Failed saves in a row before the user is told.
constexpr int kFailuresToNotify = 3;

}  // namespace

CachedSettingsBackend::CachedSettingsBackend(SettingsBackend& platform, StorageWorker& worker,
                                             std::function<void()> on_persist_failed)
    : platform_(platform), worker_(worker), on_persist_failed_(std::move(on_persist_failed)) {}

CachedSettingsBackend::Slot CachedSettingsBackend::to_slot(const Value& value) {
    Slot slot;
    slot.type = value.type();
    switch (value.type()) {
        case Value::Type::Null:
            break;
        case Value::Type::Bool:
            slot.b = value.as_bool();
            break;
        case Value::Type::Int:
            slot.i = value.as_int();
            break;
        case Value::Type::Double:
            slot.d = value.as_double();
            break;
        case Value::Type::String:
            slot.text.assign(value.as_string().data(), value.as_string().size());
            break;
        case Value::Type::List:
        case Value::Type::Map: {
            const std::string json = to_json_raw(value);
            slot.text.assign(json.data(), json.size());
            break;
        }
    }
    return slot;
}

Value CachedSettingsBackend::from_slot(const Slot& slot) {
    switch (slot.type) {
        case Value::Type::Null:
            return Value();
        case Value::Type::Bool:
            return Value(slot.b);
        case Value::Type::Int:
            return Value(slot.i);
        case Value::Type::Double:
            return Value(slot.d);
        case Value::Type::String:
            return Value(std::string(slot.text.data(), slot.text.size()));
        case Value::Type::List:
        case Value::Type::Map: {
            // Written by to_json_raw() above, so it parses.
            Result<Value> parsed = from_json(std::string_view(slot.text.data(), slot.text.size()));
            return parsed ? std::move(parsed.value()) : Value();
        }
    }
    return Value();
}

CachedSettingsBackend::Name CachedSettingsBackend::make_name(std::string_view ns,
                                                             std::string_view key) {
    Name name;
    name.ns.assign(ns.data(), ns.size());
    name.key.assign(key.data(), key.size());
    return name;
}

Result<void> CachedSettingsBackend::load() {
    // kForever: the job may then point at this stack frame.
    Result<std::vector<SettingsEntry>> loaded =
        Result<std::vector<SettingsEntry>>::err(ErrorCode::Busy, "not loaded");
    WISP_TRY(worker_.run_sync([this, &loaded] { loaded = platform_.load_all(); }, os::kForever));
    if (!loaded) return Result<void>::err(loaded.error().code, loaded.error().message);

    std::lock_guard<os::Mutex> lock(mutex_);
    cache_.clear();
    for (const SettingsEntry& e : loaded.value()) {
        cache_.insert_or_assign(make_name(e.ns, e.key), to_slot(e.value));
    }
    return Result<void>::ok();
}

Result<Value> CachedSettingsBackend::get(const std::string& ns, const std::string& key) {
    WISP_TRY(validate_settings_name(ns));
    WISP_TRY(validate_settings_name(key));
    std::lock_guard<os::Mutex> lock(mutex_);
    auto it = cache_.find(NameView{ns, key});
    if (it == cache_.end()) return Result<Value>::err(ErrorCode::NotFound, ns + "/" + key);
    return Result<Value>::ok(from_slot(it->second));
}

bool CachedSettingsBackend::put_locked(std::string_view ns, std::string_view key,
                                       const Value* value) {
    if (value != nullptr) {
        auto it = cache_.find(NameView{ns, key});
        if (it != cache_.end()) {
            it->second = to_slot(*value);
        } else {
            cache_.emplace(make_name(ns, key), to_slot(*value));
        }
    } else if (auto it = cache_.find(NameView{ns, key}); it != cache_.end()) {
        cache_.erase(it);
    }
    if (dirty_.find(NameView{ns, key}) == dirty_.end()) dirty_.insert(make_name(ns, key));
    return claim_save_locked();
}

bool CachedSettingsBackend::claim_save_locked() {
    if (queued_) return false;
    queued_ = true;
    return true;
}

Result<void> CachedSettingsBackend::set(const std::string& ns, const std::string& key,
                                        const Value& value) {
    WISP_TRY(validate_settings_name(ns));
    WISP_TRY(validate_settings_name(key));
    bool save = false;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        save = put_locked(ns, key, &value);
    }
    if (save) schedule();
    return Result<void>::ok();
}

Result<void> CachedSettingsBackend::erase(const std::string& ns, const std::string& key) {
    WISP_TRY(validate_settings_name(ns));
    WISP_TRY(validate_settings_name(key));
    bool save = false;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        save = put_locked(ns, key, nullptr);
    }
    if (save) schedule();
    return Result<void>::ok();
}

Result<void> CachedSettingsBackend::erase_all() {
    bool save = false;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        cache_.clear();
        // Nothing set before this call needs writing: the erase removes it.
        dirty_.clear();
        erase_pending_ = true;
        save = claim_save_locked();
    }
    if (save) schedule();
    return Result<void>::ok();
}

Result<std::vector<SettingsEntry>> CachedSettingsBackend::load_all() {
    std::vector<SettingsEntry> out;
    std::lock_guard<os::Mutex> lock(mutex_);
    out.reserve(cache_.size());
    for (const auto& [name, slot] : cache_) {
        out.push_back({std::string(name.ns.data(), name.ns.size()),
                       std::string(name.key.data(), name.key.size()), from_slot(slot)});
    }
    return Result<std::vector<SettingsEntry>>::ok(std::move(out));
}

Result<void> CachedSettingsBackend::write_batch(std::span<const SettingsChange> changes) {
    for (const SettingsChange& c : changes) {
        WISP_TRY(validate_settings_name(c.ns));
        WISP_TRY(validate_settings_name(c.key));
    }
    bool save = false;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        for (const SettingsChange& c : changes) {
            if (put_locked(c.ns, c.key, c.value ? &*c.value : nullptr)) save = true;
        }
    }
    if (save) schedule();
    return Result<void>::ok();
}

void CachedSettingsBackend::schedule() {
    auto job = [this] { (void)persist(); };
    if (worker_.submit(job)) return;
    // The worker is not running (boot or shutdown: run_sync() runs the
    // save right here) or its queue is full. With timeout 0 a save that
    // got queued after all still runs later; one that was refused leaves
    // the keys dirty for the next change or flush().
    Result<void> r = worker_.run_sync(job, 0);
    if (!r && r.error().code != ErrorCode::Timeout) {
        WLOG_W(kTag, "settings save not queued (%s); retrying on the next change",
               r.error().message.c_str());
        std::lock_guard<os::Mutex> lock(mutex_);
        queued_ = false;
    }
}

Result<void> CachedSettingsBackend::persist() {
    std::vector<SettingsChange> changes;
    bool erase = false;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        queued_ = false;
        erase = erase_pending_;
        erase_pending_ = false;
        changes.reserve(dirty_.size());
        for (const Name& name : dirty_) {
            SettingsChange c{std::string(name.ns.data(), name.ns.size()),
                             std::string(name.key.data(), name.key.size()), std::nullopt};
            if (auto it = cache_.find(name); it != cache_.end()) c.value = from_slot(it->second);
            changes.push_back(std::move(c));
        }
        dirty_.clear();
        if (!erase && changes.empty()) return Result<void>::ok();
        saving_ = true;
    }

    Result<void> r = Result<void>::ok();
    bool erased = false;
    if (erase) {
        r = platform_.erase_all();
        erased = static_cast<bool>(r);
    }
    if (r && !changes.empty()) r = platform_.write_batch(changes);

    bool notify = false;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        saving_ = false;
        if (r) {
            failures_ = 0;
        } else {
            if (erase && !erased) erase_pending_ = true;
            for (const SettingsChange& c : changes) {
                if (dirty_.find(NameView{c.ns, c.key}) == dirty_.end()) {
                    dirty_.insert(make_name(c.ns, c.key));
                }
            }
            ++failures_;
            notify = failures_ == kFailuresToNotify;
        }
    }
    if (!r) {
        WLOG_W(kTag, "settings not saved (%s); retrying on the next change",
               r.error().message.c_str());
        if (notify && on_persist_failed_) on_persist_failed_();
    }
    return r;
}

Result<void> CachedSettingsBackend::flush(uint32_t timeout_ms) {
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (dirty_.empty() && !erase_pending_ && !saving_) return Result<void>::ok();
    }
    // The job may outlive a timed-out wait, so its result is shared.
    auto out = std::make_shared<Result<void>>(Result<void>::ok());
    WISP_TRY(worker_.run_sync([this, out] { *out = persist(); }, timeout_ms));
    return *out;
}

}  // namespace wisp::core
