#pragma once

// scene.json, the manifest at the root of every scene package (spec 6.2):
//
//   {"id": "todo", "name": "待办", "version": "1.0.0", "schema_version": 1,
//    "entry": "main.lua", "icon": "assets/icon.png",
//    "permissions": {"http": ["api.example.com"]}, "background": true,
//    "files": "auto",
//    "tools": [
//      {"name": "today",
//       "description": "Today's weather for the saved city: temperature, condition, rain chance.",
//       "parameters": {"type": "object", "properties": {"city": {"type": "string"}}},
//       "timeout_ms": 30000}]}
//
// parse_scene_manifest() checks every field it knows and ignores unknown
// top-level fields, so a newer package with extra metadata still installs.
// Nothing, unknown fields included, may nest deeper than 8 levels (the
// root object is level 1): scene.json's own structure needs up to 4
// (permissions.http[]), and a tool's "parameters" needs up to 4 more for a
// JSON-Schema shape like type/properties/<name>/type (agent-tools spec
// 5.1). "parameters" is only checked here for being an object no larger
// than kMaxToolSchemaBytes serialized; wisp_core cannot depend on
// wisp_agent, so whether it is a schema_supported() shape is checked where
// a tool is installed (agent-tools spec 5.1, task 7).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"

namespace wisp::core {

// Where the scene's files/ area lives (scene-device spec 7.2):
// Auto picks the SD card when one is present at the first write and
// internal storage otherwise; Sd never falls back (no card means
// "unavailable"); Int always uses internal storage. Once chosen, the
// location is recorded and a later change of declaration moves nothing.
enum class FilesWant { Auto, Sd, Int };

// "auto", "sd" or "int".
const char* to_string(FilesWant w);

// One entry of scene.json's "tools" array (agent-tools spec 5.1): a scene
// tool a scene's main.lua binds a function to with wisp.tools.register and
// the agent calls by its full name "<scene id>.<name>".
struct SceneToolDecl {
    std::string name;             // ^[a-z][a-z0-9_]{0,31}$, unique within the scene
    std::string description;      // 1-512 code points of valid UTF-8
    std::string parameters;       // compact JSON of the schema; "" for none
    uint32_t timeout_ms = 30000;  // 1000-120000
};

// The most tools one scene.json may declare (agent-tools spec 5.1).
inline constexpr size_t kMaxSceneTools = 16;
// The largest a tool's "parameters" schema may serialize to (agent-tools
// spec 5.1).
inline constexpr size_t kMaxToolSchemaBytes = 4096;

struct SceneManifest {
    std::string id;                  // [a-z0-9_-], 1-32 bytes; also the directory name
    std::string name;                // 1-32 bytes of valid UTF-8
    std::string version;             // SemVer 2.0, at most 64 bytes
    std::string entry = "main.lua";  // a top-level *.lua file
    std::string icon;                // empty, or a file under assets/
    // The data's structure version, 1-1000 (scene-device spec 7.3).
    int schema_version = 1;
    std::vector<std::string> http_hosts;  // permissions.http, at most 16
    // Going back sends the scene to the background instead of closing it
    // (scene-device spec 3.2).
    bool background = false;
    FilesWant files = FilesWant::Auto;  // "files"; absent means Auto
    std::vector<SceneToolDecl> tools;   // "tools"; at most kMaxSceneTools
};

// The longest scene id.
inline constexpr size_t kMaxSceneIdBytes = 32;

// True if `id` matches ^[a-z0-9_-]{1,32}$.
bool valid_scene_id(std::string_view id);

// Parses and validates scene.json. Every failure is InvalidArgument with a
// message naming the field ("scene.json: version ...").
Result<SceneManifest> parse_scene_manifest(std::string_view json);

}  // namespace wisp::core
