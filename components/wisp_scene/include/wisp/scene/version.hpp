#pragma once

namespace wisp::scene {

// The vendored Lua's release string (LUA_RELEASE, e.g. "Lua 5.5.1"), for
// boot reports and diagnostics without exposing Lua's headers to callers.
const char* lua_release();

}  // namespace wisp::scene
