#include "wisp/core/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "wisp/os/flash_guard.hpp"

namespace wisp::log {

namespace {

bool is_utf8_continuation(char c) { return (static_cast<uint8_t>(c) & 0xC0) == 0x80; }

// Where to cut `text` (at least limit + 1 bytes long) to keep at most
// `limit` bytes without splitting a UTF-8 character: the sinks (the admin
// WebSocket among them) pass messages to browsers, which reject truncated
// sequences. If the first dropped byte continues a character, that
// character's lead byte is at most 3 bytes back and the cut moves to it.
// Bytes that are not well-formed UTF-8 to begin with are cut at `limit`.
size_t cut_at_char_boundary(const char* text, size_t limit) {
    if (!is_utf8_continuation(text[limit])) return limit;
    size_t lead = limit;
    while (lead > 0 && limit - lead < 3 && is_utf8_continuation(text[lead - 1])) --lead;
    if (lead > 0 && (static_cast<uint8_t>(text[lead - 1]) & 0xC0) == 0xC0) return lead - 1;
    return limit;
}

char level_char(LogLevel level) {
    switch (level) {
        case LogLevel::Debug:
            return 'D';
        case LogLevel::Info:
            return 'I';
        case LogLevel::Warn:
            return 'W';
        case LogLevel::Error:
            return 'E';
    }
    return '?';
}

// Each ring entry is a fixed 16-byte header followed by the tag and the
// message bytes. The header (rather than a delimiter) carries the entry's
// length, so eviction is O(1) per entry and a message may contain any
// byte, '\n' included, without ever leaving a fragment at the ring's head;
// seq/ts/level are stored once in binary instead of being re-parsed from
// text. 16 bytes is about what the "<ts> <L> <tag>: " text prefix took.
struct EntryHeader {
    uint64_t ts_ms;
    uint32_t seq;
    uint16_t msg_len;
    uint8_t tag_len;
    uint8_t level;
};
static_assert(sizeof(EntryHeader) == 16, "ring entries assume a 16-byte header");

// All global logging state, protected by `mutex` (every field below is
// only ever touched while holding it), except `level`, which is read on
// every write() call and so is kept as a plain atomic instead of forcing
// an extra lock just to filter out a message that will be dropped anyway.
//
// This struct is never heap-allocated or freed (see state() below): the
// one instance lives for the life of the process, and init()/
// reset_for_test() only ever swap its *contents* (the ring storage
// pointer, sink list, level) in place, under `mutex`. This means a
// concurrent write()/add_sink()/set_level()/ring_entries() can never
// observe a dangling State or a freed ring buffer -- it either runs
// before or after a given init()/reset_for_test() call (serialized by
// `mutex`), never during a half-updated one. Before the first init() (or
// right after reset_for_test()), ring_capacity == 0 and sinks is empty,
// so write() just records nothing into the ring and delivers to no
// sinks -- a safe, documented no-op rather than a special "not
// initialized" state.
struct State {
    os::Mutex mutex;
    char* ring_storage = nullptr;
    size_t ring_capacity = 0;
    size_t ring_head = 0;
    size_t ring_len = 0;
    os::Mem ring_mem = os::Mem::Large;
    // The last seq handed out; survives init() so seq never repeats within
    // a boot.
    uint32_t last_seq = 0;
    std::vector<LogSink*> sinks;
    std::atomic<int> level{static_cast<int>(LogLevel::Debug)};

    // Sink deliveries in progress, counted per epoch. write() delivers to
    // a copy of `sinks` outside the lock, so removing a sink from the list
    // is not enough to stop it being called: remove_sink() flips the epoch
    // (deliveries starting afterwards copy a list without the sink) and
    // then waits for the old epoch's count to reach zero. remove_mutex
    // serializes removals, so the epoch being waited on only ever holds
    // deliveries that started before the flip and the wait cannot starve
    // under continuous logging. Never reset: in-flight writes still
    // decrement the slot they incremented.
    uint32_t delivery_epoch = 0;
    uint32_t deliveries[2] = {0, 0};
    os::Mutex remove_mutex;

    // Appends one entry, dropping whole entries from the front first to
    // make room. An entry larger than the whole ring is dropped by itself,
    // leaving the existing history alone. Must be called with `mutex` held.
    void push_entry(const EntryHeader& h, const char* tag, const char* msg) {
        const size_t size = sizeof(EntryHeader) + h.tag_len + h.msg_len;
        if (ring_capacity == 0 || size > ring_capacity) return;
        while (ring_len + size > ring_capacity) evict_oldest();
        size_t at = (ring_head + ring_len) % ring_capacity;
        at = put(at, &h, sizeof h);
        at = put(at, tag, h.tag_len);
        put(at, msg, h.msg_len);
        ring_len += size;
    }

    // Copies the used part of the ring, oldest entry first, into `out`
    // (ring_len bytes). Must be called with `mutex` held.
    void copy_used(char* out) const {
        if (ring_len > 0) get(ring_head, out, ring_len);
    }

    // Resets the ring to `new_capacity` bytes (dropping any existing
    // contents) and, if `full_reset` is set, also clears the sink list,
    // restores the default level and restarts seq. Must be called with
    // `mutex` held.
    void reinit_ring(size_t new_capacity, os::Mem mem, bool full_reset) {
        if (ring_storage != nullptr) {
            os::dealloc(ring_storage);
        }
        ring_storage =
            new_capacity > 0 ? static_cast<char*>(os::alloc(new_capacity, mem)) : nullptr;
        ring_capacity = ring_storage != nullptr ? new_capacity : 0;
        ring_mem = mem;
        ring_head = 0;
        ring_len = 0;
        if (full_reset) {
            sinks.clear();
            level.store(static_cast<int>(LogLevel::Debug), std::memory_order_relaxed);
            last_seq = 0;
        }
    }

private:
    // Copy `n` bytes into / out of the ring at offset `at`, wrapping at the
    // end; return the offset just past them.
    size_t put(size_t at, const void* src, size_t n) {
        const auto* p = static_cast<const char*>(src);
        const size_t first = std::min(n, ring_capacity - at);
        std::memcpy(ring_storage + at, p, first);
        if (first < n) std::memcpy(ring_storage, p + first, n - first);
        return (at + n) % ring_capacity;
    }

    size_t get(size_t at, void* dst, size_t n) const {
        auto* p = static_cast<char*>(dst);
        const size_t first = std::min(n, ring_capacity - at);
        std::memcpy(p, ring_storage + at, first);
        if (first < n) std::memcpy(p + first, ring_storage, n - first);
        return (at + n) % ring_capacity;
    }

    // Drops the oldest entry. Must be called with `mutex` held, and only
    // when ring_len > 0.
    void evict_oldest() {
        EntryHeader h;
        get(ring_head, &h, sizeof h);
        const size_t size = sizeof(EntryHeader) + h.tag_len + h.msg_len;
        ring_head = (ring_head + size) % ring_capacity;
        ring_len -= size;
    }
};

// Decodes ring bytes laid out oldest first (State::copy_used()).
std::vector<LogEntry> decode(const char* p, size_t len) {
    std::vector<LogEntry> out;
    size_t at = 0;
    while (len - at >= sizeof(EntryHeader)) {
        EntryHeader h;
        std::memcpy(&h, p + at, sizeof h);
        at += sizeof h;
        LogEntry e;
        e.seq = h.seq;
        e.ts_ms = h.ts_ms;
        e.level = static_cast<LogLevel>(h.level);
        e.tag.assign(p + at, h.tag_len);
        at += h.tag_len;
        e.msg.assign(p + at, h.msg_len);
        at += h.msg_len;
        out.push_back(std::move(e));
    }
    return out;
}

// A single, function-local-static instance: constructed on first use,
// never destroyed or reallocated, so no caller ever holds a pointer to a
// State that init()/reset_for_test() could free out from under it (see
// State's own comment above for why that makes the whole thing race- and
// use-after-free-safe).
State& state() {
    static State instance;
    return instance;
}

// Set while a sink's write() is running, so that a log call made from
// within a sink (which would otherwise re-enter write() below) is still
// recorded into the ring buffer but not forwarded to any sink -- avoiding
// unbounded recursion (and a sink deadlocking against itself) without
// ever dropping the message from the ring entirely. thread_local because
// different sinks may run concurrently from different threads.
thread_local bool g_in_sink = false;

// Installed on os::flash_guard (wisp/os/flash_guard.hpp) below: wisp_os
// cannot call WLOG_E itself (its own layer sits below wisp_core --
// AGENTS.md section 3), so a device-build flash-thread violation reaches
// this layer's logging through that hook instead of a direct call.
void report_flash_violation(const char* what) {
    WLOG_E("flash", "flash access off the flash thread: %s", what);
}

}  // namespace

void init(size_t ring_bytes, os::Mem mem) {
    // Idempotent, so installing it on every init() call (including the
    // simulator's early one before App::start() re-inits) is harmless;
    // this is simply the first point every boot path already reaches.
    os::set_flash_violation_reporter(&report_flash_violation);

    State& s = state();
    std::lock_guard<os::Mutex> lock(s.mutex);
    // The simulator initializes logging before App::start() does it again;
    // the lines logged in between must survive that.
    if (s.ring_storage != nullptr && s.ring_capacity == ring_bytes && s.ring_mem == mem) return;
    s.reinit_ring(ring_bytes, mem, /*full_reset=*/false);
}

void add_sink(LogSink* sink) {
    State& s = state();
    std::lock_guard<os::Mutex> lock(s.mutex);
    s.sinks.push_back(sink);
}

void remove_sink(LogSink* sink) {
    State& s = state();
    std::lock_guard<os::Mutex> serialize(s.remove_mutex);
    uint32_t old_epoch = 0;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        s.sinks.erase(std::remove(s.sinks.begin(), s.sinks.end(), sink), s.sinks.end());
        old_epoch = s.delivery_epoch;
        s.delivery_epoch ^= 1;
    }
    while (true) {
        {
            std::lock_guard<os::Mutex> lock(s.mutex);
            if (s.deliveries[old_epoch] == 0) return;
        }
        os::sleep_ms(1);
    }
}

void set_level(LogLevel level) {
    state().level.store(static_cast<int>(level), std::memory_order_relaxed);
}

namespace {

// Records a finished message (already within its limit) under `tag`, cut
// to kMaxTagBytes: into the ring, then to the sinks.
void record(LogLevel level, const char* tag, const char* msg) {
    State& s = state();
    if (tag == nullptr) tag = "";
    char tag_buf[kMaxTagBytes + 1];
    const size_t full_tag = std::strlen(tag);
    if (full_tag > kMaxTagBytes) {
        const size_t cut = cut_at_char_boundary(tag, kMaxTagBytes);
        std::memcpy(tag_buf, tag, cut);
        tag_buf[cut] = '\0';
        tag = tag_buf;
    }

    LogRecord record;
    record.level = level;
    record.tag = tag;
    record.msg = msg;

    // seq and ts are taken under the lock that orders the ring, so ring
    // order is seq order and every sink sees exactly the ring's values.
    // Sinks run outside the lock, so a sink that does its own (possibly
    // slow) I/O never holds up concurrent logging from other threads. See
    // g_in_sink's comment for the reentrancy rule and State::deliveries
    // for how remove_sink() waits these out.
    const bool deliver = !g_in_sink;
    std::vector<LogSink*> sinks_copy;
    uint32_t epoch = 0;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        record.seq = ++s.last_seq;
        record.ts_ms = os::now_ms();
        EntryHeader h{};
        h.ts_ms = record.ts_ms;
        h.seq = record.seq;
        h.msg_len = static_cast<uint16_t>(std::strlen(msg));
        h.tag_len = static_cast<uint8_t>(std::strlen(tag));
        h.level = static_cast<uint8_t>(level);
        s.push_entry(h, tag, msg);
        if (deliver && !s.sinks.empty()) {
            sinks_copy = s.sinks;
            epoch = s.delivery_epoch;
            ++s.deliveries[epoch];
        }
    }
    if (sinks_copy.empty()) {
        return;
    }
    g_in_sink = true;
    for (LogSink* sink : sinks_copy) {
        sink->write(record);
    }
    g_in_sink = false;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        --s.deliveries[epoch];
    }
}

bool filtered(LogLevel level) {
    return static_cast<int>(level) < state().level.load(std::memory_order_relaxed);
}

// The longer of the two cut copies below must fit the ring's 16-bit length.
static_assert(kMaxTextBytes < UINT16_MAX && kMaxTagBytes < UINT8_MAX);

}  // namespace

void write(LogLevel level, const char* tag, const char* fmt, ...) {
    if (filtered(level)) return;
    // One byte past the limit is rendered too, so the cut can tell whether
    // it would split a character.
    char msg[kMaxMsgBytes + 2];
    va_list args;
    va_start(args, fmt);
    const int full = std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (full > static_cast<int>(kMaxMsgBytes)) {
        msg[cut_at_char_boundary(msg, kMaxMsgBytes)] = '\0';
    }
    record(level, tag, msg);
}

namespace {

// Kept out of write_text() so that only a message that must be cut pays
// for the copy's stack.
[[gnu::noinline]] void record_cut(LogLevel level, const char* tag, const char* msg) {
    char cut[kMaxTextBytes + 1];
    const size_t at = cut_at_char_boundary(msg, kMaxTextBytes);
    std::memcpy(cut, msg, at);
    cut[at] = '\0';
    record(level, tag, cut);
}

}  // namespace

void write_text(LogLevel level, const char* tag, const char* msg) {
    if (filtered(level)) return;
    if (msg == nullptr) msg = "";
    if (std::strlen(msg) > kMaxTextBytes) {
        record_cut(level, tag, msg);
    } else {
        record(level, tag, msg);
    }
}

std::vector<LogEntry> ring_entries() {
    State& s = state();
    // Only the byte copy happens under the lock; the per-entry strings are
    // built afterwards, so a subscriber's replay does not hold up loggers.
    char* raw = nullptr;
    size_t len = 0;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        len = s.ring_len;
        if (len == 0) return {};
        raw = static_cast<char*>(os::alloc(len, os::Mem::Large));
        if (raw == nullptr) return {};
        s.copy_used(raw);
    }
    std::vector<LogEntry> out = decode(raw, len);
    os::dealloc(raw);
    return out;
}

std::string ring_snapshot() {
    std::string out;
    for (const LogEntry& e : ring_entries()) {
        char head[40];
        std::snprintf(head, sizeof(head), "%llu %c ", static_cast<unsigned long long>(e.ts_ms),
                      level_char(e.level));
        out += head;
        out += e.tag;
        out += ": ";
        out += e.msg;
        out += '\n';
    }
    return out;
}

void reset_for_test() {
    State& s = state();
    std::lock_guard<os::Mutex> lock(s.mutex);
    s.reinit_ring(0, os::Mem::Large, /*full_reset=*/true);
}

}  // namespace wisp::log
