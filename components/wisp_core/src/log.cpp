#include "wisp/core/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace wisp::log {

namespace {

constexpr size_t kMaxMsgBytes = 255;

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

// All global logging state, protected by `mutex` (every field below is
// only ever touched while holding it), except `level`, which is read on
// every write() call and so is kept as a plain atomic instead of forcing
// an extra lock just to filter out a message that will be dropped anyway.
//
// This struct is never heap-allocated or freed (see state() below): the
// one instance lives for the life of the process, and init()/
// reset_for_test() only ever swap its *contents* (the ring storage
// pointer, sink list, level) in place, under `mutex`. This means a
// concurrent write()/add_sink()/set_level()/ring_snapshot() can never
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

    // Appends `line` (length `len`, including its trailing '\n'),
    // dropping whole lines from the front first to make room if needed.
    // Must be called with `mutex` held.
    void push_line(const char* line, size_t len) {
        if (ring_capacity == 0) {
            return;
        }
        if (len > ring_capacity) {
            // Can never fit as a whole line, even in an empty ring.
            // Drop just this line and leave the existing contents alone
            // (storing a newline-less fragment instead would break
            // every other line's "ends in \n" invariant that eviction
            // relies on, and clearing the ring would needlessly destroy
            // perfectly good, unrelated history).
            return;
        }
        while (ring_len + len > ring_capacity) {
            evict_oldest_line();
        }
        size_t write_at = (ring_head + ring_len) % ring_capacity;
        size_t first_chunk = std::min(len, ring_capacity - write_at);
        std::memcpy(ring_storage + write_at, line, first_chunk);
        if (first_chunk < len) {
            std::memcpy(ring_storage, line + first_chunk, len - first_chunk);
        }
        ring_len += len;
    }

    // Must be called with `mutex` held.
    std::string snapshot() const {
        std::string out(ring_len, '\0');
        size_t first_chunk = std::min(ring_len, ring_capacity - ring_head);
        std::memcpy(out.data(), ring_storage + ring_head, first_chunk);
        if (first_chunk < ring_len) {
            std::memcpy(out.data() + first_chunk, ring_storage, ring_len - first_chunk);
        }
        return out;
    }

    // Resets the ring to `new_capacity` bytes (dropping any existing
    // contents) and, if `clear_sinks_and_level` is set, also clears the
    // sink list and restores the default level. Must be called with
    // `mutex` held.
    void reinit_ring(size_t new_capacity, os::Mem mem, bool clear_sinks_and_level) {
        if (ring_storage != nullptr) {
            os::dealloc(ring_storage);
        }
        ring_storage =
            new_capacity > 0 ? static_cast<char*>(os::alloc(new_capacity, mem)) : nullptr;
        ring_capacity = new_capacity;
        ring_head = 0;
        ring_len = 0;
        if (clear_sinks_and_level) {
            sinks.clear();
            level.store(static_cast<int>(LogLevel::Debug), std::memory_order_relaxed);
        }
    }

private:
    // Drops the oldest stored line (up to and including its '\n'). Must
    // be called with `mutex` held, and only when ring_len > 0.
    void evict_oldest_line() {
        for (size_t i = 0; i < ring_len; ++i) {
            if (ring_storage[(ring_head + i) % ring_capacity] == '\n') {
                size_t remove = i + 1;
                ring_head = (ring_head + remove) % ring_capacity;
                ring_len -= remove;
                return;
            }
        }
        // No newline found among the stored bytes; every line this class
        // ever stores ends in '\n', so this should not happen. Drop
        // everything rather than looping forever.
        ring_head = 0;
        ring_len = 0;
    }
};

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

}  // namespace

void init(size_t ring_bytes, os::Mem mem) {
    State& s = state();
    std::lock_guard<os::Mutex> lock(s.mutex);
    s.reinit_ring(ring_bytes, mem, /*clear_sinks_and_level=*/false);
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

void write(LogLevel level, const char* tag, const char* fmt, ...) {
    State& s = state();
    if (static_cast<int>(level) < s.level.load(std::memory_order_relaxed)) {
        return;
    }

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

    uint64_t ts = os::now_ms();
    char ts_buf[24];
    std::snprintf(ts_buf, sizeof(ts_buf), "%llu", static_cast<unsigned long long>(ts));

    std::string line;
    line.reserve(sizeof(ts_buf) + std::strlen(tag) + kMaxMsgBytes + 8);
    line += ts_buf;
    line += ' ';
    line += level_char(level);
    line += ' ';
    line += tag;
    line += ": ";
    line += msg;
    line += '\n';

    // Deliver to sinks outside the lock, so a sink that does its own
    // (possibly slow) I/O never holds up concurrent logging from other
    // threads. See g_in_sink's comment for the reentrancy rule and
    // State::deliveries for how remove_sink() waits these out.
    const bool deliver = !g_in_sink;
    std::vector<LogSink*> sinks_copy;
    uint32_t epoch = 0;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        s.push_line(line.data(), line.size());
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
        sink->write(level, tag, msg);
    }
    g_in_sink = false;
    {
        std::lock_guard<os::Mutex> lock(s.mutex);
        --s.deliveries[epoch];
    }
}

std::string ring_snapshot() {
    State& s = state();
    std::lock_guard<os::Mutex> lock(s.mutex);
    return s.snapshot();
}

void reset_for_test() {
    State& s = state();
    std::lock_guard<os::Mutex> lock(s.mutex);
    s.reinit_ring(0, os::Mem::Large, /*clear_sinks_and_level=*/true);
}

}  // namespace wisp::log
