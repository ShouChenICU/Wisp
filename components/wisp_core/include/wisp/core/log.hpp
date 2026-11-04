#pragma once

// Portable logging facility: a leveled, tag/printf-style API backed by a
// fixed-size in-memory ring buffer (for later retrieval, e.g. over a
// diagnostics endpoint) plus zero or more LogSink callbacks (e.g. the
// host simulator's stdout sink, or a future serial/network sink on the
// ESP port). This header declares types and functions only; it must never
// include platform headers or expose platform types (see wisp/os/os.hpp
// for the same rule).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "wisp/os/os.hpp"

namespace wisp {

enum class LogLevel { Debug, Info, Warn, Error };

// One accepted log line as the ring stores it and every sink receives it.
// (seq, ts_ms) are assigned together under the log lock, so a sink's copy
// of a line and the ring's copy are identical and ring order is seq order.
struct LogRecord {
    LogLevel level = LogLevel::Info;
    const char* tag = "";
    const char* msg = "";  // may contain '\n'; it is still one line
    // os::now_ms() when the line was accepted: ms since boot (spec 6.2).
    uint64_t ts_ms = 0;
    // This boot's line number: 1 for the first accepted line, +1 for each
    // one after (filtered-out messages take none). Together with the
    // admin API's per-boot id it identifies a line exactly, so a browser
    // can drop the copies a ring replay repeats (spec 6.2). Wraps after
    // 2^32 lines.
    uint32_t seq = 0;
};

// A line read back from the ring (ring_entries()).
struct LogEntry {
    uint32_t seq = 0;
    uint64_t ts_ms = 0;
    LogLevel level = LogLevel::Info;
    std::string tag;
    std::string msg;
};

// A destination for formatted log lines, e.g. stdout on the simulator or
// a UART/network sink on the ESP port. Sinks are invoked outside of
// log's internal lock (see log::write()), so a sink is free to do
// blocking I/O without holding up concurrent logging from other threads;
// a sink must not call back into wisp::log::write() (or the WLOG_* macros)
// itself from within write() — doing so is tolerated (it is still recorded
// into the ring buffer) but is not delivered to any sink, to avoid
// unbounded recursion or deadlock.
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
};

namespace log {

// write() keeps at most this much of a formatted message (it formats into
// a stack buffer of this size, and small task stacks log too).
inline constexpr size_t kMaxMsgBytes = 255;
// write_text() keeps at most this much of a message it is given whole.
inline constexpr size_t kMaxTextBytes = 512;
// Both keep at most this much of a tag: enough for "scene." plus the
// longest scene id (32 bytes, scene_manifest.hpp).
inline constexpr size_t kMaxTagBytes = 47;

// (Re)initializes the logging subsystem with a ring buffer of
// `ring_bytes`, allocated via os::alloc(ring_bytes, mem), replacing any
// previous ring of a different size or memory class (existing sinks and
// the current level are left as-is; see reset_for_test() to clear those
// too). Called again with the ring's current size and memory class, it
// keeps the ring and its lines. Safe to call concurrently
// with write()/add_sink()/set_level()/ring_snapshot() from other
// threads/tasks (e.g. other tasks logging during startup, before or
// during this call) -- those calls are simply serialized against it, not
// racing it. A write() call made before the first init() (or after
// reset_for_test(), before the next init()) is a safe no-op: it records
// nothing into the ring (capacity 0) and is delivered to no sinks (none
// registered yet / cleared).
void init(size_t ring_bytes = 32 * 1024, os::Mem mem = os::Mem::Large);

// Registers `sink` to receive every log line accepted by the current
// level filter. `sink` must stay alive until it is removed with
// remove_sink() (or reset_for_test()); ownership is not taken.
void add_sink(LogSink* sink);

// Unregisters `sink` (a no-op if it is not registered) and waits for any
// delivery to it already in progress on other threads to return, so the
// caller may destroy the sink as soon as this returns. Must not be called
// from within a LogSink::write() call, which would wait for itself.
void remove_sink(LogSink* sink);

// Sets the minimum level that reaches both the ring buffer and the
// sinks; messages below `level` are dropped entirely. Defaults to
// LogLevel::Debug (nothing filtered) after init().
void set_level(LogLevel level);

// Formats `fmt`/`args` (printf-style) into one line and records it with
// the next seq: always into the ring buffer, and (if not currently inside
// a nested call from a sink) delivered to every registered sink. Messages
// at or above the current level only; see set_level(). The formatted
// message is truncated to kMaxMsgBytes and the tag to kMaxTagBytes (the
// copies live on the caller's stack), never inside a UTF-8 character.
// Prefer the WLOG_D/I/W/E macros below over calling this directly.
void write(LogLevel level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// Records `msg` as it is, like write() but without formatting and with
// the longer kMaxTextBytes limit (a longer one is cut there, never inside
// a UTF-8 character): for lines that are built elsewhere, such as a
// scene's print and wisp.log (wisp/scene/vm.hpp). Its frame holds a copy
// only when `msg` must be cut; null `tag` or `msg` count as "".
void write_text(LogLevel level, const char* tag, const char* msg);

// The ring buffer's current lines, oldest (lowest seq) first.
std::vector<LogEntry> ring_entries();

// The same lines as text, each formatted "<ts_ms> <D|I|W|E> <tag>: <msg>\n"
// (a message containing '\n' spans several text lines). For diagnostics
// and tests; use ring_entries() to process lines.
std::string ring_snapshot();

// Tears down all global logging state (ring buffer, sinks, level, seq) so
// a fresh init() can be called as if the process had just started. For
// use by tests only.
void reset_for_test();

}  // namespace log

}  // namespace wisp

#define WLOG_D(tag, fmt, ...) ::wisp::log::write(::wisp::LogLevel::Debug, tag, fmt, ##__VA_ARGS__)
#define WLOG_I(tag, fmt, ...) ::wisp::log::write(::wisp::LogLevel::Info, tag, fmt, ##__VA_ARGS__)
#define WLOG_W(tag, fmt, ...) ::wisp::log::write(::wisp::LogLevel::Warn, tag, fmt, ##__VA_ARGS__)
#define WLOG_E(tag, fmt, ...) ::wisp::log::write(::wisp::LogLevel::Error, tag, fmt, ##__VA_ARGS__)
