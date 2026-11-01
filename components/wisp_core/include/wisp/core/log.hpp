#pragma once

// Portable logging facility: a leveled, tag/printf-style API backed by a
// fixed-size in-memory ring buffer (for later retrieval, e.g. over a
// diagnostics endpoint) plus zero or more LogSink callbacks (e.g. the
// host simulator's stdout sink, or a future serial/network sink on the
// ESP port). This header declares types and functions only; it must never
// include platform headers or expose platform types (see wisp/os/os.hpp
// for the same rule).

#include <cstddef>
#include <string>

#include "wisp/os/os.hpp"

namespace wisp {

enum class LogLevel { Debug, Info, Warn, Error };

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
    virtual void write(LogLevel level, const char* tag, const char* msg) = 0;
};

namespace log {

// (Re)initializes the logging subsystem with a ring buffer of
// `ring_bytes`, allocated via os::alloc(ring_bytes, mem), replacing any
// previous ring (existing sinks and the current level are left as-is;
// see reset_for_test() to clear those too). Safe to call concurrently
// with write()/add_sink()/set_level()/ring_snapshot() from other
// threads/tasks (e.g. other tasks logging during startup, before or
// during this call) -- those calls are simply serialized against it, not
// racing it. A write() call made before the first init() (or after
// reset_for_test(), before the next init()) is a safe no-op: it records
// nothing into the ring (capacity 0) and is delivered to no sinks (none
// registered yet / cleared).
void init(size_t ring_bytes = 32 * 1024, os::Mem mem = os::Mem::Large);

// Registers `sink` to receive every log line accepted by the current
// level filter, for as long as the process/test runs. `sink` must outlive
// the logging subsystem (or be removed via reset_for_test()); ownership is
// not taken.
void add_sink(LogSink* sink);

// Sets the minimum level that reaches both the ring buffer and the
// sinks; messages below `level` are dropped entirely. Defaults to
// LogLevel::Debug (nothing filtered) after init().
void set_level(LogLevel level);

// Formats `fmt`/`args` (printf-style) into a single line and records it:
// always into the ring buffer, and (if not currently inside a nested call
// from a sink) delivered to every registered sink. Messages at or above
// the current level only; see set_level(). The formatted message is
// truncated to 255 bytes. Prefer the WLOG_D/I/W/E macros below over
// calling this directly.
void write(LogLevel level, const char* tag, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

// Returns the ring buffer's current contents, oldest line first, each
// line formatted as "<ts_ms> <D|I|W|E> <tag>: <msg>\n".
std::string ring_snapshot();

// Tears down all global logging state (ring buffer, sinks, level) so a
// fresh init() can be called as if the process had just started. For use
// by tests only.
void reset_for_test();

}  // namespace log

}  // namespace wisp

#define WLOG_D(tag, fmt, ...) ::wisp::log::write(::wisp::LogLevel::Debug, tag, fmt, ##__VA_ARGS__)
#define WLOG_I(tag, fmt, ...) ::wisp::log::write(::wisp::LogLevel::Info, tag, fmt, ##__VA_ARGS__)
#define WLOG_W(tag, fmt, ...) ::wisp::log::write(::wisp::LogLevel::Warn, tag, fmt, ##__VA_ARGS__)
#define WLOG_E(tag, fmt, ...) ::wisp::log::write(::wisp::LogLevel::Error, tag, fmt, ##__VA_ARGS__)
