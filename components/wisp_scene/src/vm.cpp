#include "wisp/scene/vm.hpp"

#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#include "wisp/core/scene_manifest.hpp"

namespace wisp::scene {

namespace {

// Registry keys (their addresses, as light userdata).
char kImportCacheKey;
char kImportStackKey;
char kTimeoutMsgKey;
char kMemoryMsgKey;
char kBudgetMsgKey;

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

enum class FaultText : uint8_t { Timeout, Memory, Budget };

void format_fault(char* out, size_t cap, FaultText kind, const VmLimits& limits) {
    char size[32];
    switch (kind) {
        case FaultText::Timeout:
            std::snprintf(out, cap, "execution exceeded %u ms",
                          static_cast<unsigned>(limits.exec_ms));
            return;
        case FaultText::Memory:
            format_bytes(size, sizeof(size), limits.memory_bytes);
            std::snprintf(out, cap, "memory limit %s exceeded", size);
            return;
        case FaultText::Budget:
            format_bytes(size, sizeof(size), limits.pool != nullptr ? limits.pool->budget() : 0);
            std::snprintf(out, cap, "scene memory budget %s exceeded", size);
            return;
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

// See Vm::check_limits.
void enter(lua_State* L) { Vm::of(L)->check_limits(L); }

// How often the C loops below check the limits, which bounds how far one
// of them can run past the deadline: 256 elements, each at most one read
// and one write that can each walk MAXTAGLOOP (2000) __index/__newindex
// steps, so up to about 256 * 2 * 2000 = 1M table lookups between two
// checks. That took 6-8 ms on the host at -O2 (13 ms unoptimized); the
// device is slower. Checking every element would read the clock up to
// 2^20 times per call.
constexpr lua_Integer kLoopCheckEvery = 256;

// Called once per element by the C loops below.
void tick(lua_State* L, lua_Integer& steps) {
    if (++steps % kLoopCheckEvery == 0) enter(L);
}

// Every C function a scene can reach is this closure, except
// coroutine.wrap's functions, which check by themselves: it checks the
// limits and then calls the real function, on the same Lua frame (no
// lua_call: no extra C call level, a few bytes of C stack). The real
// function's own N upvalues come first, so it finds them where it
// expects; it sits in upvalue N + 1. A wrapper written here may keep
// more upvalues after it (its stock original, the iterator it hands out).
template <int N>
int checked(lua_State* L) {
    enter(L);
    return lua_tocfunction(L, lua_upvalueindex(N + 1))(L);
}

// The most upvalues of its own a function can bring to checked<>: a
// gmatch iterator has three (its subject, pattern and match state).
constexpr int kMaxCheckedUpvalues = 3;
constexpr lua_CFunction kChecked[kMaxCheckedUpvalues + 1] = {checked<0>, checked<1>, checked<2>,
                                                             checked<3>};

bool is_checked(lua_CFunction f) {
    for (lua_CFunction c : kChecked)
        if (f == c) return true;
    return false;
}

// True if the C function at `idx` has no more upvalues than checked<>
// can carry.
bool checkable(lua_State* L, int idx) {
    if (!lua_getupvalue(L, idx, kMaxCheckedUpvalues + 1)) return true;
    lua_pop(L, 1);
    return false;
}

// Pushes `fn` behind the entry check, with the `extra` values from the
// top of the stack (popped) after it as upvalues 2 and on.
void push_checked(lua_State* L, lua_CFunction fn, int extra = 0) {
    lua_pushcfunction(L, fn);
    lua_insert(L, -extra - 1);
    lua_pushcclosure(L, checked<0>, extra + 1);
}

// Runs the stock function a wrapper replaced (upvalue 2, a function
// without upvalues of its own), on the wrapper's frame.
int call_original(lua_State* L) { return lua_tocfunction(L, lua_upvalueindex(2))(L); }

int return_upvalue(lua_State* L) {
    lua_pushvalue(L, lua_upvalueindex(1));
    return 1;
}

// A load() reader, checked on every call: lua_load keeps calling it with
// no instruction in between.
int checked_reader(lua_State* L) {
    enter(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_call(L, 0, 1);
    return 1;
}

// load(chunk [, chunkname [, mode [, env]]]) with mode forced to "t".
// Only grows the stack to three values, so an absent env stays absent
// (an explicit nil env would leave the chunk without globals). lua_load
// parses in protected mode, so it catches whatever a reader function
// raises; a fault is raised again on the way out.
int sandbox_load(lua_State* L) {
    if (lua_type(L, 1) == LUA_TFUNCTION) {
        lua_pushvalue(L, 1);
        lua_pushcclosure(L, checked_reader, 1);
        lua_replace(L, 1);
    }
    if (lua_gettop(L) < 3) lua_settop(L, 3);
    lua_pushliteral(L, "t");
    lua_replace(L, 3);
    int n = call_original(L);
    enter(L);
    return n;
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
    if (Vm::of(L)->faulted()) return 1;
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

// pcall and xpcall behave like Lua 5.5.1's (lbaselib.c, MIT), and check
// the limits again when the call returns or fails (the entry check is
// the wrapper's), so a fault never comes back from them as a result,
// even to a C caller (a metamethod) that runs no Lua instruction
// afterwards.
int finish_pcall(lua_State* L, int status, lua_KContext extra) {
    enter(L);
    if (status != LUA_OK && status != LUA_YIELD) {
        lua_pushboolean(L, 0);
        lua_pushvalue(L, -2);
        return 2;
    }
    return lua_gettop(L) - static_cast<int>(extra);
}

int sandbox_pcall(lua_State* L) {
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    int status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finish_pcall);
    return finish_pcall(L, status, 0);
}

// The message handler is wrapped in guarded_handler.
int sandbox_xpcall(lua_State* L) {
    int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, guarded_handler, 1);
    lua_replace(L, 2);
    lua_pushboolean(L, 1);
    lua_pushvalue(L, 1);
    lua_rotate(L, 3, 2);  // f, msgh, true, f, args...
    int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finish_pcall);
    return finish_pcall(L, status, 2);
}

// A gsub replacement table is read once per match, inside gsub's C loop;
// with a metatable each read can walk a long __index chain. Such a table
// is passed to gsub as this function instead, which reads it the same
// way (the first capture, or the whole match, as the key) and checks the
// limits on every match.
int table_repl(lua_State* L) {
    enter(L);
    lua_settop(L, 1);
    lua_gettable(L, lua_upvalueindex(1));
    return 1;
}

int sandbox_gsub(lua_State* L) {
    if (lua_type(L, 3) == LUA_TTABLE && lua_getmetatable(L, 3)) {
        lua_pop(L, 1);
        lua_pushvalue(L, 3);
        lua_pushcclosure(L, table_repl, 1);
        lua_replace(L, 3);
    }
    return call_original(L);
}

// The library functions that make or hand out a C function at run time
// hand out a checked one. A generic for runs an instruction between two
// calls of its iterator, but a C loop that calls a scene's function per
// step (a sort comparator, __lt in sort and math.max, a gsub replacement,
// __index in table.move) runs none, and one call can be slow: a gmatch
// iterator that finds nothing rescans the rest of its subject every
// time, and a utf8.codes iterator given a position that is not a number
// starts over and skips every continuation byte it finds there (a whole
// string of them). pairs, ipairs and utf8.codes return the same
// function on every call, so each keeps one checked copy (pairs's is
// _G.next itself) and identities such as pairs({}) == next hold as in
// stock Lua; they follow Lua 5.5.1's lbaselib.c and lutf8lib.c (MIT).
// gmatch makes a new iterator per call, and each one is copied.

int pairs_cont(lua_State*, int, lua_KContext) { return 4; }

// Upvalue 2: the checked next.
int sandbox_pairs(lua_State* L) {
    luaL_checkany(L, 1);
    if (luaL_getmetafield(L, 1, "__pairs") == LUA_TNIL) {
        lua_pushvalue(L, lua_upvalueindex(2));
        lua_pushvalue(L, 1);
        lua_pushnil(L);
        lua_pushnil(L);
    } else {
        lua_pushvalue(L, 1);
        lua_callk(L, 1, 4, 0, pairs_cont);
    }
    return 4;
}

// Upvalue 2: the checked ipairs iterator.
int sandbox_ipairs(lua_State* L) {
    luaL_checkany(L, 1);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    return 3;
}

// Upvalues 2 and 3: the checked strict and lax iterators.
int sandbox_utf8_codes(lua_State* L) {
    int lax = lua_toboolean(L, 2);
    const char* s = luaL_checkstring(L, 1);
    luaL_argcheck(L, (static_cast<unsigned char>(*s) & 0xC0) != 0x80, 1, "invalid UTF-8 code");
    lua_pushvalue(L, lua_upvalueindex(lax ? 3 : 2));
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    return 3;
}

// Upvalue 2: the stock gmatch.
int sandbox_gmatch(lua_State* L) {
    call_original(L);
    push_checked_function(L, -1);
    return 1;
}

// Pushes a checked copy of the iterator the stock t[name]("", flag)
// returns, t being the table at `table`.
void push_checked_iterator(lua_State* L, int table, const char* name, bool flag) {
    table = lua_absindex(L, table);
    lua_getfield(L, table, name);
    lua_pushliteral(L, "");
    lua_pushboolean(L, flag);
    lua_call(L, 2, 1);
    push_checked_function(L, -1);
    lua_replace(L, -2);
}

// The table library's loops run in C: no instruction, so no hook, and
// reading or writing absent keys allocates nothing, so no memory limit
// either. Every count that drives one of them (an argument, or a length
// that may come from __len) is capped at kMaxTableRange elements, and so
// is the span each loop actually walks. Each loop also checks the limits
// every kLoopCheckEvery elements: one element can walk a long
// __index/__newindex chain without a call or an instruction. move,
// concat, unpack, insert and remove are written out after Lua 5.5.1's
// ltablib.c (MIT) for that; sort keeps the stock loop for plain tables
// (nothing there can run long) and copies anything else.
constexpr lua_Integer kMaxTableRange = lua_Integer{1} << 20;

// True if [first, last] holds more than kMaxTableRange elements.
bool range_too_large(lua_Integer first, lua_Integer last) {
    return last >= first && static_cast<lua_Unsigned>(last) - static_cast<lua_Unsigned>(first) >=
                                static_cast<lua_Unsigned>(kMaxTableRange);
}

int range_error(lua_State* L) { return luaL_error(L, "range too large"); }

// What a value must support to stand in for a table (ltablib's TAB_*).
constexpr int kTabR = 1;
constexpr int kTabW = 2;
constexpr int kTabL = 4;
constexpr int kTabRW = kTabR | kTabW;

bool has_field(lua_State* L, const char* key, int n) {
    lua_pushstring(L, key);
    return lua_rawget(L, -n) != LUA_TNIL;
}

// ltablib's checktab: a table, or a value whose metatable has every
// metamethod `what` needs (a string has a length without __len).
void checktab(lua_State* L, int arg, int what) {
    int tp = lua_type(L, arg);
    if (tp == LUA_TTABLE) return;
    int n = 1;
    if (lua_getmetatable(L, arg) && (!(what & kTabR) || has_field(L, "__index", ++n)) &&
        (!(what & kTabW) || has_field(L, "__newindex", ++n)) &&
        (!(what & kTabL) || tp == LUA_TSTRING || has_field(L, "__len", ++n)))
        lua_pop(L, n);
    else
        luaL_checktype(L, arg, LUA_TTABLE);
}

// The length of argument 1, as ltablib's aux_getn reads it, refused when
// it is over the cap or negative (only __len can make it negative, and
// that would turn the position checks into ranges of up to 2^64).
lua_Integer checked_len(lua_State* L, int what) {
    checktab(L, 1, what | kTabL);
    lua_Integer n = luaL_len(L, 1);
    if (n < 0 || n > kMaxTableRange) range_error(L);
    return n;
}

int sandbox_table_move(lua_State* L) {
    lua_Integer f = luaL_checkinteger(L, 2);
    lua_Integer e = luaL_checkinteger(L, 3);
    lua_Integer t = luaL_checkinteger(L, 4);
    int tt = !lua_isnoneornil(L, 5) ? 5 : 1;  // destination table
    checktab(L, 1, kTabR);
    checktab(L, tt, kTabW);
    if (e >= f) {
        luaL_argcheck(L, f > 0 || e < LUA_MAXINTEGER + f, 3, "too many elements to move");
        if (range_too_large(f, e)) return range_error(L);
        lua_Integer n = e - f + 1;
        luaL_argcheck(L, t <= LUA_MAXINTEGER - n + 1, 4, "destination wrap around");
        lua_Integer steps = 0;
        if (t > e || t <= f || (tt != 1 && !lua_compare(L, 1, tt, LUA_OPEQ))) {
            for (lua_Integer i = 0; i < n; i++) {
                tick(L, steps);
                lua_geti(L, 1, f + i);
                lua_seti(L, tt, t + i);
            }
        } else {
            for (lua_Integer i = n - 1; i >= 0; i--) {
                tick(L, steps);
                lua_geti(L, 1, f + i);
                lua_seti(L, tt, t + i);
            }
        }
    }
    lua_pushvalue(L, tt);
    return 1;
}

void add_field(lua_State* L, luaL_Buffer* b, lua_Integer i) {
    lua_geti(L, 1, i);
    if (!lua_isstring(L, -1))
        luaL_error(L, "invalid value (%s) at index %I in table for 'concat'", luaL_typename(L, -1),
                   static_cast<LUAI_UACINT>(i));
    luaL_addvalue(b);
}

int sandbox_table_concat(lua_State* L) {
    checktab(L, 1, kTabR | kTabL);
    lua_Integer last = luaL_len(L, 1);
    size_t lsep = 0;
    const char* sep = luaL_optlstring(L, 2, "", &lsep);
    lua_Integer i = luaL_optinteger(L, 3, 1);
    last = luaL_optinteger(L, 4, last);
    if (range_too_large(i, last)) return range_error(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    lua_Integer steps = 0;
    for (; i < last; i++) {
        tick(L, steps);
        add_field(L, &b, i);
        luaL_addlstring(&b, sep, lsep);
    }
    if (i == last) add_field(L, &b, i);
    luaL_pushresult(&b);
    return 1;
}

// The count is bounded by the Lua stack size (lua_checkstack), not by
// kMaxTableRange.
int sandbox_table_unpack(lua_State* L) {
    checktab(L, 1, kTabR | kTabL);
    lua_Integer len = luaL_len(L, 1);
    lua_Integer i = luaL_optinteger(L, 2, 1);
    lua_Integer e = lua_isnoneornil(L, 3) ? len : luaL_checkinteger(L, 3);
    if (i > e) return 0;
    lua_Unsigned n = static_cast<lua_Unsigned>(e) - static_cast<lua_Unsigned>(i);
    if (n >= static_cast<unsigned int>(INT_MAX) || !lua_checkstack(L, static_cast<int>(++n)))
        return luaL_error(L, "too many results to unpack");
    lua_Integer steps = 0;
    for (; i < e; i++) {
        tick(L, steps);
        lua_geti(L, 1, i);
    }
    lua_geti(L, 1, e);
    return static_cast<int>(n);
}

int sandbox_table_insert(lua_State* L) {
    lua_Integer e = checked_len(L, kTabRW) + 1;  // first empty slot
    lua_Integer pos = e;
    switch (lua_gettop(L)) {
        case 2:
            break;
        case 3: {
            pos = luaL_checkinteger(L, 2);
            luaL_argcheck(L, static_cast<lua_Unsigned>(pos) - 1u < static_cast<lua_Unsigned>(e), 2,
                          "position out of bounds");
            if (range_too_large(pos, e)) return range_error(L);
            lua_Integer steps = 0;
            for (lua_Integer i = e; i > pos; --i) {
                tick(L, steps);
                lua_geti(L, 1, i - 1);
                lua_seti(L, 1, i);
            }
            break;
        }
        default:
            return luaL_error(L, "wrong number of arguments to 'insert'");
    }
    lua_seti(L, 1, pos);
    return 0;
}

int sandbox_table_remove(lua_State* L) {
    lua_Integer size = checked_len(L, kTabRW);
    lua_Integer pos = luaL_optinteger(L, 2, size);
    if (pos != size)
        luaL_argcheck(L, static_cast<lua_Unsigned>(pos) - 1u <= static_cast<lua_Unsigned>(size), 2,
                      "position out of bounds");
    if (range_too_large(pos, size)) return range_error(L);
    lua_geti(L, 1, pos);
    lua_Integer steps = 0;
    for (; pos < size; ++pos) {
        tick(L, steps);
        lua_geti(L, 1, pos + 1);
        lua_seti(L, 1, pos);
    }
    lua_pushnil(L);
    lua_seti(L, 1, pos);
    return 1;
}

// A plain table's length involves no Lua code, so the original sort
// reads the same length that was checked, and its reads are raw. Anything
// with a metatable is sorted through a copy whose length is fixed:
// elements are read once (through __index), the copy is sorted, and the
// result is written back (through __newindex). Holes stay holes, so
// comparing one still fails.
int sandbox_table_sort(lua_State* L) {
    if (lua_type(L, 1) == LUA_TTABLE && !lua_getmetatable(L, 1)) {
        if (static_cast<lua_Integer>(lua_rawlen(L, 1)) > kMaxTableRange) return range_error(L);
        return call_original(L);
    }
    if (lua_type(L, 1) == LUA_TTABLE) lua_pop(L, 1);  // the metatable
    lua_Integer n = checked_len(L, kTabRW);
    if (n <= 1) return 0;
    if (!lua_isnoneornil(L, 2)) luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_settop(L, 2);
    lua_createtable(L, static_cast<int>(n), 0);  // 3: the copy
    lua_Integer steps = 0;
    for (lua_Integer i = 1; i <= n; ++i) {
        tick(L, steps);
        lua_geti(L, 1, i);
        lua_rawseti(L, 3, i);
    }
    lua_createtable(L, 0, 1);  // its metatable: __len = n
    lua_pushinteger(L, n);
    lua_pushcclosure(L, return_upvalue, 1);
    lua_setfield(L, -2, "__len");
    lua_setmetatable(L, 3);
    lua_pushvalue(L, lua_upvalueindex(2));  // the original sort
    lua_pushvalue(L, 3);
    lua_pushvalue(L, 2);
    lua_call(L, 2, 0);
    for (lua_Integer i = 1; i <= n; ++i) {
        tick(L, steps);
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
    : scene_id_(std::move(scene_id)),
      limits_(limits),
      now_ms_(std::move(now_ms)),
      exec_ms_(limits.exec_ms) {
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
    Vm* vm = Vm::of(L);
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

    // The library functions this file replaces. All of them, like every
    // other C function in the tables below, end up behind the entry check.
    const struct {
        const char* lib;  // nullptr: _G
        const char* name;
        lua_CFunction fn;
        bool keeps_original;
    } replaced[] = {
        {nullptr, "load", sandbox_load, true},
        {nullptr, "collectgarbage", sandbox_collectgarbage, true},
        {nullptr, "setmetatable", sandbox_setmetatable, true},
        {nullptr, "pcall", sandbox_pcall, false},
        {nullptr, "xpcall", sandbox_xpcall, false},
        {nullptr, "print", sandbox_print, false},
        {LUA_COLIBNAME, "resume", &Vm::co_resume, true},
        {LUA_COLIBNAME, "close", &Vm::co_close, true},
        {LUA_COLIBNAME, "wrap", &Vm::co_wrap, false},
        {LUA_STRLIBNAME, "gsub", sandbox_gsub, true},
        {LUA_STRLIBNAME, "gmatch", sandbox_gmatch, true},
        {LUA_TABLIBNAME, "move", sandbox_table_move, false},
        {LUA_TABLIBNAME, "concat", sandbox_table_concat, false},
        {LUA_TABLIBNAME, "unpack", sandbox_table_unpack, false},
        {LUA_TABLIBNAME, "insert", sandbox_table_insert, false},
        {LUA_TABLIBNAME, "remove", sandbox_table_remove, false},
        {LUA_TABLIBNAME, "sort", sandbox_table_sort, true},
    };
    for (const auto& r : replaced) {
        if (r.lib)
            lua_getglobal(L, r.lib);
        else
            lua_pushglobaltable(L);
        if (r.keeps_original) lua_getfield(L, -1, r.name);
        push_checked(L, r.fn, r.keeps_original ? 1 : 0);
        lua_setfield(L, -2, r.name);
        lua_pop(L, 1);
    }

    // pairs, ipairs and utf8.codes keep the iterator they hand out, a
    // checked copy of what the stock function returns (see sandbox_pairs);
    // pairs's is the next in _G.
    lua_pushglobaltable(L);
    lua_getfield(L, -1, "next");
    push_checked_function(L, -1);
    lua_replace(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, "next");
    push_checked(L, sandbox_pairs, 1);
    lua_setfield(L, -2, "pairs");
    push_checked_iterator(L, -1, "ipairs", false);
    push_checked(L, sandbox_ipairs, 1);
    lua_setfield(L, -2, "ipairs");
    lua_pop(L, 1);
    lua_getglobal(L, LUA_UTF8LIBNAME);
    push_checked_iterator(L, -1, "codes", false);
    push_checked_iterator(L, -2, "codes", true);
    push_checked(L, sandbox_utf8_codes, 2);
    lua_setfield(L, -2, "codes");
    lua_pop(L, 1);

    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, &Vm::import);
    lua_setfield(L, -2, "import");
    lua_setglobal(L, "wisp");

    for (const char* lib : {LUA_GNAME, LUA_COLIBNAME, LUA_MATHLIBNAME, LUA_STRLIBNAME,
                            LUA_TABLIBNAME, LUA_UTF8LIBNAME, "wisp"}) {
        lua_getglobal(L, lib);
        check_c_functions(L, -1);
        lua_pop(L, 1);
    }
    // The string metatable's arithmetic metamethods (its __index is the
    // string table, done above).
    lua_pushliteral(L, "");
    lua_getmetatable(L, -1);
    check_c_functions(L, -1);
    lua_pop(L, 2);

    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kImportCacheKey);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kImportStackKey);

    // The fault messages are made now, so raising one later never needs
    // to allocate (the heap may be full by then).
    char msg[64];
    format_fault(msg, sizeof(msg), FaultText::Timeout, vm->limits_);
    lua_pushstring(L, msg);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kTimeoutMsgKey);
    format_fault(msg, sizeof(msg), FaultText::Memory, vm->limits_);
    lua_pushstring(L, msg);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kMemoryMsgKey);
    format_fault(msg, sizeof(msg), FaultText::Budget, vm->limits_);
    lua_pushstring(L, msg);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &kBudgetMsgKey);
    return 0;
}

bool VmPool::reserve(size_t bytes) {
    size_t used = used_.load(std::memory_order_relaxed);
    do {
        if (bytes > budget_ - used) return false;
    } while (!used_.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed));
    return true;
}

void VmPool::release(size_t bytes) { used_.fetch_sub(bytes, std::memory_order_relaxed); }

void* Vm::alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    Vm* vm = static_cast<Vm*>(ud);
    VmPool* pool = vm->limits_.pool;
    // With ptr == nullptr, osize is the kind of object, not a size.
    size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        os::dealloc(ptr);
        vm->used_ -= old;
        if (pool != nullptr) pool->release(old);
        return nullptr;
    }
    if (nsize <= old) {
        // Lua assumes shrinking never fails; the old block is big enough.
        void* p = os::realloc(ptr, nsize, os::Mem::Large);
        vm->used_ -= old - nsize;
        if (pool != nullptr) pool->release(old - nsize);
        return p ? p : ptr;
    }
    void* p = nullptr;
    bool by_pool = false;
    const size_t grow = nsize - old;
    if (grow <= vm->limits_.memory_bytes - vm->used_) {
        if (pool == nullptr || pool->reserve(grow)) {
            p = os::realloc(ptr, nsize, os::Mem::Large);
            if (p == nullptr && pool != nullptr) pool->release(grow);
        } else {
            by_pool = true;
        }
    }
    bool same = vm->refused_ && vm->refused_ptr_ == ptr && vm->refused_osize_ == osize &&
                vm->refused_nsize_ == nsize;
    if (!p) {
        if (vm->refused_) {
            // Either the emergency GC's retry was refused too, or an
            // earlier refusal was never retried: the memory is gone. The
            // limit named is the one this request (a retry) or the
            // earlier one met.
            vm->fault((same ? by_pool : vm->refused_by_pool_) ? Fault::Budget : Fault::Memory);
        } else {
            vm->refused_ = true;
            vm->refused_by_pool_ = by_pool;
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
    vm->used_ += grow;
    if (vm->refused_) {
        vm->refused_ = false;
        if (!same) vm->fault(vm->refused_by_pool_ ? Fault::Budget : Fault::Memory);
    }
    return p;
}

void Vm::settle_refusal() {
    if (refused_) {
        refused_ = false;
        fault(refused_by_pool_ ? Fault::Budget : Fault::Memory);
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
    const void* key = fault_ == Fault::Memory   ? &kMemoryMsgKey
                      : fault_ == Fault::Budget ? &kBudgetMsgKey
                                                : &kTimeoutMsgKey;
    lua_rawgetp(L, LUA_REGISTRYINDEX, key);
    return lua_error(L);
}

void Vm::hook(lua_State* L, lua_Debug* ar) {
    Vm* vm = Vm::of(L);
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
        if (vm->now_ms_() - vm->start_ms_ <= vm->exec_ms_) return;
        vm->fault(Fault::Timeout);
        if (lua_getinfo(L, "Sl", ar) && ar->currentline > 0) {
            std::snprintf(vm->fault_file_, sizeof(vm->fault_file_), "%s", ar->short_src);
            vm->fault_line_ = ar->currentline;
        }
    }
    vm->raise_fault(L);
}

Vm* Vm::of(lua_State* L) {
    void* ud = nullptr;
    lua_getallocf(L, &ud);
    return static_cast<Vm*>(ud);
}

bool Vm::past_limits() const {
    if (refused_ || fault_ != Fault::None) return true;
    return running_ && now_ms_() - start_ms_ > exec_ms_;
}

void Vm::check_limits(lua_State* L) {
    settle_refusal();
    if (fault_ == Fault::None) {
        if (!running_ || now_ms_() - start_ms_ <= exec_ms_) return;
        fault(Fault::Timeout);
        // The innermost Lua function with a line, as the hook reports it.
        lua_Debug ar;
        for (int level = 0; lua_getstack(L, level, &ar); ++level) {
            if (lua_getinfo(L, "Sl", &ar) && ar.currentline > 0) {
                std::snprintf(fault_file_, sizeof(fault_file_), "%s", ar.short_src);
                fault_line_ = ar.currentline;
                break;
            }
        }
    }
    raise_fault(L);
}

// Runs the original coroutine function (upvalue 2, see push_checked) with
// every argument, tracking `co` as the running thread. A fault refuses to
// start anything (the entry check) and is raised again in the caller once
// control returns, so a coroutine
// cannot catch its child's fault and carry on. A refusal still pending
// when the child gives control back was not recovered by a retry, so it
// is settled (as a fault) before the caller runs another instruction.
int Vm::co_switch(lua_State* L, lua_State* co) {
    Vm* vm = Vm::of(L);
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(2));
    lua_insert(L, 1);
    lua_State* saved = vm->current_;
    if (co) vm->current_ = co;
    int status = lua_pcall(L, n, LUA_MULTRET, 0);
    vm->current_ = saved;
    vm->check_limits(L);
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
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_State* co = lua_newthread(L);  // copies L's hook
    lua_pushvalue(L, 1);
    lua_xmove(L, co, 1);
    lua_pushcclosure(L, &Vm::co_wrapped, 1);
    return 1;
}

// Not in any table, so it checks the limits itself.
int Vm::co_wrapped(lua_State* L) {
    Vm* vm = Vm::of(L);
    vm->check_limits(L);
    lua_State* co = lua_tothread(L, lua_upvalueindex(1));
    int narg = lua_gettop(L);
    if (!lua_checkstack(co, narg)) return luaL_error(L, "too many arguments to resume");
    lua_xmove(L, co, narg);
    lua_State* saved = vm->current_;
    vm->current_ = co;
    int nres = 0;
    int status = lua_resume(co, L, narg, &nres);
    vm->current_ = saved;
    vm->check_limits(L);
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
        vm->check_limits(L);
        lua_xmove(co, L, 1);
    }
    if (status != LUA_ERRMEM && lua_type(L, -1) == LUA_TSTRING) {
        luaL_where(L, 1);
        lua_insert(L, -2);
        lua_concat(L, 2);
    }
    return lua_error(L);
}

void push_checked_function(lua_State* L, int idx) {
    idx = lua_absindex(L, idx);
    lua_CFunction f = lua_tocfunction(L, idx);
    if (!f) luaL_error(L, "push_checked_function: not a C function");
    if (is_checked(f)) {
        lua_pushvalue(L, idx);
        return;
    }
    luaL_checkstack(L, kMaxCheckedUpvalues + 1, nullptr);
    if (!checkable(L, idx))
        luaL_error(L, "C function with more than %d upvalues cannot be checked",
                   kMaxCheckedUpvalues);
    int n = 0;
    while (lua_getupvalue(L, idx, n + 1)) ++n;
    lua_pushcfunction(L, f);
    lua_pushcclosure(L, kChecked[n], n + 1);
}

void check_c_functions(lua_State* L, int table) {
    table = lua_absindex(L, table);
    luaL_checkstack(L, 4, nullptr);
    // Every function is known to be checkable before the first field
    // changes, so a refusal leaves the table as it was.
    lua_pushnil(L);
    while (lua_next(L, table)) {
        lua_CFunction f = lua_tocfunction(L, -1);
        if (f && !is_checked(f) && !checkable(L, -1))
            luaL_error(L, "C function '%s' has more than %d upvalues and cannot be checked",
                       lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : "?",
                       kMaxCheckedUpvalues);
        lua_pop(L, 1);
    }
    lua_pushnil(L);
    while (lua_next(L, table)) {
        lua_CFunction f = lua_tocfunction(L, -1);
        if (f && !is_checked(f)) {
            // Changing an existing field is allowed while lua_next runs.
            lua_pushvalue(L, -2);
            push_checked_function(L, -2);
            lua_rawset(L, table);
        }
        lua_pop(L, 1);
    }
}

int Vm::import(lua_State* L) {
    Vm* vm = Vm::of(L);
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
    // The limit of the call that timed out, which may not be the default.
    VmLimits limits = limits_;
    limits.exec_ms = exec_ms_;
    format_fault(msg, sizeof(msg),
                 fault_ == Fault::Timeout  ? FaultText::Timeout
                 : fault_ == Fault::Budget ? FaultText::Budget
                                           : FaultText::Memory,
                 limits);
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

std::optional<ScriptError> Vm::call(int nargs, int nresults, Budget budget, uint32_t exec_ms) {
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
        if (budget == Budget::Fresh || !started_) start_ms_ = now_ms_();
        started_ = true;
        exec_ms_ = exec_ms != 0 ? exec_ms : limits_.exec_ms;
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

// "scene.<id>" whole, for every id scene.json allows.
constexpr size_t kSceneTagBytes = sizeof("scene.") - 1 + core::kMaxSceneIdBytes;
static_assert(kSceneTagBytes <= log::kMaxTagBytes);
// The log keeps the whole line.
static_assert(kLuaLogMaxBytes <= log::kMaxTextBytes);

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
    char tag[kSceneTagBytes + 1];
    std::snprintf(tag, sizeof(tag), "scene.%s", Vm::of(L)->scene_id().c_str());
    log::write_text(level, tag, line);
}

}  // namespace wisp::scene
