#include "wisp/scene/vm.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace wisp::scene {

namespace {

// Registry keys (their addresses, as light userdata).
char kImportCacheKey;
char kImportStackKey;
char kTimeoutMsgKey;
char kMemoryMsgKey;

constexpr std::string_view kPreamble =
    "global<const> ui, state, computed, bind, wisp, scene, assert, error, getmetatable, ipairs, "
    "next, pairs, pcall, print, rawequal, rawget, rawlen, rawset, select, setmetatable, "
    "tonumber, tostring, type, xpcall, collectgarbage, load, _G, _VERSION, string, table, math, "
    "utf8, coroutine; ";

// "lib/<name>" plus ".lua" stays within the package's 128-byte path limit.
constexpr size_t kMaxImportPath = 124;

// Feeds lua_load the preamble and then the source, so the source is never
// copied just to put the preamble in front of it.
struct ChunkReader {
    const char* parts[2];
    size_t sizes[2];
    int next = 0;
};

const char* read_chunk(lua_State*, void* ud, size_t* size) {
    auto* r = static_cast<ChunkReader*>(ud);
    while (r->next < 2) {
        int i = r->next++;
        if (r->sizes[i] > 0) {
            *size = r->sizes[i];
            return r->parts[i];
        }
    }
    *size = 0;
    return nullptr;
}

bool is_binary(std::string_view source) { return !source.empty() && source[0] == LUA_SIGNATURE[0]; }

// Compiles preamble + source as text. Pushes the function, or the error
// message. Never raises (lua_load is protected). The preamble hides the
// binary signature from lua_load's own mode check, so callers reject
// binary chunks with is_binary() first.
int load_text(lua_State* L, std::string_view path, std::string_view source) {
    char name[256];
    std::snprintf(name, sizeof(name), "=%.*s", static_cast<int>(path.size()), path.data());
    ChunkReader reader{{kPreamble.data(), source.data()}, {kPreamble.size(), source.size()}};
    return lua_load(L, read_chunk, &reader, name, "t");
}

// "2 MB", "512 KB" or "1000 bytes".
void format_bytes(char* out, size_t cap, size_t bytes) {
    if (bytes % (1024 * 1024) == 0)
        std::snprintf(out, cap, "%zu MB", bytes / (1024 * 1024));
    else if (bytes % 1024 == 0)
        std::snprintf(out, cap, "%zu KB", bytes / 1024);
    else
        std::snprintf(out, cap, "%zu bytes", bytes);
}

void format_fault(char* out, size_t cap, bool timeout, const VmLimits& limits) {
    if (timeout) {
        std::snprintf(out, cap, "execution exceeded %u ms", static_cast<unsigned>(limits.exec_ms));
    } else {
        char size[32];
        format_bytes(size, sizeof(size), limits.memory_bytes);
        std::snprintf(out, cap, "memory limit %s exceeded", size);
    }
}

// Splits "file:line: message" (the shape luaL_error and the parser give
// every positioned message).
bool split_position(std::string_view s, ScriptError& e) {
    for (size_t colon = s.find(':'); colon != std::string_view::npos;
         colon = s.find(':', colon + 1)) {
        if (colon == 0 || s.substr(0, colon).find('\n') != std::string_view::npos) return false;
        size_t i = colon + 1;
        int line = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && line < 10000000)
            line = line * 10 + (s[i++] - '0');
        if (i == colon + 1 || s.substr(i, 2) != ": ") continue;
        e.file.assign(s.substr(0, colon));
        e.line = line;
        e.message.assign(s.substr(i + 2));
        return true;
    }
    return false;
}

// Builds a ScriptError from the error object on top of L's stack without
// calling anything that can raise (no __tostring).
ScriptError error_from_top(lua_State* L, ScriptFault fault, std::string_view fallback_file) {
    ScriptError e;
    e.fault = fault;
    if (lua_type(L, -1) == LUA_TSTRING) {
        size_t n = 0;
        const char* s = lua_tolstring(L, -1, &n);
        if (!split_position(std::string_view(s, n), e)) e.message.assign(s, n);
    } else {
        e.message = "(error object is a ";
        e.message += lua_typename(L, lua_type(L, -1));
        e.message += " value)";
    }
    if (e.file.empty() && e.line == 0) e.file.assign(fallback_file);
    return e;
}

bool valid_lib_path(const char* p, size_t n) {
    if (n <= 4 || n > kMaxImportPath || std::memcmp(p, "lib/", 4) != 0) return false;
    bool segment_empty = true;
    for (size_t i = 4; i < n; ++i) {
        char c = p[i];
        if (c == '/') {
            if (segment_empty) return false;
            segment_empty = true;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
            segment_empty = false;
        } else {
            return false;
        }
    }
    return !segment_empty;
}

Vm* owner(lua_State* L) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    return static_cast<Vm*>(ud);
}

int return_all(lua_State* L, int /*status*/, lua_KContext /*ctx*/) { return lua_gettop(L); }

int return_upvalue(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

// Calls the original function in upvalue 1 with every argument.
int call_original(lua_State* L) {
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    return lua_gettop(L);
}

// load(chunk [, chunkname [, mode [, env]]]) with mode forced to "t".
// Only grows the stack to three values, so an absent env stays absent
// (an explicit nil env would leave the chunk without globals).
int sandbox_load(lua_State* L) {
    if (lua_gettop(L) < 3) lua_settop(L, 3);
    lua_pushliteral(L, "t");
    lua_replace(L, 3);
    return call_original(L);
}

int sandbox_collectgarbage(lua_State* L) {
    const char* opt = luaL_optstring(L, 1, "collect");
    if (std::strcmp(opt, "collect") != 0 && std::strcmp(opt, "count") != 0 &&
        std::strcmp(opt, "step") != 0)
        return luaL_error(L, "collectgarbage option '%s' is not allowed", opt);
    return call_original(L);
}

// Lua runs __gc finalizers with hooks disabled, so a looping finalizer
// would escape the time limit, and lua_close would hang in it. Only a
// metatable that has __gc when it is set marks the object for
// finalization, so checking here is enough.
int sandbox_setmetatable(lua_State* L) {
    if (lua_type(L, 2) == LUA_TTABLE) {
        lua_pushliteral(L, "__gc");
        if (lua_rawget(L, 2) != LUA_TNIL) return luaL_error(L, "__gc metamethods are not allowed");
        lua_pop(L, 1);
    }
    return call_original(L);
}

// Lua calls a message handler from luaG_errormsg, before unwinding; for
// an error the time hook raised that is still inside the hook, with hooks
// disabled, so the handler would run unbounded. Once the VM has faulted
// the error object passes through untouched instead.
int guarded_handler(lua_State* L) {
    if (owner(L)->faulted()) return 1;
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

// xpcall(f, msgh, ...) with msgh wrapped in guarded_handler. Called with
// lua_callk so f can still yield across it, as with the original.
int sandbox_xpcall(lua_State* L) {
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, guarded_handler, 1);
    lua_replace(L, 2);
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_callk(L, n, LUA_MULTRET, 0, return_all);
    return lua_gettop(L);
}

// The table library's loops run in C: no instruction, so no hook, and
// reading or writing absent keys allocates nothing, so no memory limit
// either. Every count that drives one of them (an argument, or a length
// that may come from __len) is capped. A length is read exactly once, so
// a __len that answers differently on a second call cannot get past the
// cap.
constexpr lua_Integer kMaxTableRange = lua_Integer{1} << 20;

// True if [first, last] holds more than kMaxTableRange elements.
bool range_too_large(lua_Integer first, lua_Integer last) {
    return last >= first && static_cast<lua_Unsigned>(last) - static_cast<lua_Unsigned>(first) >=
                                static_cast<lua_Unsigned>(kMaxTableRange);
}

int range_error(lua_State* L) { return luaL_error(L, "range too large"); }

// The length of argument 1 for insert/remove/sort: a table, or a value
// whose metatable provides it (as ltablib's checktab accepts).
lua_Integer checked_len(lua_State* L) {
    if (lua_type(L, 1) != LUA_TTABLE) {
        if (!lua_getmetatable(L, 1))
            luaL_checktype(L, 1, LUA_TTABLE);
        else
            lua_pop(L, 1);
    }
    lua_Integer n = luaL_len(L, 1);
    if (n > kMaxTableRange) range_error(L);
    return n;
}

int sandbox_table_move(lua_State* L) {
    lua_Integer f = luaL_checkinteger(L, 2);
    lua_Integer e = luaL_checkinteger(L, 3);
    if (range_too_large(f, e)) return range_error(L);
    return call_original(L);
}

// concat reads the length itself even when j is given, but then uses j;
// passing the checked bound as j keeps its loop to what was checked.
int sandbox_table_concat(lua_State* L) {
    lua_Integer first = luaL_optinteger(L, 3, 1);
    lua_Integer last = lua_isnoneornil(L, 4) ? luaL_len(L, 1) : luaL_checkinteger(L, 4);
    if (range_too_large(first, last)) return range_error(L);
    lua_settop(L, 4);
    lua_pushinteger(L, last);
    lua_replace(L, 4);
    return call_original(L);
}

// Behaves like Lua 5.5.1's table.insert (ltablib.c, MIT), reading the
// length once.
int sandbox_table_insert(lua_State* L) {
    lua_Integer e = checked_len(L) + 1;  // first empty slot
    lua_Integer pos = e;
    switch (lua_gettop(L)) {
        case 2:
            break;
        case 3:
            pos = luaL_checkinteger(L, 2);
            luaL_argcheck(L, static_cast<lua_Unsigned>(pos) - 1u < static_cast<lua_Unsigned>(e), 2,
                          "position out of bounds");
            for (lua_Integer i = e; i > pos; --i) {
                lua_geti(L, 1, i - 1);
                lua_seti(L, 1, i);
            }
            break;
        default:
            return luaL_error(L, "wrong number of arguments to 'insert'");
    }
    lua_seti(L, 1, pos);
    return 0;
}

// Behaves like Lua 5.5.1's table.remove (ltablib.c, MIT), reading the
// length once.
int sandbox_table_remove(lua_State* L) {
    lua_Integer size = checked_len(L);
    lua_Integer pos = luaL_optinteger(L, 2, size);
    if (pos != size)
        luaL_argcheck(L, static_cast<lua_Unsigned>(pos) - 1u <= static_cast<lua_Unsigned>(size), 2,
                      "position out of bounds");
    lua_geti(L, 1, pos);
    for (; pos < size; ++pos) {
        lua_geti(L, 1, pos + 1);
        lua_seti(L, 1, pos);
    }
    lua_pushnil(L);
    lua_seti(L, 1, pos);
    return 1;
}

// A plain table's length involves no Lua code, so the original sort
// reads the same length that was checked. Anything with a metatable is
// sorted through a copy whose length is fixed: elements are read once
// (through __index), the copy is sorted, and the result is written back
// (through __newindex). Holes stay holes, so comparing one still fails.
int sandbox_table_sort(lua_State* L) {
    if (lua_type(L, 1) == LUA_TTABLE && !lua_getmetatable(L, 1)) {
        if (static_cast<lua_Integer>(lua_rawlen(L, 1)) > kMaxTableRange) return range_error(L);
        return call_original(L);
    }
    if (lua_type(L, 1) == LUA_TTABLE) lua_pop(L, 1);  // the metatable
    lua_Integer n = checked_len(L);
    if (n <= 1) return 0;
    if (!lua_isnoneornil(L, 2)) luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_settop(L, 2);
    lua_createtable(L, static_cast<int>(n), 0);  // 3: the copy
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_geti(L, 1, i);
        lua_rawseti(L, 3, i);
    }
    lua_createtable(L, 0, 1);  // its metatable: __len = n
    lua_pushinteger(L, n);
    lua_pushcclosure(L, return_upvalue, 1);
    lua_setfield(L, -2, "__len");
    lua_setmetatable(L, 3);
    lua_pushvalue(L, lua_upvalueindex(1));  // the original sort
    lua_pushvalue(L, 3);
    lua_pushvalue(L, 2);
    lua_call(L, 2, 0);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, 3, i);
        lua_seti(L, 1, i);
    }
    return 0;
}

int sandbox_print(lua_State* L) {
    log_lua_args(L, LogLevel::Info);
    return 0;
}

int panic(lua_State* L) {
    const char* msg = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : "?";
    WLOG_E("scene", "unprotected Lua error: %s", msg);
    return 0;  // Lua aborts after this returns
}

struct ProtectCall {
    void (*fn)(lua_State*, void*);
    void* ctx;
};

int protect_trampoline(lua_State* L) {
    auto* call = static_cast<ProtectCall*>(lua_touserdata(L, 1));
    lua_settop(L, 0);
    call->fn(L, call->ctx);
    return lua_gettop(L);
}

// compile_check's throwaway state: counted against the same default limit
// as a scene VM, nothing else.
struct CheckHeap {
    size_t used = 0;
    size_t limit = VmLimits{}.memory_bytes;
};

void* check_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* heap = static_cast<CheckHeap*>(ud);
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        os::dealloc(ptr);
        heap->used -= old;
        return nullptr;
    }
    if (nsize > old && nsize - old > heap->limit - heap->used) return nullptr;
    void* p = os::realloc(ptr, nsize, os::Mem::Large);
    if (!p && nsize > old) return nullptr;
    // A failed shrink keeps the old block, which Lua now sizes as nsize.
    heap->used = heap->used - old + nsize;
    return p ? p : ptr;
}

}  // namespace

std::string ScriptError::text() const {
    std::string out;
    if (!file.empty()) {
        out = file;
        if (line > 0) {
            out += ':';
            out += std::to_string(line);
        }
        out += ": ";
    }
    out += message;
    return out;
}

std::string_view global_preamble() { return kPreamble; }

Vm::Vm(std::string scene_id, VmLimits limits, std::function<uint64_t()> now_ms)
    : scene_id_(std::move(scene_id)), limits_(limits), now_ms_(std::move(now_ms)) {
    if (limits_.hook_instructions < 1) limits_.hook_instructions = 1;
}

Vm::~Vm() {
    if (L_) {
        // lua_close runs the main thread's pending __close values (none
        // once call() has returned, but the hook makes sure) and the
        // pending __gc finalizers. The hook stops every Lua instruction;
        // finalizers can only come from binding metatables, which are
        // host C code (setmetatable refuses __gc, and new_binding_metatable
        // hides binding metatables from scripts).
        running_ = false;
        closing_ = true;
        lua_sethook(L_, &Vm::hook, LUA_MASKCOUNT, 1);
        lua_close(L_);
    }
}

std::unique_ptr<Vm> Vm::create(std::string scene_id, VmLimits limits,
                               std::function<uint64_t()> now_ms) {
    std::unique_ptr<Vm> vm(new Vm(std::move(scene_id), limits, std::move(now_ms)));
    lua_State* L = lua_newstate(&Vm::alloc, vm.get(), static_cast<unsigned>(os::now_us()));
    if (!L) return nullptr;
    vm->L_ = L;
    lua_atpanic(L, panic);
    lua_pushcfunction(L, &Vm::setup);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK || vm->faulted()) return nullptr;
    // Threads copy their creator's hook (lstate.c, lua_newthread), so every
    // coroutine a scene creates starts with this one.
    lua_sethook(L, &Vm::hook, LUA_MASKCOUNT, vm->limits_.hook_instructions);
    vm->refused_ = false;
    return vm;
}

int Vm::setup(lua_State* L) {
    Vm* vm = owner(L);
    const struct {
        const char* name;
        lua_CFunction open;
    } libs[] = {
        {LUA_GNAME, luaopen_base},       {LUA_COLIBNAME, luaopen_coroutine},
        {LUA_MATHLIBNAME, luaopen_math}, {LUA_STRLIBNAME, luaopen_string},
        {LUA_TABLIBNAME, luaopen_table}, {LUA_UTF8LIBNAME, luaopen_utf8},
    };
    for (const auto& lib : libs) {
        luaL_requiref(L, lib.name, lib.open, 1);
        lua_pop(L, 1);
    }

    for (const char* name : {"dofile", "loadfile", "warn"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);

    const struct {
        const char* name;
        lua_CFunction fn;
    } wrapped[] = {
        {"load", sandbox_load},
        {"collectgarbage", sandbox_collectgarbage},
        {"setmetatable", sandbox_setmetatable},
        {"xpcall", sandbox_xpcall},
    };
    for (const auto& w : wrapped) {
        lua_getglobal(L, w.name);
        lua_pushcclosure(L, w.fn, 1);
        lua_setglobal(L, w.name);
    }
    lua_pushcfunction(L, sandbox_print);
    lua_setglobal(L, "print");

    lua_getglobal(L, LUA_COLIBNAME);
    const struct {
        const char* name;
        lua_CFunction fn;
    } co_wrapped[] = {
        {"resume", &Vm::co_resume},
        {"close", &Vm::co_close},
    };
    for (const auto& w : co_wrapped) {
        lua_getfield(L, -1, w.name);
        lua_pushcclosure(L, w.fn, 1);
        lua_setfield(L, -2, w.name);
    }
    lua_pushcfunction(L, &Vm::co_wrap);
    lua_setfield(L, -2, "wrap");
    lua_pop(L, 1);

    lua_getglobal(L, LUA_TABLIBNAME);
    const struct {
        const char* name;
        lua_CFunction fn;
        bool keeps_original;
    } table_fns[] = {
        {"move", sandbox_table_move, true},      {"concat", sandbox_table_concat, true},
        {"sort", sandbox_table_sort, true},      {"insert", sandbox_table_insert, false},
        {"remove", sandbox_table_remove, false},
    };
    for (const auto& t : table_fns) {
        if (t.keeps_original) {
            lua_getfield(L, -1, t.name);
            lua_pushcclosure(L, t.fn, 1);
        } else {
            lua_pushcfunction(L, t.fn);
        }
        lua_setfield(L, -2, t.name);
    }
    lua_pop(L, 1);

    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, &Vm::import);
    lua_setfield(L, -2, "import");
    lua_setglobal(L, "wisp");

    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kImportCacheKey);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kImportStackKey);

    // The fault messages are made now, so raising one later never needs
    // to allocate (the heap may be full by then).
    char msg[64];
    format_fault(msg, sizeof(msg), true, vm->limits_);
    lua_pushstring(L, msg);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kTimeoutMsgKey);
    format_fault(msg, sizeof(msg), false, vm->limits_);
    lua_pushstring(L, msg);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kMemoryMsgKey);
    return 0;
}

void* Vm::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    Vm* vm = static_cast<Vm*>(ud);
    // With ptr == nullptr, osize is the kind of object, not a size.
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        os::dealloc(ptr);
        vm->used_ -= old;
        return nullptr;
    }
    if (nsize <= old) {
        // Lua assumes shrinking never fails; the old block is big enough.
        void* p = os::realloc(ptr, nsize, os::Mem::Large);
        vm->used_ -= old - nsize;
        return p ? p : ptr;
    }
    void* p = nullptr;
    if (nsize - old <= vm->limits_.memory_bytes - vm->used_)
        p = os::realloc(ptr, nsize, os::Mem::Large);
    bool same = vm->refused_ && vm->refused_ptr_ == ptr && vm->refused_osize_ == osize &&
                vm->refused_nsize_ == nsize;
    if (!p) {
        if (vm->refused_) {
            // Either the emergency GC's retry was refused too, or an
            // earlier refusal was never retried: the memory is gone.
            vm->fault(Fault::Memory);
        } else {
            vm->refused_ = true;
            vm->refused_ptr_ = ptr;
            vm->refused_osize_ = osize;
            vm->refused_nsize_ = nsize;
            // Lua's core retries synchronously, inside this same
            // allocation; anything else that refusal leads to (a caught
            // error) reaches the hook on the very next instruction, which
            // settles it. Frees and shrinks in between (the emergency
            // GC's own work) do not touch the pending refusal.
            if (vm->current_) lua_sethook(vm->current_, &Vm::hook, LUA_MASKCOUNT, 1);
        }
        return nullptr;
    }
    vm->used_ += nsize - old;
    if (vm->refused_) {
        vm->refused_ = false;
        if (!same) vm->fault(Fault::Memory);
    }
    return p;
}

void Vm::settle_refusal() {
    if (refused_) {
        refused_ = false;
        fault(Fault::Memory);
    }
}

void Vm::fault(Fault kind) {
    if (fault_ != Fault::None) return;
    fault_ = kind;
    // From here on every instruction of the main thread raises. The
    // thread that faulted gets the same from raise_fault() (a timeout) or
    // already got it from the first refusal (memory, see alloc()). Every
    // other thread is covered by the coroutine wrappers: none can be
    // resumed, and each resumer raises as soon as control comes back.
    if (L_) lua_sethook(L_, &Vm::hook, LUA_MASKCOUNT, 1);
}

int Vm::raise_fault(lua_State* L) {
    lua_sethook(L, &Vm::hook, LUA_MASKCOUNT, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, fault_ == Fault::Memory ? &kMemoryMsgKey : &kTimeoutMsgKey);
    return lua_error(L);
}

void Vm::hook(lua_State* L, lua_Debug* ar) {
    Vm* vm = owner(L);
    if (vm->closing_) {
        vm->raise_fault(L);
        return;
    }
    vm->settle_refusal();
    if (vm->fault_ == Fault::None) {
        // A refusal shortened this thread's count; put it back.
        if (lua_gethookcount(L) != vm->limits_.hook_instructions)
            lua_sethook(L, &Vm::hook, LUA_MASKCOUNT, vm->limits_.hook_instructions);
        // Lua code only runs outside call() inside some other protected
        // call (a host's own lua_pcall); it is not timed.
        if (!vm->running_) return;
        if (vm->now_ms_() - vm->start_ms_ <= vm->limits_.exec_ms) return;
        vm->fault(Fault::Timeout);
        if (lua_getinfo(L, "Sl", ar) && ar->currentline > 0) {
            std::snprintf(vm->fault_file_, sizeof(vm->fault_file_), "%s", ar->short_src);
            vm->fault_line_ = ar->currentline;
        }
    }
    vm->raise_fault(L);
}

// Runs the original coroutine function in upvalue 1 with every argument,
// tracking `co` as the running thread. A fault refuses to start anything
// and is raised again in the caller once control returns, so a coroutine
// cannot catch its child's fault and carry on. A refusal still pending
// when the child gives control back was not recovered by a retry, so it
// is settled (as a fault) before the caller runs another instruction.
int Vm::co_switch(lua_State* L, lua_State* co) {
    Vm* vm = owner(L);
    if (vm->faulted()) return vm->raise_fault(L);
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_State* saved = vm->current_;
    if (co) vm->current_ = co;
    int status = lua_pcall(L, n, LUA_MULTRET, 0);
    vm->current_ = saved;
    vm->settle_refusal();
    if (vm->faulted()) return vm->raise_fault(L);
    if (status != LUA_OK) return lua_error(L);
    return lua_gettop(L);
}

int Vm::co_resume(lua_State* L) { return co_switch(L, lua_tothread(L, 1)); }

int Vm::co_close(lua_State* L) { return co_switch(L, lua_tothread(L, 1)); }

// coroutine.wrap, written out instead of wrapping lcorolib's: on an error
// the original closes the dead coroutine (lua_closethread), which runs its
// pending __close values. After a fault raised by the hook that thread's
// hooks are off for good (luaD_hook never got to turn them back on), so
// those values would run unbounded. Here a fault skips the close.
int Vm::co_wrap(lua_State* L) {
    Vm* vm = owner(L);
    if (vm->faulted()) return vm->raise_fault(L);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_State* co = lua_newthread(L);  // copies L's hook
    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);
    lua_pushcclosure(L, &Vm::co_wrapped, 1);
    return 1;
}

int Vm::co_wrapped(lua_State* L) {
    Vm* vm = owner(L);
    if (vm->faulted()) return vm->raise_fault(L);
    lua_State* co = lua_tothread(L, lua_upvalueindex(1));
    int narg = lua_gettop(L);
    if (!lua_checkstack(co, narg)) return luaL_error(L, "too many arguments to resume");
    lua_xmove(L, co, narg);
    lua_State* saved = vm->current_;
    vm->current_ = co;
    int nres = 0;
    int status = lua_resume(co, L, narg, &nres);
    vm->current_ = saved;
    vm->settle_refusal();
    if (vm->faulted()) return vm->raise_fault(L);
    if (status == LUA_OK || status == LUA_YIELD) {
        if (!lua_checkstack(L, nres + 1)) {
            lua_pop(co, nres);
            return luaL_error(L, "too many results to resume");
        }
        lua_xmove(co, L, nres);
        return nres;
    }
    lua_xmove(co, L, 1);  // the error
    int co_status = lua_status(co);
    if (co_status != LUA_OK && co_status != LUA_YIELD) {
        // Died of an ordinary error: hooks are on, so its __close values
        // run bounded like any other code.
        vm->current_ = co;
        status = lua_closethread(co, L);
        vm->current_ = saved;
        vm->settle_refusal();
        if (vm->faulted()) return vm->raise_fault(L);
        lua_xmove(co, L, 1);
    }
    if (status != LUA_ERRMEM && lua_type(L, -1) == LUA_TSTRING) {
        luaL_where(L, 1);
        lua_insert(L, -2);
        lua_concat(L, 2);
    }
    return lua_error(L);
}

int Vm::import(lua_State* L) {
    Vm* vm = owner(L);
    size_t len = 0;
    const char* path = lua_type(L, 1) == LUA_TSTRING ? lua_tolstring(L, 1, &len) : nullptr;
    if (!path) return luaL_typeerror(L, 1, "string");
    if (!valid_lib_path(path, len))
        return luaL_error(L, "import: invalid path '%s' (expected lib/<name>, without .lua)", path);
    lua_settop(L, 1);
    lua_rawgetp(L, LUA_REGISTRYINDEX, &kImportCacheKey);  // 2
    lua_pushvalue(L, 1);
    if (lua_rawget(L, 2) != LUA_TNIL) return 1;
    lua_pop(L, 1);

    lua_rawgetp(L, LUA_REGISTRYINDEX, &kImportStackKey);  // 3
    lua_Integer depth = static_cast<lua_Integer>(lua_rawlen(L, 3));
    for (lua_Integer i = 1; i <= depth; ++i) {
        lua_rawgeti(L, 3, i);
        bool same = lua_rawequal(L, -1, 1);
        lua_pop(L, 1);
        if (!same) continue;
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        luaL_addstring(&b, "import cycle: ");
        for (lua_Integer j = i; j <= depth; ++j) {
            lua_rawgeti(L, 3, j);
            luaL_addvalue(&b);
            luaL_addstring(&b, " -> ");
        }
        lua_pushvalue(L, 1);
        luaL_addvalue(&b);
        luaL_pushresult(&b);
        return luaL_error(L, "%s", lua_tostring(L, -1));
    }

    char file[kMaxImportPath + 8];
    std::snprintf(file, sizeof(file), "%s.lua", path);
    // optional<string_view> is trivially destructible, so it may stay in
    // this frame across the raising calls below.
    std::optional<std::string_view> source;
    if (vm->sources_) source = vm->sources_(std::string_view(file));
    if (!source) return luaL_error(L, "import: %s not found", file);
    if (is_binary(*source))
        return luaL_error(L, "import: %s: attempt to load a binary chunk", file);
    if (load_text(L, file, *source) != LUA_OK) return lua_error(L);  // 4

    lua_pushvalue(L, 1);
    lua_rawseti(L, 3, depth + 1);
    int status = lua_pcall(L, 0, 1, 0);  // result or error at 4
    lua_pushnil(L);
    lua_rawseti(L, 3, depth + 1);
    if (status != LUA_OK) return lua_error(L);
    if (lua_isnil(L, 4)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, 1);
    lua_pushvalue(L, 4);
    lua_rawset(L, 2);
    return 1;
}

void Vm::set_sources(SourceLookup lookup) { sources_ = std::move(lookup); }

ScriptError Vm::fault_error(std::string_view fallback_file) const {
    ScriptError e;
    e.fault = fault_ == Fault::Timeout ? ScriptFault::Timeout : ScriptFault::Memory;
    char msg[64];
    format_fault(msg, sizeof(msg), fault_ == Fault::Timeout, limits_);
    e.message = msg;
    if (fault_line_ > 0) {
        e.file = fault_file_;
        e.line = fault_line_;
    } else {
        e.file.assign(fallback_file);
    }
    return e;
}

std::optional<ScriptError> Vm::load(std::string_view path, std::string_view source) {
    if (faulted()) return fault_error(path);
    if (is_binary(source))
        return ScriptError{ScriptFault::Compile, std::string(path), 0,
                           "attempt to load a binary chunk"};
    int status = load_text(L_, path, source);
    settle_refusal();
    if (faulted()) {
        lua_pop(L_, 1);  // the function or the error
        return fault_error(path);
    }
    if (status == LUA_OK) return std::nullopt;
    ScriptError e = error_from_top(L_, ScriptFault::Compile, path);
    lua_pop(L_, 1);
    return e;
}

std::optional<ScriptError> Vm::call(int nargs, int nresults) {
    int base = lua_gettop(L_) - nargs - 1;
    // The callee's file, for errors that carry no position.
    char callee[LUA_IDSIZE] = "";
    if (lua_type(L_, base + 1) == LUA_TFUNCTION && lua_checkstack(L_, 1)) {
        lua_Debug ar;
        lua_pushvalue(L_, base + 1);
        if (lua_getinfo(L_, ">S", &ar) && std::strcmp(ar.what, "C") != 0)
            std::snprintf(callee, sizeof(callee), "%s", ar.short_src);
    }
    if (faulted()) {
        lua_settop(L_, base);
        return fault_error(callee);
    }
    bool outer = !running_;
    if (outer) {
        start_ms_ = now_ms_();
        running_ = true;
        current_ = L_;
        lua_sethook(L_, &Vm::hook, LUA_MASKCOUNT, limits_.hook_instructions);
    }
    int status = lua_pcall(L_, nargs, nresults, 0);
    if (outer) {
        running_ = false;
        current_ = nullptr;
        settle_refusal();
    }
    if (faulted()) {
        lua_settop(L_, base);
        return fault_error(callee);
    }
    if (status != LUA_OK) {
        ScriptError e = error_from_top(L_, ScriptFault::Runtime, callee);
        lua_settop(L_, base);
        return e;
    }
    return std::nullopt;
}

std::optional<ScriptError> compile_check(std::string_view path, std::string_view source) {
    if (is_binary(source))
        return ScriptError{ScriptFault::Compile, std::string(path), 0,
                           "attempt to load a binary chunk"};
    CheckHeap heap;
    lua_State* L = lua_newstate(check_alloc, &heap, 0);
    if (!L) return ScriptError{ScriptFault::Memory, std::string(path), 0, "not enough memory"};
    lua_atpanic(L, panic);
    std::optional<ScriptError> out;
    int status = load_text(L, path, source);
    if (status != LUA_OK)
        out = error_from_top(L, status == LUA_ERRMEM ? ScriptFault::Memory : ScriptFault::Compile,
                             path);
    lua_close(L);
    return out;
}

int protect(lua_State* L, void (*fn)(lua_State*, void*), void* ctx) {
    ProtectCall call{fn, ctx};
    lua_pushcfunction(L, protect_trampoline);
    lua_pushlightuserdata(L, &call);
    return lua_pcall(L, 1, LUA_MULTRET, 0);
}

bool new_binding_metatable(lua_State* L, const char* name) {
    if (!luaL_newmetatable(L, name)) return false;
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "__metatable");
    return true;
}

void log_lua_args(lua_State* L, LogLevel level) {
    char line[kLuaLogMaxBytes + 1];
    size_t len = 0;
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) {
            if (len == kLuaLogMaxBytes) break;
            line[len++] = '\t';
        }
        size_t size = 0;
        const char* s = luaL_tolstring(L, i, &size);
        size_t room = kLuaLogMaxBytes - len;
        bool cut = size > room;
        if (cut) {
            // Back off to the start of the character the cut lands in.
            size = room;
            while (size > 0 && (static_cast<unsigned char>(s[size]) & 0xC0) == 0x80) --size;
        }
        std::memcpy(line + len, s, size);
        len += size;
        lua_pop(L, 1);
        if (cut) break;
    }
    line[len] = '\0';
    char tag[48];
    std::snprintf(tag, sizeof(tag), "scene.%s", owner(L)->scene_id().c_str());
    log::write(level, tag, "%s", line);
}

}  // namespace wisp::scene
