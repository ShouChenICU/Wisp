#pragma once

// scene.json, the manifest at the root of every scene package (spec 6.2):
//
//   {"id": "todo", "name": "待办", "version": "1.0.0", "schema_version": 1,
//    "entry": "main.lua", "icon": "assets/icon.png",
//    "permissions": {"http": ["api.example.com"]}, "background": true}
//
// parse_scene_manifest() checks every field it knows and ignores unknown
// top-level fields, so a newer package with extra metadata still installs.
// Nothing, unknown fields included, may nest deeper than 4 levels (the
// root object is level 1).

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"

namespace wisp::core {

struct SceneManifest {
    std::string id;                  // [a-z0-9_-], 1-32 bytes; also the directory name
    std::string name;                // 1-32 bytes of valid UTF-8
    std::string version;             // SemVer 2.0, at most 64 bytes
    std::string entry = "main.lua";  // a top-level *.lua file
    std::string icon;                // empty, or a file under assets/
    int schema_version = 1;
    std::vector<std::string> http_hosts;  // permissions.http, at most 16
    // Going back sends the scene to the background instead of closing it
    // (scene-device spec 3.2).
    bool background = false;
};

// The longest scene id.
inline constexpr size_t kMaxSceneIdBytes = 32;

// True if `id` matches ^[a-z0-9_-]{1,32}$.
bool valid_scene_id(std::string_view id);

// Parses and validates scene.json. Every failure is InvalidArgument with a
// message naming the field ("scene.json: version ...").
Result<SceneManifest> parse_scene_manifest(std::string_view json);

}  // namespace wisp::core
