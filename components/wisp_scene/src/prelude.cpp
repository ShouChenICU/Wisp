// The Lua half of the reactive core (see wisp/scene/reactive.hpp for
// the semantics). Trusted code: install_prelude() compiles it on its own,
// without global_preamble(), and calls it with three C functions:
//   emit(node, prop, value)  PatchBatch::set_prop()
//   where()                  "file:line" of the innermost Lua frame that
//                            is not this chunk's, or nil
//   tcall(f, x)              f(x) in an inner lua_pcall: true, result or
//                            false, error. A C call boundary, so f cannot
//                            yield; faults are raised again on return.
// It returns the internal table and the state, computed and bind
// functions; install_prelude() sets the globals and keeps the internal
// table in the registry.
//
// Every object handed to scene code is an empty proxy table with a hidden
// metatable; its data is a record in `recs`, keyed by the proxy (weak
// keys, so a dropped proxy can be collected). The graph links records:
//   state     k v o
//   computed  k v o s c f e d b at l  (bind: fn and src instead of f)
//   effect    id s q dead run [fn | node prop src quiet]
// k kind, v value, o observers (weak set), s sources (set), c true for
// computed and bind, e evaluator, d dirty, b being evaluated, at where it
// was made ("main.lua:3"), l label for messages, q queued for the flush, dead disposed.
// Observer sets are weak: a computed nobody holds any more drops out of
// its sources' sets. Effects are held by `live` until disposed.

#include <string_view>

namespace wisp::scene {

std::string_view prelude_source() {
    return R"lua(
local emit, where, tcall = ...
global<const> setmetatable, type, error, pairs, rawequal, table, math

local setmetatable, type, error, pairs, rawequal = setmetatable, type, error, pairs, rawequal
local sort, concat, mtype = table.sort, table.concat, math.type

local MAX_ROUNDS = 100
local WEAK = {__mode = "k"}

local recs = setmetatable({}, WEAK)
local current = nil          -- the computed or effect being run, if any
local stack, depth = {}, 0   -- computeds being evaluated, outermost first
local queue, queued = {}, 0  -- effects to run at the next flush
local live = {}              -- effect -> true until disposed
local next_id = 0

-- Raises msg at the scene line that led here. A tail call in scene code
-- (`return a:get()`) leaves no frame to find; then the computed being
-- evaluated, if any, is blamed where it was made.
local function fail(msg)
    local at = where() or (depth > 0 and stack[depth].at) or nil
    if at then msg = at .. ": " .. msg end
    error(msg, 0)
end

local function read_only()
    fail("reactive objects are read-only (use :set(v) on a state)")
end

local state_methods, computed_methods = {}, {}
local STATE = {__index = state_methods, __newindex = read_only, __metatable = false}
local COMPUTED = {__index = computed_methods, __newindex = read_only, __metatable = false}
local SCOPE = {__newindex = read_only, __metatable = false}

local function source_rec(obj)
    local r = recs[obj]
    if r ~= nil and r.k ~= "scope" then return r end
    return nil
end

local function track(src)
    local obs = current
    if obs ~= nil then
        obs.s[src] = true
        src.o[obs] = true
    end
end

local function untrack(obs)
    local sources = obs.s
    for src in pairs(sources) do
        src.o[obs] = nil
        sources[src] = nil
    end
end

local function schedule(e)
    if not e.q then
        e.q = true
        queued = queued + 1
        queue[queued] = e
    end
end

-- Marks everything downstream of src: computeds dirty, effects queued.
local function notify(src)
    for obs in pairs(src.o) do
        if obs.c then
            if not obs.d then
                obs.d = true
                notify(obs)
            end
        elseif not obs.dead then
            schedule(obs)
        end
    end
end

local function cycle_message(c)
    local from = depth
    while stack[from] ~= c do from = from - 1 end
    local parts, n = {}, 0
    for i = from, depth do
        n = n + 1
        parts[n] = stack[i].l
    end
    parts[n + 1] = c.l
    return "cyclic dependency: " .. concat(parts, " -> ")
end

local read

local function eval_computed(c) return c.f() end
local function eval_bind(c) return c.fn(read(c.src)) end

local function evaluate(c)
    if c.b then fail(cycle_message(c)) end
    untrack(c)
    c.b = true
    depth = depth + 1
    stack[depth] = c
    local saved = current
    current = c
    local ok, v = tcall(c.e, c)
    current = saved
    stack[depth] = nil
    depth = depth - 1
    c.b = false
    if not ok then error(v, 0) end
    c.v = v
    c.d = false
    return v
end

function read(src)
    track(src)
    if src.d then return evaluate(src) end
    return src.v
end

function state_methods.get(self)
    local r = recs[self]
    if r == nil or r.k ~= "state" then fail("expected a state (call it as s:get())") end
    track(r)
    return r.v
end

function state_methods.set(self, v)
    local r = recs[self]
    if r == nil or r.k ~= "state" then fail("expected a state (call it as s:set(v))") end
    if current ~= nil and current.c then fail("state:set() inside computed") end
    if rawequal(r.v, v) then return end
    r.v = v
    notify(r)
end

function computed_methods.get(self)
    local r = recs[self]
    if r == nil or not r.c then fail("expected a computed or bind (call it as c:get())") end
    return read(r)
end

local function state(v)
    local p = setmetatable({}, STATE)
    recs[p] = {k = "state", v = v, o = setmetatable({}, WEAK)}
    return p
end

local function computed(fn)
    if type(fn) ~= "function" then fail("computed: expected a function") end
    local p = setmetatable({}, COMPUTED)
    local at = where()
    recs[p] = {k = "computed", c = true, f = fn, e = eval_computed, d = true, b = false,
               s = {}, o = setmetatable({}, WEAK), at = at, l = "computed@" .. (at or "?")}
    return p
end

local function bind(source, fn)
    local src = source_rec(source)
    if src == nil then fail("bind: expected a state, computed or bind as the first argument") end
    if type(fn) ~= "function" then fail("bind: expected a function as the second argument") end
    local p = setmetatable({}, COMPUTED)
    local at = where()
    recs[p] = {k = "bind", c = true, fn = fn, src = src, e = eval_bind, d = true, b = false,
               s = {}, o = setmetatable({}, WEAK), at = at, l = "bind@" .. (at or "?")}
    return p
end

-- Effects ------------------------------------------------------------

local function run_effect(e)
    untrack(e)
    local saved = current
    current = e
    local ok, v = tcall(e.run, e)
    current = saved
    return ok, v
end

local function dispose_effect(e)
    if e.dead then return end
    e.dead = true
    untrack(e)
    live[e] = nil
end

local function scope_rec(scope)
    if scope == nil then return nil end
    local r = recs[scope]
    if r == nil or r.k ~= "scope" then fail("expected a scope") end
    if r.dead then fail("scope already disposed") end
    return r
end

-- Registers e, runs it once (tracked) and returns its disposer and the
-- run's result.
local function start(e, sc)
    next_id = next_id + 1
    e.id = next_id
    e.s = {}
    e.q = false
    e.dead = false
    live[e] = true
    if sc ~= nil then
        sc.n = sc.n + 1
        sc.members[sc.n] = e
    end
    local ok, v = run_effect(e)
    if not ok then
        dispose_effect(e)
        error(v, 0)
    end
    return function() dispose_effect(e) end, v
end

local function effect_run(e) e.fn() end

local function watch_run(e)
    local v = read(e.src)
    if e.quiet then return v end
    local t = type(v)
    if t ~= "nil" and t ~= "boolean" and t ~= "number" and t ~= "string" then
        fail("a widget property bound to " .. (e.src.l or "a state") .. " got a " .. t ..
             " (it takes nil, a boolean, a number or a string)")
    end
    emit(e.node, e.prop, v)
end

local function drop_queue()
    for i = 1, queued do queue[i].q = false end
    queue, queued = {}, 0
end

local function by_id(a, b) return a.id < b.id end

local function flush()
    local rounds = 0
    while queued > 0 do
        rounds = rounds + 1
        if rounds > MAX_ROUNDS then
            drop_queue()
            error("reactive updates did not settle", 0)
        end
        local batch, n = queue, queued
        queue, queued = {}, 0
        if n > 1 then sort(batch, by_id) end
        for i = 1, n do
            local e = batch[i]
            e.q = false
            if not e.dead then
                local ok, err = run_effect(e)
                if not ok then
                    for j = i + 1, n do batch[j].q = false end
                    drop_queue()
                    error(err, 0)
                end
            end
        end
    end
end

local W = {}

local function check_id(v, max, what)
    if mtype(v) ~= "integer" or v < 0 or v > max then fail("watch: bad " .. what) end
end

function W.watch(node, prop, source, scope)
    check_id(node, 0xFFFFFFFF, "node id")
    check_id(prop, 0xFFFF, "property id")
    local src = source_rec(source)
    if src == nil then fail("watch: expected a state, computed or bind") end
    local e = {node = node, prop = prop, src = src, run = watch_run, quiet = true}
    local dispose, v = start(e, scope_rec(scope))
    e.quiet = false
    return dispose, v
end

function W.effect(fn, scope)
    if type(fn) ~= "function" then fail("effect: expected a function") end
    local dispose = start({fn = fn, run = effect_run}, scope_rec(scope))
    return dispose
end

function W.scope()
    local p = setmetatable({}, SCOPE)
    recs[p] = {k = "scope", members = {}, n = 0, dead = false}
    return p
end

function W.dispose(scope)
    local r = recs[scope]
    if r == nil or r.k ~= "scope" then fail("dispose: expected a scope") end
    if r.dead then return end
    r.dead = true
    for i = 1, r.n do dispose_effect(r.members[i]) end
    r.members, r.n = {}, 0
end

function W.refresh_all()
    for e in pairs(live) do
        if e.node ~= nil then schedule(e) end
    end
    flush()
end

function W.kind(v)
    local r = recs[v]
    return r and r.k
end

W.flush = flush
W.emit = emit

return W, state, computed, bind
)lua";
}

}  // namespace wisp::scene
