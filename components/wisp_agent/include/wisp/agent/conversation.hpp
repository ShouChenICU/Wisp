#pragma once

// ConversationStore (agent-core spec 3.5): the current conversation and its
// archives, as JSON Lines under <System>/agent/.
//
//   current.jsonl          one message per line, appended as it happens
//   archive-<n>.jsonl      earlier conversations; n only grows, and only
//                          the kArchivesKept largest are kept
//
// Each line is {"seq","ts","role","turn",...} with the role's own keys:
//   user       text, source ("voice" | "device_text" | "web")
//   assistant  text, calls [{id, name (dotted), args (the model's text)}]
//   tool       call_id, name (dotted), ok, result (the whole result JSON)
//   note       text
//   summary    text, covers [first_seq, last_seq]; stands in for those
//              messages in the model context (context.hpp), an empty text
//              meaning they were dropped without a summary
//   marker     kind ("turn_begin" | "turn_end"), status (turn_end: "done",
//              "stopped", "step_limit", "time_limit", "error", "interrupted")
//
// Appends go through StorageManager::append(), the same exception to
// write_atomic() as SceneStore's log: a torn tail (a last line without its
// '\n') is dropped by load(), which then rewrites the file whole with
// write_atomic() so the next append starts on a fresh line.
//
// Threads: every method that touches files must run on the wisp.fs worker
// (the caller's job). history(), find(), bytes(), needs_rollover(),
// interrupted_turn() and last_turn() read the in-memory copy under a mutex
// and are safe from any thread.
//
// Memory: the history can approach kRolloverBytes with well over a
// thousand tool calls, so every string and vector a Message owns is in
// Mem::Large (call ids and dotted names exceed the small-string buffer and
// would otherwise land in internal RAM). history() hands out a shared,
// immutable snapshot instead of a copy: the agent loop reads it every step,
// usually as a std::span<const Message>. An append copies the history only
// while some older snapshot is still held; otherwise it grows in place.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::agent {

enum class Role : uint8_t { User, Assistant, Tool, Note, Summary, Marker };
// "user" "assistant" "tool" "note" "summary" "marker"
const char* to_string(Role r);

// Where a user message came from.
enum class Source : uint8_t { Voice, DeviceText, Web };
// "voice" "device_text" "web"
const char* to_string(Source s);

struct ToolCallRec {
    os::LargeString id;
    os::LargeString name;  // dotted
    os::LargeString args;
};

struct Message {
    uint64_t seq = 0;
    int64_t ts = 0;  // unix s, 0 while the clock is unset
    uint32_t turn = 0;
    Role role = Role::User;
    os::LargeString text;                // user/assistant/note/summary text; tool result JSON
    Source source = Source::Web;         // user
    os::LargeVector<ToolCallRec> calls;  // assistant
    os::LargeString call_id, tool;       // tool
    bool ok = true;                      // tool
    os::LargeString marker, status;      // marker
    uint64_t covers_first = 0, covers_last = 0;  // summary
};

using History = os::LargeVector<Message>;

struct ArchiveInfo {
    uint32_t n = 0;
    int64_t ts = 0;     // the first message's
    std::string first;  // the first user message, cut to 100 code points
};

class ConversationStore {
public:
    // `dir` is <System>/agent.
    ConversationStore(core::StorageManager& storage, std::string dir);

    ConversationStore(const ConversationStore&) = delete;
    ConversationStore& operator=(const ConversationStore&) = delete;

    // Reads current.jsonl (a missing file is an empty conversation). A turn
    // begun but never ended gets a turn_end "interrupted" marker appended,
    // and interrupted_turn() names it.
    Result<void> load();

    // Assigns the next seq, appends one line and keeps the message. Returns
    // the seq.
    Result<uint64_t> append(Message m);

    // The current conversation as of now; never null. Later appends do not
    // change a snapshot already handed out.
    std::shared_ptr<const History> history() const;
    std::optional<Message> find(uint64_t seq) const;
    // Size of current.jsonl.
    uint64_t bytes() const;
    bool needs_rollover() const;  // bytes() > kRolloverBytes
    std::optional<uint32_t> interrupted_turn() const;
    uint32_t last_turn() const;

    // Moves current.jsonl to archive-<n>.jsonl and starts an empty
    // conversation (seq and turn start over); an empty conversation is not
    // archived. Keeps the kArchivesKept newest archives.
    Result<void> archive();
    // Newest first.
    Result<std::vector<ArchiveInfo>> archives();
    // NotFound for an archive that does not exist.
    Result<History> read_archive(uint32_t n);
    // Removes the current conversation and every archive.
    Result<void> clear_all();

private:
    std::string current_path() const;
    std::string archive_path(uint32_t n) const;
    Result<std::vector<uint32_t>> archive_numbers();
    Result<uint64_t> append_locked(Message m);
    // Under mutex_: makes history_ safe to modify (copying it when a
    // snapshot is out) or replaces it with `fresh`.
    History& writable_history();
    void reset_history(History fresh);

    core::StorageManager& storage_;
    std::string dir_;

    // Serializes the file operations, so seq order is file order; held
    // across I/O, unlike mutex_, which readers on other threads take.
    os::Mutex write_mutex_;
    mutable os::Mutex mutex_;
    std::shared_ptr<History> history_;
    uint64_t bytes_ = 0;
    uint64_t next_seq_ = 1;
    uint32_t last_turn_ = 0;
    std::optional<uint32_t> interrupted_;
};

// One message as the REST and WS interfaces show it: the line's keys, with
// a tool result and call arguments cut to `max_result_chars` code points
// ("result_len"/"args_len" give the full size in bytes).
Value to_value(const Message& m, size_t max_result_chars);

}  // namespace wisp::agent
