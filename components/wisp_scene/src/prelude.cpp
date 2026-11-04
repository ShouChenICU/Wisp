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
//   computed  k v o s c f e d b failed at l  (bind: fn and src, not f)
//   effect    id s q dead run [fn | node prop src quiet]
// k kind, v value, o observers (weak set), s sources (set), c true for
// computed and bind, e evaluator, d dirty, b being evaluated, failed its
// last evaluation raised, at where it was made ("main.lua:3"), l label
// for messages, q queued for the flush, dead disposed.
// Observer sets are weak: a computed nobody holds any more drops out of
// its sources' sets. Effects are held by `live` until disposed.

#include <string_view>

namespace wisp::scene {

std::string_view prelude_source() {
    return R"lua(
local emit, where, tcall = ...
global<const> setmetatable, type, error, pairs, rawequal, rawget, rawlen, table, math, string

local setmetatable, type, error, pairs, rawequal = setmetatable, type, error, pairs, rawequal
local rawget, rawlen = rawget, rawlen
local sort, concat, mtype, match = table.sort, table.concat, math.type, string.match

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

-- Raises an error caught from a run of `rec` again. An error Lua raised
-- while a prelude function was running ("C stack overflow" at the
-- nesting limit) carries a position inside the prelude; it is moved to
-- the scene line that led here, or to where `rec` was made.
local function rethrow(err, rec)
    if type(err) == "string" then
        local msg = match(err, "^wisp%.reactive:%d+: (.*)$")
        if msg then
            local at = where() or rec.at
            error(at and (at .. ": " .. msg) or msg, 0)
        end
    end
    error(err, 0)
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
-- A computed that is already dirty normally stops the walk, since its
-- observers were told when it became dirty. One whose last evaluation
-- failed is the exception: a reader that caught the error (pcall) has
-- subscribed since, while it stayed dirty, so the walk goes on through
-- it once.
local function notify(src)
    for obs in pairs(src.o) do
        if obs.c then
            if not obs.d or obs.failed then
                obs.d = true
                obs.failed = false
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
    if not ok then
        c.failed = true
        rethrow(v, c)
    end
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
        rethrow(v, e)
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
        if e.node ~= nil or e.redo then schedule(e) end
    end
    flush()
end

-- Widgets fed by tables (list, canvas, chart) -------------------------

-- The widget effects of the tree collected last, and its lists:
-- lists[id] = {items = the table shown, row = fn, rows = {[index] =
-- {sc = scope, ids = {node ids}}}} with 0-based row indexes.
local widgets = {}
local lists = {}

-- A widget effect: it first runs at the next flush, after the op that
-- creates its widget is in the batch, and refresh_all() runs it again,
-- since it may re-send ops a collapsed patch queue dropped.
local function widget_effect(fn, sc)
    next_id = next_id + 1
    local e = {fn = fn, run = effect_run, id = next_id, s = {}, q = false, dead = false,
               redo = true}
    live[e] = true
    widgets[e] = true
    if sc ~= nil then
        sc.n = sc.n + 1
        sc.members[sc.n] = e
    end
    schedule(e)
end

-- The value a widget property holds now: a state, computed or bind is
-- read (tracked), anything else is the value itself.
local function current_value(v)
    local src = source_rec(v)
    if src ~= nil then return read(src) end
    return v
end

local function drop_row(list, index, r)
    local nodes, values = W.nodes, W.values
    W.dispose(r.sc)
    local ids = r.ids
    for i = 1, #ids do
        nodes[ids[i]] = nil
        values[ids[i]] = nil
    end
    list.rows[index] = nil
end

local function drop_rows(list)
    for index, r in pairs(list.rows) do drop_row(list, index, r) end
end

-- W.widget(id, kind, a, b [, scope]): starts the effect behind node id
-- (collect_tree() calls it). list: a = items, b = row; canvas: a = draw;
-- chart: a = series. The C functions it uses (list_reset, canvas_begin,
-- canvas_end, set_series) are added to W by install_ui().
function W.widget(id, kind, a, b, scope)
    local sc = scope_rec(scope)
    if kind == "list" then
        local list = {row = b, rows = {}}
        lists[id] = list
        widget_effect(function()
            local items = current_value(a)
            W.list_reset(id, items)
            drop_rows(list)
            list.items = items
        end, sc)
    elseif kind == "canvas" then
        widget_effect(function()
            local g = W.canvas_begin(id)
            a(g)
            W.canvas_end(g)
        end, sc)
    elseif kind == "chart" then
        widget_effect(function() W.set_series(id, current_value(a)) end, sc)
    end
end

-- Stops the widget effects and drops the lists of the tree collected
-- last (collect_tree() calls it before collecting a new one).
function W.reset_widgets()
    for e in pairs(widgets) do dispose_effect(e) end
    widgets = {}
    for _, list in pairs(lists) do drop_rows(list) end
    lists = {}
end

-- W.build_rows(id, first, count, collect_row): makes rows [first,
-- first + count) of list id exist and drops the others (0-based). Each
-- new row is row(item, index) (1-based) in a scope of its own, handed to
-- collect_row(index, node, scope, ids), which collects it and fills ids.
-- An unknown list, or one whose items have not been read yet, has no
-- rows to build.
function W.build_rows(id, first, count, collect_row)
    local list = lists[id]
    if list == nil or list.items == nil then return end
    local items = list.items
    local n = rawlen(items)
    local last = first + count
    if last > n then last = n end
    for index, r in pairs(list.rows) do
        if index < first or index >= last then drop_row(list, index, r) end
    end
    for index = first, last - 1 do
        if list.rows[index] == nil then
            local sc = W.scope()
            local ids = {}
            local node = list.row(rawget(items, index + 1), index + 1)
            list.rows[index] = {sc = sc, ids = ids}
            collect_row(index, node, sc, ids)
        end
    end
end

function W.kind(v)
    local r = recs[v]
    return r and r.k
end

-- ui.show_if's source for hidden: true while s is falsy.
function W.negate(s)
    return bind(s, function(v) return not v end)
end

W.flush = flush
W.emit = emit

return W, state, computed, bind
)lua";
}

}  // namespace wisp::scene
