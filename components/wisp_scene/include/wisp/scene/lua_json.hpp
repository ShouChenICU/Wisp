#pragma once

// Lua values <-> JSON text, for wisp.store (scene-runtime-core spec 5.4).
//
// Encoding: nil -> null; booleans; integers as integers; finite floats
// with a fraction or exponent so they decode as floats again (3.0 ->
// "3.0"); strings must be UTF-8. A table whose keys are exactly 1..n
// (n >= 1) is an array, one whose keys are all strings an object, an
// empty table {}. Tables are read raw (no metamethods). Mixed or other
// keys, functions, userdata, threads, NaN/inf, invalid UTF-8 and a table
// that contains itself are "not_serializable"; tables nested more than
// kLuaJsonMaxDepth deep are "too_deep"; output past `max_bytes` is
// "record_too_large".
//
// Decoding: objects -> tables, arrays -> sequences, null -> nil (an
// array element or object field that is null is absent), numbers without
// fraction or exponent that fit int64 -> integers, other numbers ->
// floats. Nesting is capped at kLuaJsonMaxDepth as well.
//
// Both follow the longjmp rule (vm.hpp) and fit the scene task's C stack:
// recursion is capped at kLuaJsonMaxDepth levels with small frames. L
// must belong to a Vm (its limits are checked).

#include <cstddef>
#include <string_view>

#include "lua.hpp"
#include "wisp/os/large.hpp"

namespace wisp::scene {

inline constexpr int kLuaJsonMaxDepth = 32;
// The default output cap: the scene data quota (spec 5.6), so no value
// that could be stored is refused and none can run PSRAM out.
inline constexpr size_t kLuaJsonMaxBytes = 1024 * 1024;
// Lua stack slots lua_to_json() needs free; a binding reserves them with
// luaL_checkstack before it holds C++ objects.
inline constexpr int kLuaJsonStackSlots = 2 * kLuaJsonMaxDepth + 8;

// Arrays, not pointers to literals: each is one object program-wide, so
// callers can compare a returned code with == (a literal's address may
// differ between translation units).
inline constexpr char kJsonNotSerializable[] = "not_serializable";
inline constexpr char kJsonTooDeep[] = "too_deep";
inline constexpr char kJsonTooLarge[] = "record_too_large";
// The VM's time or memory limit was reached during the walk; the caller
// must call Vm::check_limits() once its C++ objects are gone.
inline constexpr char kJsonLimits[] = "limits";

// Encodes the value at `idx` into `out` (replacing its contents).
// Returns nullptr on success or one of the kJson* codes above; never
// raises a Lua error and leaves the Lua stack as it was. Checks
// Vm::past_limits() every 256 values.
const char* lua_to_json(lua_State* L, int idx, os::LargeString& out,
                        size_t max_bytes = kLuaJsonMaxBytes);

// lua_to_json() for a wisp.store record: a top-level "id" field is left
// out, since a record's id is the store's own (find() adds it back).
const char* lua_record_to_json(lua_State* L, int idx, os::LargeString& out,
                               size_t max_bytes = kLuaJsonMaxBytes);

// Decodes `json` and pushes the value. On failure (malformed text, too
// deep, out of memory, the VM's limits) returns false with the stack as
// it was; the caller then calls Vm::check_limits() once its C++ objects
// are gone, so a timeout or memory fault still reaches Vm::call().
// Runs inside protect(), so it never raises.
bool push_json(lua_State* L, std::string_view json);

// push_json() without the protection: raises a Lua error on malformed
// text and checks the VM's limits every 256 values. For code already
// inside protect() (or another lua_pcall) with no C++ objects on its
// frames. `json` must stay valid until it returns.
void decode_json(lua_State* L, std::string_view json);

}  // namespace wisp::scene
