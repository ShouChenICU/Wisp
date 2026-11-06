#include "wisp/agent/conversation.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

#include "wisp/agent/limits.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"

namespace wisp::agent {

namespace {

constexpr const char* kTag = "agent";
constexpr const char* kCurrent = "current.jsonl";
constexpr std::string_view kArchivePrefix = "archive-";
constexpr std::string_view kArchiveSuffix = ".jsonl";
// Archives are read in pieces this big: a line can be far longer (a whole
// tool result), but the list only needs the first few lines.
constexpr size_t kReadChunk = 16384;
constexpr size_t kFirstChars = 100;
// Lines nest no deeper than message -> calls -> call.
constexpr uint8_t kMaxDepth = 8;

std::span<const uint8_t> as_bytes(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string_view view(const os::LargeString& s) { return {s.data(), s.size()}; }

// Bytes in the first `max_cp` code points of `s`.
size_t utf8_prefix(std::string_view s, size_t max_cp) {
    size_t cps = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (cps == max_cp) return i;
            ++cps;
        }
    }
    return s.size();
}

template <class S>
void put_uint(S& out, uint64_t v) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, r.ptr);
}

template <class S>
void put_int(S& out, int64_t v) {
    char buf[24];
    auto r = std::to_chars(buf, buf + sizeof(buf), v);
    out.append(buf, r.ptr);
}

template <class S>
void key(S& out, const char* k) {
    out.append(",\"");
    out.append(k);
    out.append("\":");
}

// One line, '\n' included, written straight into PSRAM: a tool result can
// be 64 KiB, and Value/std::string would grow it through internal RAM.
os::LargeString serialize(const Message& m) {
    os::LargeString out;
    out.reserve(96 + m.text.size() + m.text.size() / 8);
    out.append("{\"seq\":");
    put_uint(out, m.seq);
    key(out, "ts");
    put_int(out, m.ts);
    key(out, "role");
    append_json_string(out, to_string(m.role));
    key(out, "turn");
    put_uint(out, m.turn);
    switch (m.role) {
        case Role::User:
            key(out, "text");
            append_json_string(out, view(m.text));
            key(out, "source");
            append_json_string(out, to_string(m.source));
            break;
        case Role::Assistant:
            key(out, "text");
            append_json_string(out, view(m.text));
            key(out, "calls");
            out.push_back('[');
            for (size_t i = 0; i < m.calls.size(); ++i) {
                const ToolCallRec& c = m.calls[i];
                if (i > 0) out.push_back(',');
                out.append("{\"id\":");
                append_json_string(out, c.id);
                key(out, "name");
                append_json_string(out, c.name);
                key(out, "args");
                append_json_string(out, view(c.args));
                out.push_back('}');
            }
            out.push_back(']');
            break;
        case Role::Tool:
            key(out, "call_id");
            append_json_string(out, m.call_id);
            key(out, "name");
            append_json_string(out, m.tool);
            key(out, "ok");
            out.append(m.ok ? "true" : "false");
            key(out, "result");
            append_json_string(out, view(m.text));
            break;
        case Role::Note:
            key(out, "text");
            append_json_string(out, view(m.text));
            break;
        case Role::Summary:
            key(out, "text");
            append_json_string(out, view(m.text));
            key(out, "covers");
            out.push_back('[');
            put_uint(out, m.covers_first);
            out.push_back(',');
            put_uint(out, m.covers_last);
            out.push_back(']');
            break;
        case Role::Marker:
            key(out, "kind");
            append_json_string(out, m.marker);
            if (!m.status.empty()) {
                key(out, "status");
                append_json_string(out, m.status);
            }
            break;
    }
    out.append("}\n");
    return out;
}

std::optional<Role> role_from(std::string_view s) {
    static constexpr Role kRoles[] = {Role::User, Role::Assistant, Role::Tool,
                                      Role::Note, Role::Summary,   Role::Marker};
    for (Role r : kRoles) {
        if (s == to_string(r)) return r;
    }
    return std::nullopt;
}

Source source_from(std::string_view s) {
    if (s == to_string(Source::Voice)) return Source::Voice;
    if (s == to_string(Source::DeviceText)) return Source::DeviceText;
    return Source::Web;
}

const std::string* str_at(const Value& v, const char* k) {
    const Value* f = v.find(k);
    return f != nullptr && f->type() == Value::Type::String ? &f->as_string() : nullptr;
}

std::string str_or(const Value& v, const char* k) {
    const std::string* s = str_at(v, k);
    return s != nullptr ? *s : std::string();
}

os::LargeString large_or(const Value& v, const char* k) {
    const std::string* s = str_at(v, k);
    return s != nullptr ? os::LargeString(s->data(), s->size()) : os::LargeString();
}

int64_t int_or(const Value& v, const char* k) {
    const Value* f = v.find(k);
    return f != nullptr && f->type() == Value::Type::Int ? f->as_int() : 0;
}

// nullopt for a line that is not a message.
std::optional<Message> parse(std::string_view line) {
    auto parsed = from_json(line, kMaxDepth);
    if (!parsed || parsed.value().type() != Value::Type::Map) return std::nullopt;
    const Value& v = parsed.value();
    const std::string* role = str_at(v, "role");
    if (role == nullptr) return std::nullopt;
    std::optional<Role> r = role_from(*role);
    if (!r || int_or(v, "seq") <= 0) return std::nullopt;

    Message m;
    m.role = *r;
    m.seq = static_cast<uint64_t>(int_or(v, "seq"));
    m.ts = int_or(v, "ts");
    m.turn = static_cast<uint32_t>(int_or(v, "turn"));
    switch (m.role) {
        case Role::User:
            m.text = large_or(v, "text");
            m.source = source_from(str_or(v, "source"));
            break;
        case Role::Assistant:
            m.text = large_or(v, "text");
            if (const Value* calls = v.find("calls");
                calls != nullptr && calls->type() == Value::Type::List) {
                for (const Value& c : calls->as_list()) {
                    if (c.type() != Value::Type::Map) continue;
                    m.calls.push_back(
                        ToolCallRec{str_or(c, "id"), str_or(c, "name"), large_or(c, "args")});
                }
            }
            break;
        case Role::Tool: {
            m.call_id = str_or(v, "call_id");
            m.tool = str_or(v, "name");
            const Value* ok = v.find("ok");
            m.ok = ok == nullptr || ok->type() != Value::Type::Bool || ok->as_bool();
            m.text = large_or(v, "result");
            break;
        }
        case Role::Note:
            m.text = large_or(v, "text");
            break;
        case Role::Summary: {
            m.text = large_or(v, "text");
            const Value* covers = v.find("covers");
            if (covers != nullptr && covers->type() == Value::Type::List &&
                covers->as_list().size() == 2) {
                const auto& c = covers->as_list();
                if (c[0].type() == Value::Type::Int) m.covers_first = c[0].as_int();
                if (c[1].type() == Value::Type::Int) m.covers_last = c[1].as_int();
            }
            break;
        }
        case Role::Marker:
            m.marker = str_or(v, "kind");
            m.status = str_or(v, "status");
            break;
    }
    return m;
}

// Calls `fn` with each complete line of `path` (without its '\n') until it
// returns false. A torn last line is skipped.
template <class Fn>
Result<void> for_each_line(core::StorageManager& storage, const std::string& path, Fn&& fn) {
    os::LargeString pending;
    uint64_t offset = 0;
    for (;;) {
        auto chunk = storage.read_range(path, offset, kReadChunk);
        if (!chunk) return Result<void>::err(chunk.error().code, chunk.error().message);
        if (chunk.value().size() == 0) return Result<void>::ok();
        offset += chunk.value().size();
        size_t scan = pending.size();
        pending.append(reinterpret_cast<const char*>(chunk.value().data()), chunk.value().size());
        size_t start = 0;
        size_t nl;
        while ((nl = pending.find('\n', scan)) != os::LargeString::npos) {
            if (!fn(std::string_view(pending.data() + start, nl - start))) {
                return Result<void>::ok();
            }
            start = nl + 1;
            scan = start;
        }
        pending.erase(0, start);
    }
}

}  // namespace

const char* to_string(Role r) {
    switch (r) {
        case Role::User:
            return "user";
        case Role::Assistant:
            return "assistant";
        case Role::Tool:
            return "tool";
        case Role::Note:
            return "note";
        case Role::Summary:
            return "summary";
        case Role::Marker:
            return "marker";
    }
    return "note";
}

const char* to_string(Source s) {
    switch (s) {
        case Source::Voice:
            return "voice";
        case Source::DeviceText:
            return "device_text";
        case Source::Web:
            return "web";
    }
    return "web";
}

ConversationStore::ConversationStore(core::StorageManager& storage, std::string dir)
    : storage_(storage), dir_(std::move(dir)) {}

std::string ConversationStore::current_path() const { return dir_ + "/" + kCurrent; }

std::string ConversationStore::archive_path(uint32_t n) const {
    std::string p = dir_ + "/";
    p.append(kArchivePrefix);
    p += std::to_string(n);
    p.append(kArchiveSuffix);
    return p;
}

Result<void> ConversationStore::load() {
    std::lock_guard<os::Mutex> write_lock(write_mutex_);
    os::LargeVector<Message> loaded;
    uint64_t kept_bytes = 0;

    auto file = storage_.read_file_large(current_path());
    if (!file && file.error().code != ErrorCode::NotFound) {
        return Result<void>::err(file.error().code, file.error().message);
    }
    if (file) {
        const os::LargeBuffer& buf = file.value();
        std::string_view all(reinterpret_cast<const char*>(buf.data()), buf.size());
        const size_t last_nl = all.rfind('\n');
        kept_bytes = last_nl == std::string_view::npos ? 0 : last_nl + 1;
        size_t start = 0;
        while (start < kept_bytes) {
            const size_t nl = all.find('\n', start);
            std::optional<Message> m = parse(all.substr(start, nl - start));
            if (m) {
                loaded.push_back(std::move(*m));
            } else if (nl > start) {
                WLOG_W(kTag, "conversation: skipping an unreadable line at byte %u",
                       static_cast<unsigned>(start));
            }
            start = nl + 1;
        }
        if (kept_bytes < all.size()) {
            WLOG_W(kTag, "conversation: dropping a torn last line (%u bytes)",
                   static_cast<unsigned>(all.size() - kept_bytes));
            WISP_TRY(storage_.write_atomic(current_path(), buf.span().first(kept_bytes)));
        }
    }

    uint64_t max_seq = 0;
    uint32_t max_turn = 0;
    std::optional<uint32_t> open;
    int64_t last_ts = 0;
    for (const Message& m : loaded) {
        max_seq = std::max(max_seq, m.seq);
        max_turn = std::max(max_turn, m.turn);
        last_ts = m.ts;
        if (m.role == Role::Marker) {
            if (m.marker == "turn_begin") {
                open = m.turn;
            } else if (m.marker == "turn_end") {
                open.reset();
            }
        }
    }
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        messages_ = std::move(loaded);
        bytes_ = kept_bytes;
        next_seq_ = max_seq + 1;
        last_turn_ = max_turn;
        interrupted_.reset();
    }

    if (open) {
        WLOG_W(kTag, "conversation: turn %u was cut short; marking it interrupted",
               static_cast<unsigned>(*open));
        Message end;
        end.role = Role::Marker;
        end.turn = *open;
        end.ts = last_ts;
        end.marker = "turn_end";
        end.status = "interrupted";
        auto r = append_locked(std::move(end));
        if (!r) return Result<void>::err(r.error().code, r.error().message);
        std::lock_guard<os::Mutex> lock(mutex_);
        interrupted_ = *open;
    }
    return Result<void>::ok();
}

Result<uint64_t> ConversationStore::append(Message m) {
    std::lock_guard<os::Mutex> write_lock(write_mutex_);
    return append_locked(std::move(m));
}

Result<uint64_t> ConversationStore::append_locked(Message m) {
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        m.seq = next_seq_;
    }
    const os::LargeString line = serialize(m);
    auto r = storage_.append(current_path(), as_bytes(view(line)));
    if (!r) return Result<uint64_t>::err(r.error().code, r.error().message);
    std::lock_guard<os::Mutex> lock(mutex_);
    ++next_seq_;
    bytes_ += line.size();
    last_turn_ = std::max(last_turn_, m.turn);
    const uint64_t seq = m.seq;
    messages_.push_back(std::move(m));
    return Result<uint64_t>::ok(seq);
}

os::LargeVector<Message> ConversationStore::messages() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return messages_;
}

std::optional<Message> ConversationStore::find(uint64_t seq) const {
    std::lock_guard<os::Mutex> lock(mutex_);
    auto it = std::lower_bound(messages_.begin(), messages_.end(), seq,
                               [](const Message& m, uint64_t s) { return m.seq < s; });
    if (it == messages_.end() || it->seq != seq) return std::nullopt;
    return *it;
}

uint64_t ConversationStore::bytes() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return bytes_;
}

bool ConversationStore::needs_rollover() const { return bytes() > kRolloverBytes; }

std::optional<uint32_t> ConversationStore::interrupted_turn() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return interrupted_;
}

uint32_t ConversationStore::last_turn() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return last_turn_;
}

Result<std::vector<uint32_t>> ConversationStore::archive_numbers() {
    using R = Result<std::vector<uint32_t>>;
    std::vector<uint32_t> out;
    auto entries = storage_.list_dir(dir_);
    if (!entries) {
        if (entries.error().code == ErrorCode::NotFound) return R::ok(std::move(out));
        return R::err(entries.error().code, entries.error().message);
    }
    for (const core::DirEntry& e : entries.value()) {
        std::string_view name = e.name;
        if (e.kind != core::EntryKind::File || !name.starts_with(kArchivePrefix) ||
            !name.ends_with(kArchiveSuffix)) {
            continue;
        }
        std::string_view digits = name.substr(
            kArchivePrefix.size(), name.size() - kArchivePrefix.size() - kArchiveSuffix.size());
        uint32_t n = 0;
        auto r = std::from_chars(digits.data(), digits.data() + digits.size(), n);
        if (digits.empty() || r.ec != std::errc() || r.ptr != digits.data() + digits.size() ||
            n == 0) {
            continue;
        }
        out.push_back(n);
    }
    std::sort(out.begin(), out.end(), std::greater<>());
    return R::ok(std::move(out));
}

Result<void> ConversationStore::archive() {
    std::lock_guard<os::Mutex> write_lock(write_mutex_);
    if (bytes() == 0) return Result<void>::ok();
    auto numbers = archive_numbers();
    if (!numbers) return Result<void>::err(numbers.error().code, numbers.error().message);
    std::vector<uint32_t>& ns = numbers.value();
    const uint32_t n = ns.empty() ? 1 : ns.front() + 1;
    WISP_TRY(storage_.rename(current_path(), archive_path(n)));
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        messages_.clear();
        bytes_ = 0;
        next_seq_ = 1;
        last_turn_ = 0;
        interrupted_.reset();
    }
    ns.insert(ns.begin(), n);
    for (size_t i = kArchivesKept; i < ns.size(); ++i) {
        if (auto r = storage_.remove_tree(archive_path(ns[i])); !r) {
            WLOG_W(kTag, "conversation: could not remove archive %u: %s",
                   static_cast<unsigned>(ns[i]), r.error().message.c_str());
        }
    }
    return Result<void>::ok();
}

Result<std::vector<ArchiveInfo>> ConversationStore::archives() {
    using R = Result<std::vector<ArchiveInfo>>;
    std::lock_guard<os::Mutex> write_lock(write_mutex_);
    auto numbers = archive_numbers();
    if (!numbers) return R::err(numbers.error().code, numbers.error().message);
    std::vector<ArchiveInfo> out;
    for (uint32_t n : numbers.value()) {
        ArchiveInfo info;
        info.n = n;
        bool have_ts = false;
        auto r = for_each_line(storage_, archive_path(n), [&](std::string_view line) {
            std::optional<Message> m = parse(line);
            if (!m) return true;
            if (!have_ts) {
                info.ts = m->ts;
                have_ts = true;
            }
            if (m->role != Role::User) return true;
            const std::string_view text = view(m->text);
            info.first.assign(text.substr(0, utf8_prefix(text, kFirstChars)));
            return false;
        });
        if (!r) {
            if (r.error().code == ErrorCode::NotFound) continue;
            return R::err(r.error().code, r.error().message);
        }
        out.push_back(std::move(info));
    }
    return R::ok(std::move(out));
}

Result<os::LargeVector<Message>> ConversationStore::read_archive(uint32_t n) {
    using R = Result<os::LargeVector<Message>>;
    std::lock_guard<os::Mutex> write_lock(write_mutex_);
    os::LargeVector<Message> out;
    auto r = for_each_line(storage_, archive_path(n), [&out](std::string_view line) {
        if (std::optional<Message> m = parse(line)) out.push_back(std::move(*m));
        return true;
    });
    if (!r) return R::err(r.error().code, r.error().message);
    return R::ok(std::move(out));
}

Result<void> ConversationStore::clear_all() {
    std::lock_guard<os::Mutex> write_lock(write_mutex_);
    WISP_TRY(storage_.remove_tree(dir_));
    std::lock_guard<os::Mutex> lock(mutex_);
    messages_.clear();
    bytes_ = 0;
    next_seq_ = 1;
    last_turn_ = 0;
    interrupted_.reset();
    return Result<void>::ok();
}

Value to_value(const Message& m, size_t max_result_chars) {
    auto cut = [max_result_chars](std::string_view s) {
        return std::string(s.substr(0, utf8_prefix(s, max_result_chars)));
    };
    Map out{
        {"seq", static_cast<int64_t>(m.seq)},
        {"ts", m.ts},
        {"turn", static_cast<int64_t>(m.turn)},
        {"role", to_string(m.role)},
    };
    switch (m.role) {
        case Role::User:
            out["text"] = std::string(view(m.text));
            out["source"] = to_string(m.source);
            break;
        case Role::Assistant: {
            out["text"] = std::string(view(m.text));
            List calls;
            for (const ToolCallRec& c : m.calls) {
                calls.push_back(Map{{"id", c.id},
                                    {"name", c.name},
                                    {"args", cut(view(c.args))},
                                    {"args_len", static_cast<int64_t>(c.args.size())}});
            }
            out["calls"] = std::move(calls);
            break;
        }
        case Role::Tool:
            out["call_id"] = m.call_id;
            out["name"] = m.tool;
            out["ok"] = m.ok;
            out["result"] = cut(view(m.text));
            out["result_len"] = static_cast<int64_t>(m.text.size());
            break;
        case Role::Note:
            out["text"] = std::string(view(m.text));
            break;
        case Role::Summary:
            out["text"] = std::string(view(m.text));
            out["covers"] =
                List{static_cast<int64_t>(m.covers_first), static_cast<int64_t>(m.covers_last)};
            break;
        case Role::Marker:
            out["kind"] = m.marker;
            if (!m.status.empty()) out["status"] = m.status;
            break;
    }
    return Value(std::move(out));
}

}  // namespace wisp::agent
