#pragma once

// Helpers the device and scene tools share: running a job on the storage
// worker from the agent's thread, and the tool error code of an Error.

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/result.hpp"
#include "wisp/os/os.hpp"

namespace wisp::agent::detail {

using SubmitIo = std::function<bool(std::function<void()>)>;

// Runs `fn` through `submit` and waits up to `timeout_ms`, giving up early
// when ctx.cancelled(). The job shares only a reference-counted state with
// this call, so once given up on it still runs to the end and its result is
// dropped: `fn` must not capture anything on the caller's stack by
// reference. Unsupported without a submit function, Busy when the job could
// not be queued, Timeout and Busy("cancelled") when given up on.
template <typename T>
Result<T> run_io(const SubmitIo& submit, std::function<Result<T>()> fn, uint32_t timeout_ms,
                 const ToolCtx& ctx) {
    struct State {
        os::Semaphore done{1, 0};
        std::function<Result<T>()> fn;
        std::optional<Result<T>> out;
    };
    if (!submit) return Result<T>::err(ErrorCode::Unsupported, "no storage worker");
    auto s = std::make_shared<State>();
    s->fn = std::move(fn);
    if (!submit([s] {
            s->out.emplace(s->fn());
            s->fn = nullptr;
            s->done.give();
        })) {
        return Result<T>::err(ErrorCode::Busy, "storage worker busy");
    }
    constexpr uint32_t kPollMs = 20;
    for (uint32_t waited = 0;; waited += kPollMs) {
        if (s->done.take(kPollMs)) return std::move(*s->out);
        if (ctx.cancelled && ctx.cancelled()) {
            return Result<T>::err(ErrorCode::Busy, "cancelled");
        }
        if (waited >= timeout_ms) return Result<T>::err(ErrorCode::Timeout, "storage timeout");
    }
}

// The tool error code (agent-core spec 4) of a service error.
inline const char* tool_code(ErrorCode c) {
    switch (c) {
        case ErrorCode::NotFound:
            return "not_found";
        case ErrorCode::Busy:
            return "busy";
        case ErrorCode::Timeout:
            return "timeout";
        case ErrorCode::QuotaExceeded:
            return "quota_exceeded";
        case ErrorCode::InvalidArgument:
            return "invalid_value";
        case ErrorCode::PermissionDenied:
            return "denied";
        case ErrorCode::Unsupported:
            return "unsupported";
        case ErrorCode::StorageUnavailable:
            return "unavailable";
        default:
            return "failed";
    }
}

inline ToolOutput fail(const Error& e) {
    if (e.code == ErrorCode::Busy && e.message == "cancelled") {
        return tool_error("cancelled", e.message);
    }
    return tool_error(tool_code(e.code), e.message);
}

inline ToolOutput done(Map fields = {}, std::string note = {}) {
    fields["ok"] = Value(true);
    ToolOutput out;
    out.result = Value(std::move(fields));
    out.note = std::move(note);
    return out;
}

inline const std::string& arg_str(const Value& args, const char* key) {
    static const std::string kEmpty;
    const Value* v = args.find(key);
    return v != nullptr && v->type() == Value::Type::String ? v->as_string() : kEmpty;
}

// Whether `s` is UTF-8 a JSON string can carry as it is.
inline bool is_text(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80           ? 1
                   : (c >> 5) == 0x6  ? 2
                   : (c >> 4) == 0xE  ? 3
                   : (c >> 3) == 0x1E ? 4
                                      : 0;
        if (n == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t') || i + n > s.size())
            return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

}  // namespace wisp::agent::detail
