#pragma once

// What constructors.cpp (the ui table) and collect.cpp (collect_tree)
// share about nodes, and the value checks behind the constructors and
// prop_value_ok(); see wisp/scene/schema.hpp.
//
// A node is a userdata (NodeHeader) with two user values: its record
// table and the file it was built in. The record keeps the children at
// 1..n and property p at prop_key(p). User values and a hidden metatable
// put both out of the scene's reach, so collect_tree() can trust what it
// walks: the properties were checked when the node was built, a node is
// a child of at most one parent (marked in its header), and only C code
// changes a record afterwards (ui.show_if).
//
// Everything here that runs inside Lua follows the longjmp rule in
// vm.hpp: frames that can raise hold only trivially destructible locals
// (char buffers, lua_Debug, string_view arrays).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "lua.hpp"
#include "wisp/scene/schema.hpp"

namespace wisp::scene::node {

inline constexpr const char* kNodeType = "wisp.node";

// Registry keys (their addresses, as light userdata), defined in
// node.cpp: the prelude's kind, watch and negate; the tree tables of the
// last collect_tree(); and whether a collect_tree() is running.
extern char kKindKey;
extern char kWatchKey;
extern char kNegateKey;
extern char kNodesKey;
extern char kRefsKey;
extern char kValuesKey;
extern char kCollectingKey;

struct NodeHeader {
    uint16_t widget;
    uint16_t children;  // at 1..children of the record
    uint8_t attached;   // already the child of another node
    int32_t line;       // 0 if unknown
};

inline lua_Integer prop_key(Prop p) { return -1 - static_cast<lua_Integer>(p); }

inline std::string_view widget_name(Widget w) { return widget_spec(w).name; }

// Fresh nodes, refs and values tables, in the registry (for C) and in the
// internal table at `internal` (for Lua).
void reset_tree_tables(lua_State* L, int internal);

// The node at idx, or nullptr if it is anything else.
NodeHeader* to_node(lua_State* L, int idx);

struct Where {
    char file[LUA_IDSIZE];
    int line;
};

// The scene line the running C function was called from (scene_where()).
void where(lua_State* L, Where& at);

// Where the node at `node` was built: writes the file, returns the line.
int node_position(lua_State* L, int node, char (&file)[LUA_IDSIZE]);

// Raises the formatted message (lua_pushfstring formats), prefixed with
// "file:line: " when the position is known.
int raise(lua_State* L, const char* file, int line, const char* fmt, ...);

std::string_view string_at(lua_State* L, int idx);

// Copies at most kMaxShown bytes of s into out (cut on a UTF-8 boundary,
// "..." marking the cut) and returns out.
inline constexpr size_t kMaxShown = 40;
const char* shown(std::string_view s, char (&out)[kMaxShown + 4]);

// nearest_name() into a fixed buffer, so its std::string is gone before
// the caller raises.
void nearest_into(char (&out)[24], std::string_view wrong, std::span<const std::string_view> names);

// "state", "computed" or "bind" if the value at idx is one, else nullptr.
// Calls the prelude's kind(), which runs no scene code; can raise.
const char* reactive_kind(lua_State* L, int idx);

enum class Check { Ok, WrongType, BadValue, TooLarge };

// Checks a value that is not a reactive object; nil passes. Never raises.
Check check_value(lua_State* L, int idx, Prop p);

// An integer, or a float with an integral value, within int32.
bool integral(lua_State* L, int idx, lua_Integer& out);

// Writes "property 'p' expects ..., got ..." (or "property 'p': value
// too large") for a failed check; `kind` names a reactive object that was
// given where it is not allowed. Never raises.
void describe_problem(lua_State* L, int idx, Prop p, Check c, const char* kind, char* out,
                      size_t size);

// Raises "ui.<widget>: <describe_problem()>" at file:line.
int raise_prop(lua_State* L, const char* file, int line, Widget w, Prop p, Check c, int idx,
               const char* kind);

}  // namespace wisp::scene::node
