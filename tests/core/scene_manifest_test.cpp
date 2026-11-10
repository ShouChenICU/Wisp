#include "wisp/core/scene_manifest.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "wisp/base/error.hpp"

using wisp::ErrorCode;
using wisp::core::parse_scene_manifest;
using wisp::core::valid_scene_id;

namespace {

// A scene.json with every required field; `extra` is spliced in after
// them, before the closing brace.
std::string manifest(const std::string& id = "todo", const std::string& name = "x",
                     const std::string& version = "1.0.0", const std::string& extra = "") {
    return R"({"id":")" + id + R"(","name":")" + name + R"(","version":")" + version +
           R"(","schema_version":1)" + extra + "}";
}

bool accepts(const std::string& json) { return parse_scene_manifest(json).is_ok(); }

bool host_ok(const std::string& host) {
    return accepts(manifest("todo", "x", "1.0.0", R"(,"permissions":{"http":[")" + host + "\"]}"));
}

}  // namespace

TEST(SceneManifest, AcceptsSpecExample) {
    const std::string json =
        R"({"id": "todo", "name": "待办", "version": "1.0.0", "schema_version": 1,
 "entry": "main.lua", "icon": "assets/icon.png",
 "permissions": {"http": ["api.example.com"]}})";
    auto r = parse_scene_manifest(json);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().id, "todo");
    EXPECT_EQ(r.value().name, "待办");
    EXPECT_EQ(r.value().version, "1.0.0");
    EXPECT_EQ(r.value().schema_version, 1);
    EXPECT_EQ(r.value().entry, "main.lua");
    EXPECT_EQ(r.value().icon, "assets/icon.png");
    EXPECT_EQ(r.value().http_hosts, (std::vector<std::string>{"api.example.com"}));
}

TEST(SceneManifest, IdRules) {
    for (const char* id : {"todo", "a", "a-b_9"}) {
        EXPECT_TRUE(valid_scene_id(id)) << id;
    }
    EXPECT_TRUE(valid_scene_id(std::string(32, 'a')));
    for (const char* id : {"", "Todo", "a.b", "a/b", "中"}) {
        EXPECT_FALSE(valid_scene_id(id)) << id;
    }
    EXPECT_FALSE(valid_scene_id(std::string(33, 'a')));

    EXPECT_TRUE(accepts(manifest("a-b_9")));
    auto bad = parse_scene_manifest(manifest("Todo"));
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(accepts(R"({"name":"x","version":"1.0.0","schema_version":1})"));
}

TEST(SceneManifest, NameIsOneTo32BytesUtf8) {
    EXPECT_TRUE(accepts(manifest("todo", "待办")));
    EXPECT_TRUE(accepts(manifest("todo", std::string(32, 'n'))));
    EXPECT_FALSE(accepts(manifest("todo", "")));
    EXPECT_FALSE(accepts(manifest("todo", std::string(33, 'n'))));
    EXPECT_FALSE(accepts(manifest("todo", "\xff")));
    // 11 three-byte characters are 33 bytes.
    std::string cjk;
    for (int i = 0; i < 11; ++i) {
        cjk += "办";
    }
    EXPECT_FALSE(accepts(manifest("todo", cjk)));
}

TEST(SceneManifest, VersionIsSemver) {
    for (const char* v : {"1.0.0", "1.2.3-beta.1+b5", "0.0.0", "10.20.30", "1.0.0-0.3.7",
                          "1.0.0-x-y.z", "1.0.0+001"}) {
        EXPECT_TRUE(accepts(manifest("todo", "x", v))) << v;
    }
    for (const char* v : {"1.0", "v1.0.0", "01.0.0", "1.01.0", "1.0.00", "1.0.0-", "1.0.0-01",
                          "1.0.0-a..b", "1.0.0+", "1.0.0+a_b", "", "1.0.0.0"}) {
        EXPECT_FALSE(accepts(manifest("todo", "x", v))) << v;
    }
    // At most 64 bytes.
    EXPECT_TRUE(accepts(manifest("todo", "x", "1.0.0-" + std::string(58, 'a'))));
    EXPECT_FALSE(accepts(manifest("todo", "x", "1.0.0-" + std::string(59, 'a'))));
}

TEST(SceneManifest, SchemaEntryIconHosts) {
    EXPECT_FALSE(accepts(R"({"id":"todo","name":"x","version":"1.0.0"})"));

    auto def = parse_scene_manifest(manifest());
    ASSERT_TRUE(def);
    EXPECT_EQ(def.value().entry, "main.lua");
    EXPECT_EQ(def.value().icon, "");
    EXPECT_TRUE(def.value().http_hosts.empty());

    EXPECT_TRUE(accepts(manifest("todo", "x", "1.0.0", R"(,"entry":"app.lua")")));
    EXPECT_FALSE(accepts(manifest("todo", "x", "1.0.0", R"(,"entry":"lib/x.lua")")));
    EXPECT_FALSE(accepts(manifest("todo", "x", "1.0.0", R"(,"entry":"main.txt")")));
    EXPECT_FALSE(accepts(manifest("todo", "x", "1.0.0", R"(,"entry":".lua")")));

    EXPECT_TRUE(accepts(manifest("todo", "x", "1.0.0", R"(,"icon":"assets/i.png")")));
    EXPECT_FALSE(accepts(manifest("todo", "x", "1.0.0", R"(,"icon":"x.png")")));
    EXPECT_FALSE(accepts(manifest("todo", "x", "1.0.0", R"(,"icon":"assets/../x.png")")));

    EXPECT_TRUE(host_ok("api.example.com"));
    EXPECT_TRUE(host_ok("a-b.c0"));
    for (const char* h : {"*.a.com", "https://a.com", "a.com:443", "A.com", "a.com/x", ""}) {
        EXPECT_FALSE(host_ok(h)) << h;
    }
    EXPECT_TRUE(host_ok(std::string(253, 'a')));
    EXPECT_FALSE(host_ok(std::string(254, 'a')));

    std::string sixteen;
    for (int i = 0; i < 16; ++i) {
        sixteen += (i ? "," : "") + std::string("\"h") + std::to_string(i) + ".com\"";
    }
    EXPECT_TRUE(
        accepts(manifest("todo", "x", "1.0.0", R"(,"permissions":{"http":[)" + sixteen + "]}")));
    EXPECT_FALSE(accepts(manifest("todo", "x", "1.0.0",
                                  R"(,"permissions":{"http":[)" + sixteen + R"(,"x.com"]}})")));
    EXPECT_FALSE(accepts(manifest("todo", "x", "1.0.0", R"(,"permissions":{"http":"a.com"})")));

    // Unknown top-level fields are ignored.
    EXPECT_TRUE(accepts(manifest("todo", "x", "1.0.0", R"(,"author":"me","future":{"a":1})")));
    EXPECT_FALSE(accepts("not json"));
    EXPECT_FALSE(accepts("[]"));
}

TEST(SceneManifest, BackgroundField) {
    auto def = parse_scene_manifest(manifest());
    ASSERT_TRUE(def);
    EXPECT_FALSE(def.value().background);

    auto yes = parse_scene_manifest(manifest("todo", "x", "1.0.0", R"(,"background":true)"));
    ASSERT_TRUE(yes) << yes.error().message;
    EXPECT_TRUE(yes.value().background);

    auto no = parse_scene_manifest(manifest("todo", "x", "1.0.0", R"(,"background":false)"));
    ASSERT_TRUE(no) << no.error().message;
    EXPECT_FALSE(no.value().background);

    for (const char* bad :
         {R"(,"background":"yes")", R"(,"background":1)", R"(,"background":null)"}) {
        auto r = parse_scene_manifest(manifest("todo", "x", "1.0.0", bad));
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << bad;
        EXPECT_EQ(r.error().message, "scene.json: background must be a boolean") << bad;
    }
}

TEST(SceneManifest, FilesField) {
    using wisp::core::FilesWant;
    auto def = parse_scene_manifest(manifest());
    ASSERT_TRUE(def);
    EXPECT_EQ(def.value().files, FilesWant::Auto);
    const std::pair<const char*, FilesWant> ok[] = {{R"(,"files":"auto")", FilesWant::Auto},
                                                    {R"(,"files":"sd")", FilesWant::Sd},
                                                    {R"(,"files":"int")", FilesWant::Int}};
    for (const auto& [extra, want] : ok) {
        auto r = parse_scene_manifest(manifest("todo", "x", "1.0.0", extra));
        ASSERT_TRUE(r) << extra;
        EXPECT_EQ(r.value().files, want) << extra;
    }
    for (const char* bad :
         {R"(,"files":"SD")", R"(,"files":"flash")", R"(,"files":1)", R"(,"files":null)"}) {
        auto r = parse_scene_manifest(manifest("todo", "x", "1.0.0", bad));
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << bad;
        EXPECT_NE(r.error().message.find("files"), std::string::npos) << r.error().message;
    }
    EXPECT_STREQ(wisp::core::to_string(FilesWant::Auto), "auto");
    EXPECT_STREQ(wisp::core::to_string(FilesWant::Sd), "sd");
    EXPECT_STREQ(wisp::core::to_string(FilesWant::Int), "int");
}

// Parsing recurses once per level, and scene.json is parsed on the storage
// worker's small stack at every boot (wisp/core/storage_worker.hpp): its
// nesting is capped, unknown fields included. The cap is 8 so a tool's
// "parameters" schema (tools[].parameters.properties.<name>.type) still
// fits.
TEST(SceneManifest, NestingIsCappedAtEightLevels) {
    auto nested = [](int n) {
        std::string s(static_cast<size_t>(n), '[');
        s += "1";
        s.append(static_cast<size_t>(n), ']');
        return s;
    };
    // "x" itself is the first bracket, so depth = 1 (root) + n brackets.
    EXPECT_TRUE(accepts(manifest("todo", "x", "1.0.0", R"(,"x":)" + nested(7))));
    auto r = parse_scene_manifest(manifest("todo", "x", "1.0.0", R"(,"x":)" + nested(8)));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "scene.json: nested deeper than 8 levels");
}

namespace {
std::string tools_manifest(const std::string& tools_json) {
    return manifest("todo", "x", "1.0.0", R"(,"tools":)" + tools_json);
}
}  // namespace

// Spec 5.1's example, plus the defaults ("parameters" omitted, "timeout_ms"
// omitted) and the no-"tools" case.
TEST(SceneManifest, ToolsParsed) {
    const std::string json = tools_manifest(R"([
        {"name": "today",
         "description": "Today's weather for the saved city: temperature, condition, rain chance.",
         "parameters": {"type": "object", "properties": {"city": {"type": "string"}}},
         "timeout_ms": 30000}])");
    auto r = parse_scene_manifest(json);
    ASSERT_TRUE(r) << r.error().message;
    ASSERT_EQ(r.value().tools.size(), 1u);
    const auto& t = r.value().tools[0];
    EXPECT_EQ(t.name, "today");
    EXPECT_EQ(t.description,
              "Today's weather for the saved city: temperature, condition, rain chance.");
    EXPECT_EQ(t.parameters, R"({"properties":{"city":{"type":"string"}},"type":"object"})");
    EXPECT_EQ(t.timeout_ms, 30000u);

    auto r2 = parse_scene_manifest(tools_manifest(R"([{"name": "a", "description": "d"}])"));
    ASSERT_TRUE(r2) << r2.error().message;
    ASSERT_EQ(r2.value().tools.size(), 1u);
    EXPECT_EQ(r2.value().tools[0].parameters, "");
    EXPECT_EQ(r2.value().tools[0].timeout_ms, 30000u);

    auto r3 = parse_scene_manifest(manifest());
    ASSERT_TRUE(r3) << r3.error().message;
    EXPECT_TRUE(r3.value().tools.empty());

    // "parameters" serializing to exactly 4096 bytes is still accepted
    // (4088 'x' bytes plus the fixed '{"d":"' ... '"}' wrapper).
    auto at_cap = parse_scene_manifest(
        tools_manifest(R"([{"name":"a","description":"d","parameters":{"d":")" +
                       std::string(4088, 'x') + R"("}}])"));
    ASSERT_TRUE(at_cap) << at_cap.error().message;
    EXPECT_EQ(at_cap.value().tools[0].parameters.size(), 4096u);
}

TEST(SceneManifest, ToolsRejected) {
    std::string seventeen = "[";
    for (int i = 0; i < 17; ++i) {
        if (i != 0) seventeen += ",";
        seventeen += R"({"name":"t)" + std::to_string(i) + R"(","description":"d"})";
    }
    seventeen += "]";
    auto too_many = parse_scene_manifest(tools_manifest(seventeen));
    ASSERT_FALSE(too_many);
    EXPECT_EQ(too_many.error().message, "scene.json: tools lists more than 16 tools");

    auto with_name = [](const std::string& name) {
        return tools_manifest(R"([{"name":")" + name + R"(","description":"d"}])");
    };
    for (const std::string& name :
         {std::string("Today"), std::string("1a"), std::string(33, 'a')}) {
        auto r = parse_scene_manifest(with_name(name));
        ASSERT_FALSE(r) << name;
        EXPECT_EQ(r.error().message, "scene.json: tools[0].name must match ^[a-z][a-z0-9_]{0,31}$")
            << name;
    }

    auto dup = parse_scene_manifest(
        tools_manifest(R"([{"name":"a","description":"d"},{"name":"a","description":"d2"}])"));
    ASSERT_FALSE(dup);
    EXPECT_EQ(dup.error().message, "scene.json: tools[1].name 'a' is already used");

    auto empty_desc = parse_scene_manifest(tools_manifest(R"([{"name":"a","description":""}])"));
    ASSERT_FALSE(empty_desc);
    EXPECT_EQ(empty_desc.error().message,
              "scene.json: tools[0].description must be 1-512 code points of valid UTF-8");

    const std::string long_desc(513, 'd');
    auto too_long = parse_scene_manifest(
        tools_manifest(R"([{"name":"a","description":")" + long_desc + R"("}])"));
    ASSERT_FALSE(too_long);
    EXPECT_EQ(too_long.error().message,
              "scene.json: tools[0].description must be 1-512 code points of valid UTF-8");

    auto bad_params = parse_scene_manifest(
        tools_manifest(R"([{"name":"a","description":"d","parameters":"x"}])"));
    ASSERT_FALSE(bad_params);
    EXPECT_EQ(bad_params.error().message, "scene.json: tools[0].parameters must be an object");

    // "parameters" serializing to 4097 bytes (one 'x' over the cap).
    auto too_big = parse_scene_manifest(
        tools_manifest(R"([{"name":"a","description":"d","parameters":{"d":")" +
                       std::string(4089, 'x') + R"("}}])"));
    ASSERT_FALSE(too_big);
    EXPECT_EQ(too_big.error().message,
              "scene.json: tools[0].parameters is larger than 4096 bytes serialized");

    for (const char* timeout : {"999", "120001"}) {
        auto r = parse_scene_manifest(tools_manifest(
            R"([{"name":"a","description":"d","timeout_ms":)" + std::string(timeout) + "}]"));
        ASSERT_FALSE(r) << timeout;
        EXPECT_EQ(r.error().message,
                  "scene.json: tools[0].timeout_ms must be an integer from 1000 to 120000")
            << timeout;
    }
}

// Spec 7.3: an integer from 1 to 1000.
TEST(SceneManifest, SchemaVersionRange) {
    auto with = [](const std::string& v) {
        return R"({"id":"todo","name":"x","version":"1.0.0","schema_version":)" + v + "}";
    };
    for (const char* bad : {"0", "1001", "1.5", "\"2\"", "-1", "null"}) {
        auto r = parse_scene_manifest(with(bad));
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << bad;
    }
    for (int ok : {1, 2, 1000}) {
        auto r = parse_scene_manifest(with(std::to_string(ok)));
        ASSERT_TRUE(r) << ok;
        EXPECT_EQ(r.value().schema_version, ok);
    }
}
