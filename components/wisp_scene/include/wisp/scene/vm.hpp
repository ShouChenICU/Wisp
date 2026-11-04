#pragma once

// One sandboxed Lua 5.5 VM per scene (scene-runtime-core spec 3.3).
//
// Sandbox: only the base, coroutine, math, string, table and utf8
// libraries are opened; dofile, loadfile, warn and string.dump are
// removed; load only accepts text; collectgarbage only accepts "collect",
// "count" and "step"; print goes to the log (tag "scene.<id>"). Every
// source file is compiled behind global_preamble(), so a name the file
// does not declare is a compile error. Lua runs some Lua code with hooks
// disabled, where the time limit cannot reach it, so the sandbox keeps
// scenes out of both places: setmetatable refuses a metatable with a
// __gc field (finalizers run unhooked, also inside lua_close), and an
// xpcall message handler is skipped once the VM has faulted (Lua calls it
// from inside the hook that raised the fault).
//
// Limits: the allocator counts every byte in PSRAM and refuses requests
// past VmLimits::memory_bytes; a count hook checks the time every
// VmLimits::hook_instructions instructions. After refusing a request,
// Lua's core runs an emergency full GC and repeats the identical request
// once; if that retry succeeds there is no fault. The same request
// refused twice in a row is a memory fault, and so is a refusal nobody
// retries (lauxlib's string buffers, used by string.rep, table.concat,
// string.format and friends, call the allocator directly and raise a
// plain error instead): the next allocation or the next instruction
// settles it. A timeout or memory fault cannot be caught: from then on
// every thread's hook fires on every instruction and raises again, and
// the coroutine functions refuse to resume anything and re-raise in the
// resumer, so no further Lua code runs until call()'s own lua_pcall
// returns. A faulted VM can only be destroyed.
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
// only after the C++ objects are gone. Outside any lua_pcall (e.g. in
// Vm's own C++ methods) a raising API call reaches the panic handler and
// aborts, so only non-raising APIs are used there.

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

struct VmLimits {
    size_t memory_bytes = 2 * 1024 * 1024;
    uint32_t exec_ms = 200;
    int hook_instructions = 1000;
};

// Returns the contents of a file in the scene package ("lib/util.lua"),
// or nullopt if there is none. The view must stay valid until the
// importing call returns.
using SourceLookup = std::function<std::optional<std::string_view>(std::string_view path)>;

// Upper bound of one print/log line built by log_lua_args(), before the
// log layer applies its own limit.
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
    size_t used_bytes() const { return used_; }

    // Where wisp.import reads lib/ files from.
    void set_sources(SourceLookup lookup);

    // Compiles `source` (text only, behind global_preamble(), chunk name
    // "=" + path). On success pushes the function and returns nullopt;
    // on failure pushes nothing.
    std::optional<ScriptError> load(std::string_view path, std::string_view source);

    // Calls the function below the top `nargs` values, like lua_pcall,
    // with a fresh time budget. On success leaves `nresults` results
    // (LUA_MULTRET allowed); on failure the function and arguments are
    // gone and nothing is pushed. Only for the scene task's top level:
    // bindings that run Lua code inside a call use lua_pcall/protect(),
    // which share the outer budget.
    std::optional<ScriptError> call(int nargs, int nresults);

    // True after a timeout or memory fault; the VM must be destroyed.
    bool faulted() const { return fault_ != Fault::None; }

private:
    enum class Fault : uint8_t { None, Timeout, Memory };

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
    Fault fault_ = Fault::None;
    bool running_ = false;
    uint64_t start_ms_ = 0;
    // A refused request Lua has not yet retried successfully, for the
    // memory fault rule above.
    bool refused_ = false;
    void* refused_ptr_ = nullptr;
    size_t refused_osize_ = 0;
    size_t refused_nsize_ = 0;
    // Where the fault was first seen by the hook (short_src, line).
    char fault_file_[LUA_IDSIZE] = {};
    int fault_line_ = 0;
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
// fault, which must reach Vm::call().
int protect(lua_State* L, void (*fn)(lua_State*, void*), void* ctx);

// Converts every value on the stack with luaL_tolstring, joins them with
// '\t', cuts the result at kLuaLogMaxBytes (on a UTF-8 character
// boundary) and logs it at `level` with tag "scene.<id>" (the id of the
// Vm that owns L). The log layer may shorten the line further. Leaves
// the stack as it was. May raise (a __tostring metamethod can).
void log_lua_args(lua_State* L, LogLevel level);

}  // namespace wisp::scene
