#include "wisp/core/log.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

namespace wisp::log {

namespace {

constexpr size_t kMaxMsgBytes = 255;

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
struct State {
    os::Mutex mutex;
    char* ring_storage = nullptr;
    size_t ring_capacity = 0;
    size_t ring_head = 0;
    size_t ring_len = 0;
    std::vector<LogSink*> sinks;
    std::atomic<int> level{static_cast<int>(LogLevel::Debug)};

    // Appends `line` (length `len`, including its trailing '\n'),
    // dropping whole lines from the front first to make room if needed.
    // Must be called with `mutex` held.
    void push_line(const char* line, size_t len) {
        if (ring_capacity == 0) {
            return;
        }
        if (len > ring_capacity) {
            // Can never fit as a whole line; drop it rather than storing
            // a newline-less fragment, which would break every other
            // line's "ends in \n" invariant that eviction relies on.
            ring_head = 0;
            ring_len = 0;
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

State* g_state = nullptr;

// Set while a sink's write() is running, so that a log call made from
// within a sink (which would otherwise re-enter write() below) is still
// recorded into the ring buffer but not forwarded to any sink -- avoiding
// unbounded recursion (and a sink deadlocking against itself) without
// ever dropping the message from the ring entirely. thread_local because
// different sinks may run concurrently from different threads.
thread_local bool g_in_sink = false;

}  // namespace

void init(size_t ring_bytes, os::Mem mem) {
    if (g_state != nullptr) {
        reset_for_test();
    }
    State* state = new State();
    state->ring_capacity = ring_bytes;
    state->ring_storage = ring_bytes > 0 ? static_cast<char*>(os::alloc(ring_bytes, mem)) : nullptr;
    g_state = state;
}

void add_sink(LogSink* sink) {
    State* state = g_state;
    if (state == nullptr) {
        return;
    }
    std::lock_guard<os::Mutex> lock(state->mutex);
    state->sinks.push_back(sink);
}

void set_level(LogLevel level) {
    State* state = g_state;
    if (state == nullptr) {
        return;
    }
    state->level.store(static_cast<int>(level), std::memory_order_relaxed);
}

void write(LogLevel level, const char* tag, const char* fmt, ...) {
    State* state = g_state;
    if (state == nullptr) {
        return;
    }
    if (static_cast<int>(level) < state->level.load(std::memory_order_relaxed)) {
        return;
    }

    char msg[kMaxMsgBytes + 1];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    msg[kMaxMsgBytes] = '\0';  // guarantee truncation to kMaxMsgBytes bytes

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

    {
        std::lock_guard<os::Mutex> lock(state->mutex);
        state->push_line(line.data(), line.size());
    }

    // Deliver to sinks outside the lock, so a sink that does its own
    // (possibly slow) I/O never holds up concurrent logging from other
    // threads. See g_in_sink's comment for the reentrancy rule.
    if (g_in_sink) {
        return;
    }
    g_in_sink = true;
    std::vector<LogSink*> sinks_copy;
    {
        std::lock_guard<os::Mutex> lock(state->mutex);
        sinks_copy = state->sinks;
    }
    for (LogSink* sink : sinks_copy) {
        sink->write(level, tag, msg);
    }
    g_in_sink = false;
}

std::string ring_snapshot() {
    State* state = g_state;
    if (state == nullptr) {
        return {};
    }
    std::lock_guard<os::Mutex> lock(state->mutex);
    return state->snapshot();
}

void reset_for_test() {
    State* state = g_state;
    g_state = nullptr;
    if (state == nullptr) {
        return;
    }
    if (state->ring_storage != nullptr) {
        os::dealloc(state->ring_storage);
    }
    delete state;
}

}  // namespace wisp::log
