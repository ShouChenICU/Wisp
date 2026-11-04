#pragma once

// Fine-grained reactive state for scenes (scene-runtime-core spec 4.2).
//
// install_prelude() runs a Lua prelude (src/prelude.cpp) in the scene's
// VM. It defines three globals for scene code:
//   state(v)      :get() reads (and, inside a computed or an effect,
//                 records the read as a dependency); :set(v) writes. A
//                 write equal to the old value (rawequal: by value for
//                 numbers, strings and booleans, by reference for tables)
//                 changes nothing.
//   computed(fn)  :get() returns fn()'s value. Lazy: fn runs on the first
//                 :get() and again on the first :get() after one of the
//                 values it read last time changed, so dependencies follow
//                 the branch fn took. A cycle between computeds is reported
//                 when it is evaluated ("cyclic dependency: computed@main.lua:3
//                 -> computed@main.lua:4 -> computed@main.lua:3"); fn may not
//                 call :set() ("state:set() inside computed").
//   bind(s, fn)   a computed whose value is fn(s:get()); s is a state,
//                 computed or bind. Same rules as computed.
// There is no equality cut-off below state: once a state changes,
// everything that read it runs again at the next flush, even if it then
// produces the same value.
//
// The objects are tables with a hidden metatable; assigning a field is an
// error. Evaluating a computed goes through a C call boundary (an inner
// lua_pcall), so a computed cannot yield, an error inside one leaves the
// tracking state as it was, and each level of computeds nested inside
// another's evaluation uses one of the LUAI_MAXCCALLS (48) C call levels:
// a chain of about 40 computeds evaluated in one go is the limit.
//
// The internal table (the prelude's name for it is __wisp) is for the
// runtime only. It is not a global: global_preamble() does not declare
// the name, so scene code cannot write it, and it is not stored in _G
// either, so _G.__wisp and rawget(_G, "__wisp") are nil. The runtime gets
// it with push_wisp_internal(). Its fields:
//   watch(node, prop, source [, scope]) -> disposer, value
//       Binds a widget property to a state/computed/bind. Reads source
//       now (tracked) and returns its value for the initial tree; emits
//       nothing until the source changes. Then, at the flush, emits the
//       new value with emit(); the value must be nil, a boolean, a number
//       or a string. disposer() stops it (calling it again is harmless).
//   effect(fn [, scope]) -> disposer
//       Runs fn now, tracked, and again at the flush after anything it
//       read changes. Unlike a computed, fn may call :set(); that schedules
//       another round of the same flush.
//   scope() -> scope; dispose(scope)
//       Watches and effects made with a scope stop together when it is
//       disposed (list rows). A disposed scope refuses new members.
//   flush()
//       Runs every scheduled watch and effect in creation order, round
//       after round while they keep scheduling more, at most 100 rounds
//       ("reactive updates did not settle"). flush_reactive() calls it.
//   refresh_all()
//       Schedules every live watch and flushes, so each emits its current
//       value once (after the patch queue dropped patches).
//   emit(node, prop, value)
//       The C function behind watch: PatchBatch::set_prop().
//   kind(v) -> "state" | "computed" | "bind" | "scope" | nil
// Later parts of the runtime may add fields to it.
//
// Everything the prelude does runs as scene code, inside the time and
// memory limits of the message that triggers it: state:set() and the
// computeds and watches it schedules are charged to that message, and so
// is the flush (flush_reactive() continues the message's time budget).
// After any error that reaches Vm::call() the scene is in the error
// state (spec 3.3) and the VM is destroyed, so the graph is not repaired.

#include <optional>

#include "lua.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/vm.hpp"

namespace wisp::scene {

// Runs the prelude in `vm` (a fresh time budget) and makes `out` the
// batch emit() writes to; `out` must outlive the VM. Call once, before
// any scene code runs.
std::optional<ScriptError> install_prelude(Vm& vm, PatchBatch& out);

// Runs the internal flush() under Vm::call() with Budget::Continue, so it
// is charged to the message that just ran. On an error the whole batch is
// discarded (PatchBatch::clear()). Does nothing without the prelude.
std::optional<ScriptError> flush_reactive(Vm& vm);

// Pushes the internal table and returns true, or pushes nothing and
// returns false if install_prelude() has not run in this state. Never
// raises.
bool push_wisp_internal(lua_State* L);

// The batch install_prelude() gave this state, or nullptr. Never raises.
PatchBatch* patch_batch(lua_State* L);

}  // namespace wisp::scene
