// wisp.store on a running SceneRuntime (scene-runtime-core spec 5.4, 5.6,
// 6.1).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "support/scene_runtime_fixture.hpp"
#include "wisp/base/value.hpp"

namespace {

using wisp::Map;
using wisp::Value;
using wisp::core::SceneState;

const char* kScreen = "return ui.screen { ui.label { text = \"x\" } }\n";

class StoreBinding : public wisp::test::SceneRuntimeFixture {
protected:
    // The scene's data file `name` ("kv.log", "todos.log"); empty if none.
    std::string data_file(const std::string& id, const std::string& name) {
        std::ifstream f(std::filesystem::path(repo->data_dir(id)) / name, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    static Value mode(const char* m) { return Value(Map{{"mode", Value(m)}}); }
};

TEST_F(StoreBinding, KvAndCollectionsFromLua) {
    start();
    install("kv", std::string(R"(
local s = wisp.store
print(s.get("missing"))
print(s.set("k", {a = 1, b = {"x", "y"}}))
local v = s.get("k")
print(v.a, v.b[1], v.b[2], math.type(v.a))
v.a = 2
print(s.get("k").a)
print(s.set("n", 3.5), s.get("n"), s.set("t", true), s.get("t"))
print(s.set("k", nil), s.get("k"), s.set("never", nil))

local c = s.collection("todos")
print(c:insert({title = "a", due = 3, done = false}))
print(c:insert({title = "b", due = 5, done = false}))
print(c:insert({title = "c", due = 9, done = true}))
local r = c:find({done = false}, {sort = "due", desc = true, limit = 1})
print(#r, r[1].id, r[1].title, r[1].due, tostring(r[1].done))
print(c:update(1, {title = "a2", due = 1, done = true}))
print(c:update(7, {title = "none"}))
print(c:delete(3))
print(c:delete(3))
local all = c:find()
print(#all, all[1].id, all[1].title, all[2].id, all[2].title)
local page = c:find(nil, {sort = "due", offset = 1})
print(#page, page[1].id)
print(#c:find({title = "zzz"}), #s.collection("empty"):find())

local ok, e = pcall(s.set, "", 1)
print(ok, e:find("bad argument #1", 1, true) ~= nil)
ok, e = pcall(s.collection, "Bad Name")
print(ok, e:find("bad argument #1", 1, true) ~= nil)
ok, e = pcall(c.insert, c, {1, 2})
print(ok, e:find("bad argument", 1, true) ~= nil)
ok, e = pcall(c.insert, c, 5)
print(ok, e:find("bad argument", 1, true) ~= nil)
ok, e = pcall(c.find, c, {[1] = true})
print(ok, e:find("bad argument", 1, true) ~= nil)
print(getmetatable(c))
)") + kScreen);
    ASSERT_TRUE(rt->open("kv"));
    EXPECT_EQ(scene_log("kv"), (std::vector<std::string>{
                                   "nil",
                                   "true",
                                   "1\tx\ty\tinteger",
                                   "1",
                                   "true\t3.5\ttrue\ttrue",
                                   "true\tnil\ttrue",
                                   "1",
                                   "2",
                                   "3",
                                   "1\t2\tb\t5\tfalse",
                                   "true",
                                   "nil\tnot_found",
                                   "true",
                                   "nil\tnot_found",
                                   "2\t1\ta2\t2\tb",
                                   "1\t2",
                                   "0\t0",
                                   "false\ttrue",
                                   "false\ttrue",
                                   "false\ttrue",
                                   "false\ttrue",
                                   "false\ttrue",
                                   "false",
                               }));
    EXPECT_EQ(rt->status("kv").state, SceneState::Running);
}

// `id` is the record id everywhere: injected by find, never stored,
// matched by filter.id and ordered by sort = "id".
TEST_F(StoreBinding, IdIsReserved) {
    start();
    install("ids", std::string(R"(
local c = wisp.store.collection("todos")
print(c:insert({t = "a", done = false, id = 99}))
print(c:insert({t = "b", done = false}))
print(c:insert({t = "c", done = false}))
-- The stored record has no id of its own.
print(c:find({id = 1})[1].id, #c:find({id = 99}))
-- find -> modify -> update round trip.
local r = c:find({id = 2})[1]
r.done = true
print(c:update(r.id, r))
r = c:find({id = 2})[1]
print(r.id, r.t, tostring(r.done))
print(#c:find({id = 2, done = true}), #c:find({id = 2, done = false}), #c:find({id = 3.0}))
-- update with a foreign id field: the record id stays.
print(c:update(3, {t = "c2", done = false, id = 7}))
print(#c:find({id = 7}), c:find({id = 3})[1].t)
local function ids(list)
  local out = {}
  for i, rec in ipairs(list) do out[i] = rec.id end
  return table.concat(out, ",")
end
print(ids(c:find(nil, {sort = "id", desc = true})))
print(ids(c:find(nil, {sort = "id"})))
print(ids(c:find({done = false}, {sort = "id", desc = true, limit = 1})))
print(ids(c:find(nil, {desc = true, offset = 1})))
local ok, e = pcall(c.find, c, {id = "2"})
print(ok, e:find("bad argument", 1, true) ~= nil)
ok, e = pcall(c.find, c, {id = 2.5})
print(ok, e:find("bad argument", 1, true) ~= nil)
)") + kScreen);
    ASSERT_TRUE(rt->open("ids")) << rt->status("ids").error;
    EXPECT_EQ(scene_log("ids"), (std::vector<std::string>{
                                    "1",
                                    "2",
                                    "3",
                                    "1\t0",
                                    "true",
                                    "2\tb\ttrue",
                                    "1\t0\t1",
                                    "true",
                                    "0\tc2",
                                    "3,2,1",
                                    "1,2,3",
                                    "3",
                                    "2,1",
                                    "false\ttrue",
                                    "false\ttrue",
                                }));
    // Nothing on disk carries an id field inside the value.
    ASSERT_TRUE(rt->close("ids"));
    ASSERT_TRUE(worker.drain(2000));
    std::istringstream todos(data_file("ids", "todos.log"));
    int lines = 0;
    for (std::string line; std::getline(todos, line); ++lines) {
        const size_t v = line.find(R"("v":)");
        ASSERT_NE(v, std::string::npos) << line;
        EXPECT_EQ(line.find(R"("id")", v), std::string::npos) << line;
    }
    EXPECT_GE(lines, 3);  // appended or compacted
}

TEST_F(StoreBinding, RecordKeysCheckedWhateverTheOrder) {
    start();
    install("keys", std::string(R"(
local c = wisp.store.collection("todos")
-- Many string keys and one boolean key: wherever traversal meets it, the
-- error is the same argument error.
local t = {}
for i = 1, 300 do t["k" .. i] = i end
t[true] = 1
for _ = 1, 3 do
  local ok, e = pcall(c.insert, c, t)
  print(ok, e:find("bad argument", 1, true) ~= nil)
end
local u = {x = 1}
u[1] = 2
local ok, e = pcall(c.update, c, 1, u)
print(ok, e:find("bad argument", 1, true) ~= nil)
-- A filter value too large to match any record finds nothing.
c:insert({s = "x"})
print(#c:find({s = string.rep("x", 20000)}), #c:find({s = {string.rep("x", 20000)}}))
)") + kScreen);
    ASSERT_TRUE(rt->open("keys")) << rt->status("keys").error;
    EXPECT_EQ(scene_log("keys"), (std::vector<std::string>{
                                     "false\ttrue",
                                     "false\ttrue",
                                     "false\ttrue",
                                     "false\ttrue",
                                     "0\t0",
                                 }));
}

TEST_F(StoreBinding, ErrorsReturnedAsNilAndCode) {
    start();
    install("err", std::string(R"(
local s = wisp.store
-- The encoded value is 2 bytes longer (the quotes).
print(s.set("big", string.rep("x", 16382)))
print(s.set("big", string.rep("x", 16383)))
print(s.collection("c0"):insert({v = string.rep("y", 16383)}))
print(s.set("f", print))
local deep = {} for i = 2, 33 do deep = {deep} end
print(s.set("deep", deep))
print(s.set("nan", 0/0))
for i = 1, 16 do s.collection("c" .. i):insert({}) end
print(s.collection("c17"):insert({}))
print(s.collection("c1"):insert({}))
-- 1 MB of data in all.
local row = {v = string.rep("z", 16000)}
local code
for i = 1, 80 do
  local id, e = s.collection("c2"):insert(row)
  if not id then code = e break end
end
print(code)
print(s.get("f"), s.get("deep"), s.get("nan"))
)") + kScreen);
    ASSERT_TRUE(rt->open("err"));
    EXPECT_EQ(scene_log("err"), (std::vector<std::string>{
                                    "true",
                                    "nil\trecord_too_large",
                                    "nil\trecord_too_large",
                                    "nil\tnot_serializable",
                                    "nil\ttoo_deep",
                                    "nil\tnot_serializable",
                                    "nil\ttoo_many_collections",
                                    "2",
                                    "quota_exceeded",
                                    "nil\tnil\tnil",
                                }));
    EXPECT_EQ(rt->status("err").state, SceneState::Running);
}

const char* kBatchScene = R"(
local a = wisp.scene.args()
local s = wisp.store
local c = s.collection("todos")
if a.mode == "seed" then
  print(s.batch(function()
    s.set("before", 1)
    c:insert({x = 1})
  end))
elseif a.mode == "error" then
  s.batch(function()
    s.set("k", 2)
    c:insert({x = 2})
    error("boom")
  end)
elseif a.mode == "loop" then
  s.batch(function()
    s.set("k", 3)
    c:insert({x = 3})
    while true do end
  end)
elseif a.mode == "caught" then
  print(s.batch(function()
    s.set("k", 4)
    print(pcall(s.batch, function() c:insert({x = 4}) error("inner") end))
    print(s.get("k"))
  end))
elseif a.mode == "show" then
  local all = c:find()
  print(s.get("before"), s.get("k"), #all, all[1] and all[1].x)
end
)";

TEST_F(StoreBinding, BatchErrorAndTimeoutRollBack) {
    start();
    install("b", std::string(kBatchScene) + kScreen);
    ASSERT_TRUE(rt->open("b", mode("seed")));
    EXPECT_EQ(scene_log("b"), (std::vector<std::string>{"true"}));

    // An error inside the batch faults the scene and writes nothing.
    EXPECT_FALSE(rt->open("b", mode("error")));
    EXPECT_EQ(rt->status("b").state, SceneState::Error);
    EXPECT_NE(rt->status("b").error.find("boom"), std::string::npos) << rt->status("b").error;
    ASSERT_TRUE(rt->open("b", mode("show")));
    EXPECT_EQ(scene_log("b").back(), "1\tnil\t1\t1");

    // So does running out of time inside it.
    EXPECT_FALSE(rt->open("b", mode("loop")));
    EXPECT_EQ(rt->status("b").state, SceneState::Error);
    EXPECT_NE(rt->status("b").error.find("execution exceeded"), std::string::npos)
        << rt->status("b").error;
    ASSERT_TRUE(rt->open("b", mode("show")));
    EXPECT_EQ(scene_log("b").back(), "1\tnil\t1\t1");

    // An inner batch whose error is caught fails the outer one.
    ASSERT_TRUE(rt->open("b", mode("caught")));
    auto log = scene_log("b");
    ASSERT_GE(log.size(), 3u);
    EXPECT_EQ(log[log.size() - 3].rfind("false\t", 0), 0u) << log[log.size() - 3];
    EXPECT_NE(log[log.size() - 3].find("inner"), std::string::npos);
    EXPECT_EQ(log[log.size() - 2], "4");
    EXPECT_EQ(log.back(), "nil\tbatch_aborted");
    ASSERT_TRUE(rt->reload("b"));
    ASSERT_TRUE(rt->open("b", mode("show")));
    EXPECT_EQ(scene_log("b").back(), "1\tnil\t1\t1");

    // What reached the disk: the seed batch only.
    ASSERT_TRUE(rt->close("b"));
    ASSERT_TRUE(worker.drain(2000));
    const std::string kv = data_file("b", "kv.log");
    const std::string todos = data_file("b", "todos.log");
    EXPECT_NE(kv.find(R"("before")"), std::string::npos) << kv;
    EXPECT_EQ(kv.find(R"("k")"), std::string::npos) << kv;
    EXPECT_NE(todos.find(R"("x":1)"), std::string::npos) << todos;
    EXPECT_EQ(todos.find(R"("x":2)"), std::string::npos) << todos;
    EXPECT_EQ(todos.find(R"("x":3)"), std::string::npos) << todos;
    EXPECT_EQ(todos.find(R"("x":4)"), std::string::npos) << todos;
}

TEST_F(StoreBinding, DataSurvivesReload) {
    start();
    install("keep", std::string(R"(
local s = wisp.store
local c = s.collection("notes")
local n = s.get("opens") or 0
s.set("opens", n + 1)
s.set("nested", {list = {1, 2.5, "三"}, flag = false})
c:insert({text = "note " .. (n + 1)})
local all = c:find(nil, {sort = "text", desc = true})
local v = s.get("nested")
print(n + 1, #all, all[1].id, all[1].text, v.list[2], v.list[3], tostring(v.flag))
)") + kScreen);
    ASSERT_TRUE(rt->open("keep"));
    ASSERT_TRUE(rt->reload("keep"));
    ASSERT_TRUE(rt->close("keep"));
    ASSERT_TRUE(rt->open("keep"));
    EXPECT_EQ(scene_log("keep"), (std::vector<std::string>{
                                     "1\t1\t1\tnote 1\t2.5\t三\tfalse",
                                     "2\t2\t2\tnote 2\t2.5\t三\tfalse",
                                     "3\t3\t3\tnote 3\t2.5\t三\tfalse",
                                 }));
}

}  // namespace
