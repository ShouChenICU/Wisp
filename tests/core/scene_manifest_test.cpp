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
    EXPECT_FALSE(accepts(R"({"id":"todo","name":"x","version":"1.0.0","schema_version":2})"));
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
// nesting is capped, unknown fields included.
TEST(SceneManifest, NestingIsCappedAtFourLevels) {
    // permissions.http[] is three deep; one more level is still fine.
    EXPECT_TRUE(accepts(manifest("todo", "x", "1.0.0", R"(,"x":{"y":[[1]]})")));
    for (const char* extra :
         {R"(,"x":{"y":[[[1]]]})", R"(,"x":[[[[[[[[[1]]]]]]]]])",
          R"(,"x":{"a":{"a":{"a":{"a":1}}}})", R"(,"permissions":{"http":[[["a.com"]]]})"}) {
        auto r = parse_scene_manifest(manifest("todo", "x", "1.0.0", extra));
        ASSERT_FALSE(r) << extra;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << extra;
        EXPECT_EQ(r.error().message, "scene.json: nested deeper than 4 levels") << extra;
    }
}
