#pragma once

// One sandboxed Lua 5.5 VM per scene (scene-runtime-core spec 3.3).
//
// Sandbox: only the base, coroutine, math, string, table and utf8
// libraries are opened; dofile, loadfile, warn and string.dump are
// removed; load only accepts text; collectgarbage only accepts "collect",
// "count" and "step"; print goes to the log (tag "scene.<id>"). Every
// source file is compiled behind global_preamble(), so a name the file
// does not declare is a compile error.
//
// Hooks-off rule: Lua runs some Lua code with hooks disabled, where the
// time limit cannot reach it. Scenes are kept out of every such place:
//   - __gc finalizers (always unhooked, also inside lua_close):
//     setmetatable refuses a metatable with a __gc field, and binding
//     metatables must be hidden with new_binding_metatable() (or set
//     __metatable themselves) so a script cannot add __gc to one.
//   - xpcall message handlers: Lua calls them from inside the hook that
//     raised a fault, so after a fault they are skipped.
//   - A thread killed by a fault the hook raised keeps hooks off for good
//     (luaD_hook never gets to turn them back on). lua_closethread on it
//     would run its pending __close values unhooked, so nothing may close
//     such a thread: coroutine.wrap is written out and skips the close on
//     a fault, coroutine.close refuses once the VM has faulted, and
//     lua_close does not close coroutines (only the main thread, whose
//     hooks every lua_pcall restores).
// Work that runs no Lua instruction: every C function in the library
// tables (_G, string, table, math, utf8, coroutine, wisp, and the string
// metatable) checks the limits when it is entered (check_c_functions()),
// and so does every C function the libraries make or hand out at run
// time (the iterators of pairs, ipairs, string.gmatch and utf8.codes, and
// coroutine.wrap's functions), so a chain or loop of C calls stops at the
// first call after the deadline; pcall, xpcall, load and the coroutine
// functions check again when they return, and load checks every call of
// a reader function. The C loops that run metamethods per element
// (table.move, concat, unpack, insert, remove, sort, and gsub with a
// replacement table that has a metatable) also check every 256 elements,
// since one element can walk a 2000-step __index chain; such a loop can
// overrun the deadline by those 256 elements. The table loops refuse
// ranges over 2^20 elements, and insert, remove and sort a negative
// length ("range too large"); each loop runs over a bound that was
// checked, whatever a __len answers when called again. Sorting anything
// with a metatable goes through a plain copy of its elements, which
// counts toward the memory limit like any other table. Known limit: a
// single C call that is slow by itself (string.find on long strings is
// O(n*m), pattern matching can backtrack) is not interrupted, so the
// time limit can be overrun by one such call.
//
// Task coroutines (lua_task.hpp) and the hooks-off rule: a task thread
// runs only under Vm::resume(), which times it like call() and marks a
// timeout there as one off the main thread (recover_timeout() refuses it).
// A task thread killed by a fault is in the state described above, hooks
// off for good, so the task code never resumes it again and never closes
// it: it is dropped from the registry, and lua_close leaves it alone like
// any other coroutine. Every resume is refused once the VM has faulted, so
// a task that was waiting when something else faulted the VM is not run
// either. A task thread that ended with an ordinary error or was cancelled
// is dropped without lua_closethread as well (its pending __close values
// never run), which keeps one rule for every way a task ends.
//
// Scene code may run only under Vm::call() (and task threads under
// Vm::resume()): the time limit is measured from there, and code a host
// runs with its own lua_pcall outside a call is not timed at all.
//
// Limits: the allocator counts every byte in PSRAM and refuses requests
// past VmLimits::memory_bytes, or past what VmLimits::pool (the budget all
// scenes share) has left; a count hook checks the time every
// VmLimits::hook_instructions instructions. After refusing a request,
// Lua's core runs an emergency full GC and repeats the identical request
// once; if that retry succeeds there is no fault. The same request
// refused twice in a row is a memory fault, and so is a refusal nobody
// retries (lauxlib's string buffers, used by string.rep, table.concat,
// string.format and friends, call the allocator directly and raise a
// plain error instead): the next allocation, the next instruction, the
// return from a coroutine or the next check_limits() settles it. A
// timeout or memory fault cannot be caught: from then on every thread's
// hook fires on every instruction and raises again, and the coroutine
// functions refuse to resume anything and re-raise in the resumer, so no
// further Lua code runs until call()'s own lua_pcall returns. A faulted
// VM can only be destroyed.
//
// C stack: Lua is built with LUAI_MAXCCALLS=48 (third_party/lua), which
// bounds how deeply C calls nest (pcall, gsub callbacks, metamethods,
// coroutines, the parser). That bound pairs with the scene task's 128 KB
// stack in PSRAM (SceneRuntimeConfig::stack_bytes): at the limit
// string.gsub recursion, the deepest per level, needs about 1.3 KB per
// level of Xtensa frames (-Os static frame sizes), and the whole scene
// task reaches about 106 KB in that case on an x86-64 host at -Os (about
// 41 KB for pcall recursion). Changing either number means measuring that
// case on the device again.
//
// longjmp rule: Lua is compiled as C and raises errors with longjmp,
// which skips C++ destructors. A C function called from Lua (and any
// code that calls a Lua API that can raise: luaL_check*, lua_push*,
// lua_new*, luaL_error, lua_call, ...) must not have a C++ object with a
// non-trivial destructor (std::string, containers, unique_ptr, Value, ...)
// alive in its frame at the time. Validate arguments with lua_type /
// lua_to* first, then do the C++ work; to call Lua APIs while holding
// C++ objects, do it through protect(), which runs them in an inner
// lua_pcall, keep the error text in a fixed char buffer, and lua_error
// only after the C++ objects are gone. The function protect() runs is
// itself inside that inner pcall, so its own frame follows the same rule. Outside any lua_pcall
// (e.g. in Vm's own C++ methods) a raising API call reaches the panic handler and aborts, so only
// non-raising APIs are used there.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "lua.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"

namespace wisp::scene {

enum class ScriptFault { Compile, Runtime, Timeout, Memory };

struct ScriptError {
    ScriptFault fault = ScriptFault::Runtime;
    std::string file;  // "main.lua", "lib/util.lua"; empty if unknown
    int line = 0;      // 0 when the error carries no position
    std::string message;

    // "main.lua:12: message"; "main.lua: message" without a line;
    // just the message without a file.
    std::string text() const;
};

// What every scene VM together may hold (scene-device spec 3.5).
inline constexpr size_t kVmBudgetBytes = 3 * 1024 * 1024;

// How long scene.migrate(old) may run (scene-device spec 7.3).
inline constexpr uint32_t kMigrateMs = 2000;

// The memory budget several VMs share. Each Vm whose limits name the pool
// reserves every growth of its heap here as well as against its own
// limit, and gives back every byte it frees (all of it when it is
// destroyed); a growth the pool has no room for is refused like one past
// the VM's own limit. Any thread. Must outlive every Vm that uses it.
class VmPool {
public:
    explicit VmPool(size_t budget_bytes = kVmBudgetBytes) : budget_(budget_bytes) {}

    VmPool(const VmPool&) = delete;
    VmPool& operator=(const VmPool&) = delete;

    // Takes `bytes` from the budget; false (nothing taken) if it does not
    // have that many left.
    bool reserve(size_t bytes);
    void release(size_t bytes);
    size_t used() const { return used_.load(std::memory_order_relaxed); }
    size_t budget() const { return budget_; }

private:
    const size_t budget_;
    std::atomic<size_t> used_{0};
};

struct VmLimits {
    size_t memory_bytes = 2 * 1024 * 1024;
    uint32_t exec_ms = 200;
    int hook_instructions = 1000;
    // The budget this VM shares with others; null: none, only
    // memory_bytes applies.
    VmPool* pool = nullptr;
};

// Returns the contents of a file in the scene package ("lib/util.lua"),
// or nullopt if there is none. The view must stay valid until the
// importing call returns.
using SourceLookup = std::function<std::optional<std::string_view>(std::string_view path)>;

// Where Vm::call() starts the time limit from.
enum class Budget : uint8_t {
    Fresh,     // now: a new message
    Continue,  // where the last top-level call() started it: more work for
               // the same message (the reactive flush after a handler).
               // Fresh when there was no call before.
};

// Upper bound of one print/log line built by log_lua_args(); the log keeps
// it whole (log::write_text()).
constexpr size_t kLuaLogMaxBytes = 512;

class Vm {
public:
    // Opens the sandboxed state. Returns nullptr if it cannot be
    // allocated within limits.memory_bytes. `now_ms` is the clock the
    // time limit uses (tests pass a fake one).
    static std::unique_ptr<Vm> create(std::string scene_id, VmLimits limits = {},
                                      std::function<uint64_t()> now_ms = os::now_ms);
    ~Vm();

    Vm(const Vm&) = delete;
    Vm& operator=(const Vm&) = delete;

    lua_State* L() const { return L_; }
    const std::string& scene_id() const { return scene_id_; }
    // Bytes the Lua heap holds right now; never above limits.memory_bytes.
    // The pool, if any, holds exactly this much for this VM.
    size_t used_bytes() const { return used_; }
    // The most used_bytes() has been since the VM was made.
    size_t peak_bytes() const { return peak_; }

    // Where print and wisp.log go (log_lua_args()): their lines are logged
    // under `tag` instead of "scene.<id>" (at most log::kMaxTagBytes), and
    // `out`, if set, also gets each one whole, not cut at kLuaLogMaxBytes:
    // the pieces of a line one by one, then "\n". `out` runs on the thread
    // running the VM, in the middle of a Lua call: it must not call Lua.
    // Before any code runs.
    void set_output(std::string tag, std::function<void(std::string_view)> out);

    // Where wisp.import reads lib/ files from.
    void set_sources(SourceLookup lookup);

    // Compiles `source` (text only, behind global_preamble(), chunk name
    // "=" + path). On success pushes the function and returns nullopt;
    // on failure pushes nothing.
    std::optional<ScriptError> load(std::string_view path, std::string_view source);

    // Calls the function below the top `nargs` values, like lua_pcall,
    // with a fresh time budget (or, with Budget::Continue, the one the
    // last top-level call started). On success leaves `nresults` results
    // (LUA_MULTRET allowed); on failure the function and arguments are
    // gone and nothing is pushed. Only for the scene task's top level:
    // bindings that run Lua code inside a call use lua_pcall/protect(),
    // which share the outer budget. This is the only way to run scene
    // code; anything run outside it has no time limit. `exec_ms` is this
    // call's time limit (0: limits().exec_ms), for the rare step allowed
    // longer than a message (a migration, scene-device spec 7.3); a
    // timeout's error names it.
    std::optional<ScriptError> call(int nargs, int nresults, Budget budget = Budget::Fresh,
                                    uint32_t exec_ms = 0);

    // Resumes task coroutine `co` (lua_task.hpp) with the `nargs` values on
    // top of its stack, like lua_resume, as a top-level slice with a fresh
    // time budget of `budget_ms` (0: limits().exec_ms). `co` is either a new
    // thread with its function below the arguments or one suspended in a
    // yield whose values the caller has already popped. On a yield sets
    // *yielded and leaves the *nresults yielded values on co's stack; on a
    // return leaves its *nresults results there. On an error returns it
    // (Runtime, or Timeout/Memory for a fault); co is then dead and must be
    // dropped, never resumed or closed. A faulted VM refuses (returns its
    // fault without touching co), and so does a call made while Lua code
    // is running. Calls only non-raising Lua APIs.
    std::optional<ScriptError> resume(lua_State* co, int nargs, int* nresults, bool* yielded,
                                      uint32_t budget_ms);

    // True after a timeout or memory fault; the VM must be destroyed.
    bool faulted() const { return fault_ != Fault::None; }

    // Clears a timeout fault once the top-level call() that hit it has
    // returned, so the VM can run again; true if it did. Only for a step
    // whose overrun is its own business, not the scene's (the agent's
    // scene.eval, agent-core spec 4.2). It is safe there because a timeout
    // is an ordinary Lua error by the time call()'s lua_pcall returns: the
    // stack has unwound through every handler that cleans up after an
    // error, and no coroutine was resumed while it was faulted. Not when
    // the timeout hit inside a coroutine (fault_off_main_), and a memory
    // fault is never cleared (the heap may still be at its limit). The
    // next call() with
    // Budget::Continue starts a fresh budget.
    bool recover_timeout();

    // Raises the VM's fault in L if it has one, or a timeout if call()'s
    // time limit has passed; otherwise returns (and settles a pending
    // refusal, see above). The count hook only sees Lua instructions, so
    // C code calls this instead: on entry to every scene-visible C
    // function (push_checked_function() arranges that), after anything
    // that catches errors, and every so often inside a C loop that calls
    // back into Lua or walks metamethods. Follows the longjmp rule below.
    void check_limits(lua_State* L);
    // True when check_limits() would raise right now. Raises nothing, so
    // a C loop that must not raise (it holds C++ objects) can stop early
    // and leave the raising to its caller once those objects are gone.
    bool past_limits() const;

    // The Vm that owns L (any of its threads).
    static Vm* of(lua_State* L);

private:
    // Reads the set_output() state.
    friend void log_lua_args(lua_State* L, LogLevel level);

    // Memory: past limits.memory_bytes; Budget: past the pool's budget.
    enum class Fault : uint8_t { None, Timeout, Memory, Budget };

    Vm(std::string scene_id, VmLimits limits, std::function<uint64_t()> now_ms);

    static void* alloc(void* ud, void* ptr, size_t osize, size_t nsize);
    static void hook(lua_State* L, lua_Debug* ar);
    static int setup(lua_State* L);
    static int import(lua_State* L);
    static int co_resume(lua_State* L);
    static int co_close(lua_State* L);
    static int co_wrap(lua_State* L);
    static int co_wrapped(lua_State* L);
    static int co_switch(lua_State* L, lua_State* co);

    void fault(Fault kind);
    // A refusal still pending at a settling point was never recovered.
    void settle_refusal();
    // Raises the fault's message in L (never returns).
    int raise_fault(lua_State* L);
    ScriptError fault_error(std::string_view fallback_file) const;

    std::string scene_id_;
    VmLimits limits_;
    std::function<uint64_t()> now_ms_;
    SourceLookup sources_;
    lua_State* L_ = nullptr;
    // The thread running Lua code right now (the main thread or the
    // coroutine the wrappers resumed), so the allocator can make that
    // thread's hook fire on its next instruction after a memory fault.
    lua_State* current_ = nullptr;
    size_t used_ = 0;
    size_t peak_ = 0;
    // set_output(); an empty tag is "scene.<id>".
    std::string log_tag_;
    std::function<void(std::string_view)> output_;
    Fault fault_ = Fault::None;
    bool running_ = false;
    // Set by the destructor: every instruction lua_close might run raises.
    bool closing_ = false;
    uint64_t start_ms_ = 0;
    // The time limit of the top-level call running (or last run).
    uint32_t exec_ms_ = 0;
    bool started_ = false;  // start_ms_ holds a top-level call's start
    // A refused request Lua has not yet retried successfully, for the
    // memory fault rule above.
    bool refused_ = false;
    // The pool refused it (not memory_bytes).
    bool refused_by_pool_ = false;
    void* refused_ptr_ = nullptr;
    size_t refused_osize_ = 0;
    size_t refused_nsize_ = 0;
    // Where the fault was first seen by the hook (short_src, line).
    char fault_file_[LUA_IDSIZE] = {};
    int fault_line_ = 0;
    // The timeout hit a coroutine, which is now dead with its hooks off
    // for good (see the hooks-off rule): recover_timeout() refuses, since
    // the VM no longer being faulted would let coroutine.close() run that
    // thread's pending __close values unbounded.
    bool fault_off_main_ = false;
};

// The text load() puts in front of every source file, on the same line
// (so line numbers do not move): it declares the names a scene may use
// as read-only globals, which makes every other undeclared name a
// compile error.
std::string_view global_preamble();

// Compiles `source` the way Vm::load() does, without running it, in a
// throwaway state. nullopt if it compiles.
std::optional<ScriptError> compile_check(std::string_view path, std::string_view source);

// Runs fn(L, ctx) inside an inner lua_pcall and returns its status
// (LUA_OK, LUA_ERRRUN, LUA_ERRMEM, ...). On LUA_OK the values fn pushed
// are left on the stack; otherwise the error object is. Needs two free
// stack slots. A caller that gets an error must re-raise it (lua_error)
// once its C++ objects are gone: the error may be a timeout or memory
// fault, which must reach Vm::call(). fn's own frame must not hold C++
// objects with non-trivial destructors either: an error inside fn
// longjmps out of it.
int protect(lua_State* L, void (*fn)(lua_State*, void*), void* ctx);

// Replaces every C function in the table at `table` with its checked
// copy from push_checked_function(), whose upvalue rule applies: a
// function that replaces its own upvalues must not also stay reachable
// as the original closure. Vm::create() does this for the library
// tables; a binding does it for each table of functions it adds for
// scenes. Each field gets its own copy. If any function there has more
// than three upvalues, raises a Lua error naming it before changing any
// field. Can raise (it allocates), so it runs inside a protected call.
void check_c_functions(lua_State* L, int table);

// Pushes a checked copy of the C function at `idx`: a closure that calls
// Vm::check_limits() and then the function itself, on the same Lua frame
// (no extra C call level). A binding that makes a C function at run time
// and hands it to a scene (an iterator, a callback) hands out this copy,
// as string.gmatch does. The copy carries the function's own upvalues,
// at most three, by value: a table or userdata upvalue is still the same
// object, but a function that replaces one of its upvalues
// (lua_replace(L, lua_upvalueindex(i))) changes only the closure it runs
// as, so if the original is also kept somewhere (the registry, another
// table), the two drift apart. Keep such state in a table or userdata,
// or keep only the copy. A function that is already checked is pushed
// as it is. Raises a Lua error for anything but a C function, and for
// one with more than three upvalues.
void push_checked_function(lua_State* L, int idx);

// luaL_newmetatable for a binding's userdata type, with __metatable set
// so scripts can neither read nor replace it (getmetatable returns
// false). Without that a script could add a __gc to it, which Lua would
// run with hooks off. Pushes the metatable; returns false if `name`
// already existed (the existing one is pushed, unchanged).
bool new_binding_metatable(lua_State* L, const char* name);

// Converts every value on the stack with luaL_tolstring, joins them with
// '\t', cuts the result at kLuaLogMaxBytes (on a UTF-8 character
// boundary) and logs it at `level` with tag "scene.<id>" (the id of the
// Vm that owns L; the tag fits the longest scene id), or the Vm's
// set_output() tag, which also gets the whole line. It goes to the log's
// ring and sinks only, never to a file, so it is safe on the scene task.
// Leaves the stack as it was. May raise (a __tostring metamethod can).
void log_lua_args(lua_State* L, LogLevel level);

}  // namespace wisp::scene
