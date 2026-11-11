// SceneWorkspace and the scene authoring tools (agent-core spec 4.1), the
// check after an install (spec 5.1) and the ui.tree tool (spec 5.2), on a
// real SceneRuntime with a circular screen.

#include "wisp/agent/scene_workspace.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/os/flash_guard.hpp"
#include "wisp/scene/ui_inspector.hpp"

namespace {

namespace fs = std::filesystem;

using wisp::ErrorCode;
using wisp::Value;
using wisp::agent::SceneWorkspace;
using wisp::agent::ToolCtx;
using wisp::agent::ToolOutput;
using wisp::core::SceneState;
using wisp::test::wait_until;

const char* kCounter = R"(local count = state(0)

return ui.screen { pad = 60,
  ui.column { gap = 12, align = "center",
    ui.label { ref = "count", text = bind(count, function(v) return "count " .. v end) },
    ui.button { ref = "inc", text = "+1", on_click = function() count:set(count:get() + 1) end },
  },
}
)";

std::string counter_json(const std::string& version = "1.0.0", const std::string& hosts = "") {
    std::string out =
        R"({"id":"counter","name":"计数器","version":")" + version + R"(","schema_version":1)";
    if (!hosts.empty()) out += R"(,"permissions":{"http":)" + hosts + "}";
    return out + "}";
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

const Value* field(const Value& v, std::string_view key) { return v.find(key); }

std::string str(const Value& v, std::string_view key) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == Value::Type::String ? f->as_string() : std::string();
}

class SceneWorkspaceTest : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        ctx.cancelled = [] { return false; };
        ctx.lang = "zh";
    }
    void TearDown() override {
        ws.reset();
        inspector.reset();
        SceneRuntimeFixture::TearDown();
    }

    // Starts the runtime with the counter installed (unless `installed`
    // is false) and makes the workspace.
    void begin(bool installed = true) {
        if (installed) install("counter", kCounter, {{"scene.json", counter_json()}});
        start("circle");
        inspector = std::make_unique<wisp::scene::UiInspector>(
            wisp::scene::InspectorDeps{*ui, *layer, *rt, shape});
        make_ws();
    }

    void make_ws(wisp::core::SceneRepository* use_repo = nullptr) {
        ws.reset();
        ws = std::make_unique<SceneWorkspace>(wisp::agent::SceneWorkspaceDeps{
            .repo = use_repo != nullptr ? *use_repo : *repo,
            .runtime = *rt,
            .storage = *storage,
            .worker = worker,
            .confirm =
                [this](const std::string& text, ToolCtx&) {
                    confirms.push_back(text);
                    return answer;
                },
            .drafts_root = in + "/scenes/.drafts",
            .inspector = inspector.get(),
            .on_tools_changed =
                [this](const wisp::core::SceneManifest& manifest) {
                    tool_changes.push_back(manifest);
                },
        });
    }

    fs::path draft(const std::string& id) const { return fs::path(in) / "scenes" / ".drafts" / id; }
    fs::path installed(const std::string& id) const { return fs::path(in) / "scenes" / id; }

    std::vector<std::string> draft_files(const std::string& id) const {
        std::vector<std::string> out;
        std::error_code ec;
        if (!fs::exists(draft(id), ec)) return out;
        for (const auto& e : fs::recursive_directory_iterator(draft(id)))
            if (e.is_regular_file()) out.push_back(fs::relative(e.path(), draft(id)).string());
        std::sort(out.begin(), out.end());
        return out;
    }

    void write(const std::string& id, const std::string& path, const std::string& body) {
        auto r = ws->write_file(id, path, body);
        ASSERT_TRUE(r) << r.error().message;
    }

    const Value* scene_in_list(const std::string& id) {
        listed = ws->list();
        for (const Value& v : listed.as_list())
            if (str(v, "id") == id) return &v;
        return nullptr;
    }

    std::unique_ptr<wisp::scene::UiInspector> inspector;
    std::unique_ptr<SceneWorkspace> ws;
    ToolCtx ctx;
    std::vector<std::string> confirms;
    std::vector<wisp::core::SceneManifest> tool_changes;
    bool answer = true;
    Value listed;
};

}  // namespace

TEST_F(SceneWorkspaceTest, FirstEditCopiesInstalledIntoDraft) {
    install(
        "counter", kCounter,
        {{"scene.json", counter_json()}, {"lib/util.lua", "return {}\n"}, {"assets/a.png", "png"}});
    begin(false);
    ASSERT_FALSE(ws->has_draft("counter"));
    ASSERT_EQ(str(*scene_in_list("counter"), "version"), "1.0.0");
    EXPECT_FALSE(scene_in_list("counter")->find("draft")->as_bool());

    write("counter", "lib/more.lua", "return 2\n");
    EXPECT_EQ(draft_files("counter"),
              (std::vector<std::string>{"assets/a.png", "lib/more.lua", "lib/util.lua", "main.lua",
                                        "scene.json"}));
    EXPECT_EQ(slurp(draft("counter") / "main.lua"), kCounter);
    EXPECT_TRUE(scene_in_list("counter")->find("draft")->as_bool());
    // The installed package is untouched.
    EXPECT_FALSE(fs::exists(installed("counter") / "lib" / "more.lua"));
}

TEST_F(SceneWorkspaceTest, NewSceneStartsWithAnEmptyDraft) {
    begin(false);
    write("fresh", "main.lua", "return ui.screen {}\n");
    EXPECT_EQ(draft_files("fresh"), (std::vector<std::string>{"main.lua"}));
    const Value* e = scene_in_list("fresh");
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(str(*e, "state"), "draft");
    EXPECT_TRUE(e->find("draft")->as_bool());
    auto files = ws->list_files("fresh");
    ASSERT_TRUE(files) << files.error().message;
    EXPECT_TRUE(files.value().find("draft")->as_bool());
    ASSERT_EQ(files.value().find("files")->as_list().size(), 1u);
    EXPECT_EQ(ws->list_files("nothing").error().code, ErrorCode::NotFound);
}

TEST_F(SceneWorkspaceTest, ReadPrefersDraft) {
    begin();
    auto before = ws->read_file("counter", "main.lua");
    ASSERT_TRUE(before) << before.error().message;
    EXPECT_EQ(std::string(before.value().c_str()), kCounter);
    write("counter", "main.lua", "return ui.screen {}\n");
    auto after = ws->read_file("counter", "main.lua");
    ASSERT_TRUE(after);
    EXPECT_EQ(std::string(after.value().c_str()), "return ui.screen {}\n");
    auto files = ws->list_files("counter");
    ASSERT_TRUE(files);
    EXPECT_TRUE(files.value().find("draft")->as_bool());
    EXPECT_EQ(ws->read_file("counter", "lib/none.lua").error().code, ErrorCode::NotFound);
}

TEST_F(SceneWorkspaceTest, ReadOver64KiBIsTooLarge) {
    begin();
    write("counter", "assets/big.txt", std::string(65537, 'a'));
    write("counter", "assets/ok.txt", std::string(65536, 'a'));
    auto big = ws->read_file("counter", "assets/big.txt");
    ASSERT_FALSE(big);
    EXPECT_EQ(wisp::agent::error_code(big.error()), "too_large");
    auto ok = ws->read_file("counter", "assets/ok.txt");
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_EQ(ok.value().size(), 65536u);
}

TEST_F(SceneWorkspaceTest, EditRequiresExactlyOneMatch) {
    begin();
    size_t count = 99;
    auto none = ws->edit_file("counter", "main.lua", "nowhere", "x", &count);
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code, ErrorCode::NotFound);
    EXPECT_EQ(wisp::agent::error_code(none.error()), "not_found");
    EXPECT_EQ(count, 0u);
    auto two = ws->edit_file("counter", "main.lua", "count", "n", &count);
    ASSERT_FALSE(two);
    EXPECT_EQ(two.error().code, ErrorCode::AlreadyExists);
    EXPECT_EQ(wisp::agent::error_code(two.error()), "not_unique");
    EXPECT_GE(count, 2u);
    // Not overlapping: "aaaa" holds "aa" twice.
    write("counter", "lib/a.lua", "aaaa");
    ASSERT_FALSE(ws->edit_file("counter", "lib/a.lua", "aa", "b", &count));
    EXPECT_EQ(count, 2u);
    auto one = ws->edit_file("counter", "main.lua", "\"+1\"", "\"plus\"", &count);
    ASSERT_TRUE(one) << one.error().message;
    EXPECT_EQ(count, 1u);
    EXPECT_NE(slurp(draft("counter") / "main.lua").find("\"plus\""), std::string::npos);
}

TEST_F(SceneWorkspaceTest, PathRulesFollowPackages) {
    begin();
    for (const char* bad : {"../x", "/x", "data/k", "x.txt", "files/a", "lib/x.txt"}) {
        auto r = ws->write_file("counter", bad, "x");
        ASSERT_FALSE(r) << bad;
        EXPECT_EQ(wisp::agent::error_code(r.error()), "bad_path") << bad;
        EXPECT_EQ(wisp::agent::error_code(ws->read_file("counter", bad).error()), "bad_path");
    }
    write("counter", "lib/u.lua", "return 1\n");
    write("counter", "assets/a.png", "png");
    EXPECT_EQ(wisp::agent::error_code(ws->write_file("Bad Id", "main.lua", "x").error()), "bad_id");
}

TEST_F(SceneWorkspaceTest, DraftCappedAtPackageLimit) {
    begin();
    write("counter", "assets/a.bin", std::string(300 * 1024, 'a'));
    auto over = ws->write_file("counter", "assets/b.bin", std::string(300 * 1024, 'b'));
    ASSERT_FALSE(over);
    EXPECT_EQ(wisp::agent::error_code(over.error()), "quota_exceeded");
    EXPECT_FALSE(fs::exists(draft("counter") / "assets" / "b.bin"));
    // Replacing a file counts only its new size.
    write("counter", "assets/a.bin", std::string(400 * 1024, 'a'));
}

TEST_F(SceneWorkspaceTest, DeleteFileOnlyTouchesDraft) {
    install("counter", kCounter, {{"scene.json", counter_json()}, {"lib/util.lua", "return {}\n"}});
    begin(false);
    auto r = ws->delete_file("counter", "lib/util.lua");
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(draft_files("counter"), (std::vector<std::string>{"main.lua", "scene.json"}));
    EXPECT_TRUE(fs::exists(installed("counter") / "lib" / "util.lua"));
    EXPECT_EQ(ws->delete_file("counter", "lib/util.lua").error().code, ErrorCode::NotFound);
    EXPECT_EQ(ws->delete_file("ghost", "main.lua").error().code, ErrorCode::NotFound);
}

TEST_F(SceneWorkspaceTest, InstallOpensInFrontAndDropsDraft) {
    begin(false);
    write("counter", "scene.json", counter_json());
    write("counter", "main.lua", kCounter);
    ToolOutput out = ws->install("counter", ctx);
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    EXPECT_EQ(out.note, "已安装「计数器」1.0.0");
    EXPECT_EQ(rt->foreground(), "counter");
    EXPECT_EQ(rt->status("counter").state, SceneState::Running);
    EXPECT_FALSE(fs::exists(draft("counter")));
    EXPECT_FALSE(ws->has_draft("counter"));
    EXPECT_EQ(str(out.result, "version"), "1.0.0");
}

TEST_F(SceneWorkspaceTest, InstallKeepsPrevious) {
    begin();
    write("counter", "scene.json", counter_json("2.0.0"));
    ToolOutput out = ws->install("counter", ctx);
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    EXPECT_EQ(out.note, "已安装「计数器」2.0.0");
    EXPECT_TRUE(repo->has_prev("counter"));
    EXPECT_EQ(slurp(fs::path(in) / "scenes" / ".prev" / "counter" / "scene.json"), counter_json());
    EXPECT_EQ(repo->info("counter").value().manifest.version, "2.0.0");
}

TEST_F(SceneWorkspaceTest, SyntaxErrorLeavesInstalledAlone) {
    begin();
    write("counter", "scene.json", counter_json("2.0.0"));
    write("counter", "main.lua", "local a = 1\nlocal b = 2\nlocal = 3\nreturn ui.screen {}\n");
    ToolOutput out = ws->install("counter", ctx);
    ASSERT_FALSE(out.ok);
    const Value* errors = field(out.result, "errors");
    ASSERT_NE(errors, nullptr) << wisp::to_json(out.result);
    ASSERT_FALSE(errors->as_list().empty());
    EXPECT_EQ(str(errors->as_list()[0], "file"), "main.lua");
    EXPECT_EQ(errors->as_list()[0].find("line")->as_int(), 3);
    EXPECT_EQ(repo->info("counter").value().manifest.version, "1.0.0");
    EXPECT_EQ(slurp(installed("counter") / "main.lua"), kCounter);
    EXPECT_TRUE(ws->has_draft("counter"));
    EXPECT_FALSE(repo->has_prev("counter"));
}

TEST_F(SceneWorkspaceTest, BadManifestLeavesInstalledAlone) {
    begin();
    write("counter", "scene.json", R"({"id":"counter","name":"x"})");
    ToolOutput out = ws->install("counter", ctx);
    ASSERT_FALSE(out.ok);
    const Value* errors = field(out.result, "errors");
    ASSERT_NE(errors, nullptr) << wisp::to_json(out.result);
    EXPECT_EQ(str(errors->as_list()[0], "file"), "scene.json");
    // A manifest naming another id is just as bad.
    write("counter", "scene.json",
          R"({"id":"other","name":"x","version":"1.0.0","schema_version":1})");
    out = ws->install("counter", ctx);
    ASSERT_FALSE(out.ok);
    EXPECT_EQ(str(field(out.result, "errors")->as_list()[0], "file"), "scene.json");
    EXPECT_EQ(repo->info("counter").value().manifest.version, "1.0.0");
}

TEST_F(SceneWorkspaceTest, InstallRejectsUnsupportedSchema) {
    begin();
    write("counter", "scene.json",
          R"json({"id":"counter","name":"计数器","version":"2.0.0","schema_version":1,
                 "tools":[{"name":"lookup","description":"Looks up a value",
                           "parameters":{"type":"object","oneOf":[]}}]})json");

    ToolOutput out = ws->install("counter", ctx);

    ASSERT_FALSE(out.ok) << wisp::to_json(out.result);
    const Value* errors = field(out.result, "errors");
    ASSERT_NE(errors, nullptr) << wisp::to_json(out.result);
    ASSERT_FALSE(errors->as_list().empty());
    EXPECT_NE(str(errors->as_list()[0], "message").find("tools[0].parameters"), std::string::npos);
    EXPECT_EQ(repo->info("counter").value().manifest.version, "1.0.0");
}

TEST_F(SceneWorkspaceTest, InstallAndRollbackNotifyNewToolDeclarations) {
    const std::string lua =
        "wisp.tools.register('old_tool', function(args) return true end)\nreturn ui.screen {}\n";
    const std::string old_manifest =
        R"({"id":"counter","name":"计数器","version":"1.0.0","schema_version":1,)"
        R"("tools":[{"name":"old_tool","description":"The old tool"}]})";
    install("counter", lua, {{"scene.json", old_manifest}});
    begin(false);

    const std::string new_manifest =
        R"({"id":"counter","name":"计数器","version":"2.0.0","schema_version":1,)"
        R"("tools":[{"name":"new_tool","description":"The new tool"}]})";
    write("counter", "scene.json", new_manifest);
    write("counter", "main.lua",
          "wisp.tools.register('new_tool', function(args) return true end)\nreturn ui.screen {}\n");
    ASSERT_TRUE(ws->install("counter", ctx).ok);
    ASSERT_EQ(tool_changes.size(), 1u);
    ASSERT_EQ(tool_changes.back().tools.size(), 1u);
    EXPECT_EQ(tool_changes.back().tools[0].name, "new_tool");

    ASSERT_TRUE(ws->rollback("counter", ctx).ok);
    ASSERT_EQ(tool_changes.size(), 2u);
    ASSERT_EQ(tool_changes.back().tools.size(), 1u);
    EXPECT_EQ(tool_changes.back().tools[0].name, "old_tool");
}

TEST_F(SceneWorkspaceTest, TreeErrorSuggestsAName) {
    begin();
    write("counter", "main.lua", "return ui.screen {\n  ui.labl { text = 'x' },\n}\n");
    ToolOutput out = ws->install("counter", ctx);
    ASSERT_FALSE(out.ok);
    const Value& e = field(out.result, "errors")->as_list()[0];
    EXPECT_EQ(str(e, "file"), "main.lua");
    EXPECT_EQ(e.find("line")->as_int(), 2);
    EXPECT_NE(str(e, "suggestion").find("label"), std::string::npos) << wisp::to_json(e);
    EXPECT_EQ(slurp(installed("counter") / "main.lua"), kCounter);
}

TEST_F(SceneWorkspaceTest, NewHttpHostAsksFirst) {
    begin();
    write("counter", "scene.json", counter_json("2.0.0", R"(["api.example.com"])"));
    answer = false;
    ToolOutput no = ws->install("counter", ctx);
    ASSERT_FALSE(no.ok);
    EXPECT_EQ(str(no.result, "error"), "denied");
    ASSERT_EQ(confirms.size(), 1u);
    EXPECT_NE(confirms[0].find("api.example.com"), std::string::npos) << confirms[0];
    EXPECT_NE(confirms[0].find("计数器"), std::string::npos) << confirms[0];
    EXPECT_EQ(repo->info("counter").value().manifest.version, "1.0.0");
    answer = true;
    ToolOutput yes = ws->install("counter", ctx);
    ASSERT_TRUE(yes.ok) << wisp::to_json(yes.result);
    EXPECT_EQ(confirms.size(), 2u);
    EXPECT_EQ(repo->info("counter").value().manifest.version, "2.0.0");
}

TEST_F(SceneWorkspaceTest, SameHostsDoNotAsk) {
    install("counter", kCounter, {{"scene.json", counter_json("1.0.0", R"(["api.example.com"])")}});
    begin(false);
    write("counter", "scene.json", counter_json("2.0.0", R"(["api.example.com"])"));
    ToolOutput out = ws->install("counter", ctx);
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    EXPECT_TRUE(confirms.empty());
}

TEST_F(SceneWorkspaceTest, RollbackSwapsWithPrev) {
    begin();
    write("counter", "scene.json", counter_json("2.0.0"));
    ASSERT_TRUE(ws->install("counter", ctx).ok);
    ToolOutput out = ws->rollback("counter", ctx);
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    EXPECT_EQ(out.note, "已把「计数器」恢复到上一个版本");
    EXPECT_EQ(repo->info("counter").value().manifest.version, "1.0.0");
    EXPECT_EQ(slurp(fs::path(in) / "scenes" / ".prev" / "counter" / "scene.json"),
              counter_json("2.0.0"));
    EXPECT_EQ(rt->foreground(), "counter");
}

TEST_F(SceneWorkspaceTest, RollbackWithoutPrevIsNotFound) {
    begin();
    ToolOutput out = ws->rollback("counter", ctx);
    ASSERT_FALSE(out.ok);
    EXPECT_EQ(str(out.result, "error"), "not_found");
}

TEST_F(SceneWorkspaceTest, DraftsSurviveRestart) {
    begin();
    write("counter", "main.lua", "return ui.screen {}\n");
    make_ws();
    EXPECT_TRUE(ws->has_draft("counter"));
    auto r = ws->read_file("counter", "main.lua");
    ASSERT_TRUE(r);
    EXPECT_EQ(std::string(r.value().c_str()), "return ui.screen {}\n");
}

TEST_F(SceneWorkspaceTest, DevModeInstallUnsupported) {
    begin();
    wisp::core::SceneRepoConfig rc;
    rc.code_root = in + "/scenes";
    rc.data_root = in + "/scenes";
    rc.dev = true;
    rc.worker = &worker;
    wisp::core::SceneRepository dev(*storage, rc);
    make_ws(&dev);
    write("counter", "main.lua", kCounter);
    ToolOutput out = ws->install("counter", ctx);
    ASSERT_FALSE(out.ok);
    EXPECT_EQ(str(out.result, "error"), "unsupported");
    EXPECT_EQ(str(ws->rollback("counter", ctx).result, "error"), "unsupported");
    ws.reset();
}

// Every StorageManager call aborts off the flash thread while wisp.fs runs
// (WISP_ASSERT_FLASH_THREAD), so an install from another thread that
// succeeds touched flash only through the worker.
TEST_F(SceneWorkspaceTest, InstallFlashWorkStaysOnTheWorker) {
    begin();
    ASSERT_FALSE(wisp::os::on_flash_thread());
    write("counter", "scene.json", counter_json("2.0.0"));
    ToolOutput out = ws->install("counter", ctx);
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    ASSERT_TRUE(ws->rollback("counter", ctx).ok);
    EXPECT_FALSE(ws->list().as_list().empty());
}

namespace {

class ToolsTest : public SceneWorkspaceTest {
protected:
    void make_tools() {
        tools = std::make_unique<wisp::agent::ToolRegistry>(wisp::agent::ToolExecutors{
            .ui = {},
            .fs = [this](std::function<void()> fn) { return worker.submit(std::move(fn)); },
            .io = {},
        });
        ASSERT_TRUE(wisp::agent::add_scene_authoring_tools(*tools, *ws));
        ASSERT_TRUE(wisp::agent::add_ui_tools(*tools, *inspector));
    }
    ToolOutput call(const char* wire, const std::string& args) {
        return tools->call(wire, args, ctx);
    }
    void TearDown() override {
        tools.reset();
        SceneWorkspaceTest::TearDown();
    }
    std::unique_ptr<wisp::agent::ToolRegistry> tools;
};

}  // namespace

TEST_F(ToolsTest, ReadFileContentIsWrapped) {
    begin();
    make_tools();
    ToolOutput out = call("scene_read_file", R"({"id":"counter","path":"main.lua"})");
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    EXPECT_EQ(str(out.result, "content").rfind("<data source=\"scene:counter\">", 0), 0u);
    EXPECT_NE(str(out.result, "content").find("ui.button"), std::string::npos);
}

TEST_F(ToolsTest, ThreadsRisksAndTimeouts) {
    begin();
    make_tools();
    using wisp::agent::ToolRisk;
    using wisp::agent::ToolThread;
    struct Want {
        const char* name;
        ToolRisk risk;
        ToolThread thread;
        uint32_t timeout;
    };
    for (const Want& w : {Want{"scene.list", ToolRisk::Normal, ToolThread::Fs, 5000},
                          Want{"scene.list_files", ToolRisk::Normal, ToolThread::Fs, 5000},
                          Want{"scene.read_file", ToolRisk::Normal, ToolThread::Fs, 5000},
                          Want{"scene.write_file", ToolRisk::Normal, ToolThread::Fs, 5000},
                          Want{"scene.edit_file", ToolRisk::Normal, ToolThread::Fs, 5000},
                          Want{"scene.delete_file", ToolRisk::Normal, ToolThread::Fs, 5000},
                          Want{"scene.install", ToolRisk::Note, ToolThread::Agent, 30000},
                          Want{"scene.discard_draft", ToolRisk::Note, ToolThread::Fs, 5000},
                          Want{"scene.rollback", ToolRisk::Note, ToolThread::Agent, 30000},
                          Want{"ui.tree", ToolRisk::Normal, ToolThread::Agent, 5000}}) {
        const wisp::agent::ToolDef* d = tools->find(w.name);
        ASSERT_NE(d, nullptr) << w.name;
        EXPECT_EQ(d->risk, w.risk) << w.name;
        EXPECT_EQ(d->thread, w.thread) << w.name;
        EXPECT_EQ(d->timeout_ms, w.timeout) << w.name;
    }
}

TEST_F(ToolsTest, EditInstallAndDiscardThroughTheTools) {
    begin();
    make_tools();
    ToolOutput edit =
        call("scene_edit_file", R"({"id":"counter","path":"main.lua","old":"count","new":"n"})");
    ASSERT_FALSE(edit.ok);
    EXPECT_EQ(str(edit.result, "error"), "not_unique");
    EXPECT_GE(edit.result.find("count")->as_int(), 2);
    // A failed edit changes nothing, so it starts no draft either.
    EXPECT_FALSE(ws->has_draft("counter"));
    ToolOutput ok = call("scene_edit_file",
                         R"({"id":"counter","path":"main.lua","old":"\"+1\"","new":"\"one\""})");
    ASSERT_TRUE(ok.ok) << wisp::to_json(ok.result);
    EXPECT_EQ(ok.result.find("count")->as_int(), 1);
    ToolOutput disc = call("scene_discard_draft", R"({"id":"counter"})");
    ASSERT_TRUE(disc.ok) << wisp::to_json(disc.result);
    EXPECT_EQ(disc.note, "已丢弃「计数器」的草稿");
    EXPECT_EQ(str(call("scene_discard_draft", R"({"id":"counter"})").result, "error"), "not_found");
    ASSERT_TRUE(
        call(
            "scene_write_file",
            R"({"id":"counter","path":"scene.json","content":")" +
                std::string(
                    R"({\"id\":\"counter\",\"name\":\"计数器\",\"version\":\"3.0.0\",\"schema_version\":1})") +
                "\"}")
            .ok);
    ToolOutput inst = call("scene_install", R"({"id":"counter"})");
    ASSERT_TRUE(inst.ok) << wisp::to_json(inst.result);
    EXPECT_EQ(inst.note, "已安装「计数器」3.0.0");
    ToolOutput list = call("scene_list", "");
    ASSERT_TRUE(list.ok);
    EXPECT_EQ(list.result.find("scenes")->as_list().size(), 1u);
}

TEST_F(ToolsTest, UiTreeTool) {
    begin();
    make_tools();
    EXPECT_EQ(str(call("ui_tree", "").result, "error"), "no_scene");
    ASSERT_TRUE(rt->open("counter"));
    ToolOutput out;
    ASSERT_TRUE(wait_until(
        [&] {
            out = call("ui_tree", "");
            return out.ok;
        },
        2000));
    EXPECT_NE(str(out.result, "tree").find("button#inc \"+1\""), std::string::npos);
    EXPECT_EQ(str(call("ui_tree", R"({"ref":"nope"})").result, "error"), "not_found");
}

class AutoCheck : public SceneWorkspaceTest {};

TEST_F(AutoCheck, InstallReportsLuaErrorsLogsAndTree) {
    begin();
    write("counter", "main.lua",
          std::string("for i = 1, 25 do wisp.log.warn('careful ' .. i) end\n") + kCounter);
    ToolOutput out = ws->install("counter", ctx);
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    const Value* check = field(out.result, "check");
    ASSERT_NE(check, nullptr) << wisp::to_json(out.result);
    EXPECT_TRUE(check->find("lua_errors")->as_list().empty());
    const Value& logs = *check->find("logs");
    ASSERT_EQ(logs.find("W")->as_list().size(), 20u) << wisp::to_json(logs);
    EXPECT_TRUE(logs.find("E")->as_list().empty());
    const std::string& w0 = logs.find("W")->as_list()[0].as_string();
    EXPECT_EQ(w0.rfind("<data source=\"scene:counter\">", 0), 0u) << w0;
    // The last ones: careful 6..25.
    EXPECT_NE(logs.find("W")->as_list()[19].as_string().find("careful 25"), std::string::npos);
    EXPECT_NE(str(*check, "tree").find("button#inc"), std::string::npos) << str(*check, "tree");
    EXPECT_TRUE(check->find("stable")->as_bool());
}

TEST_F(AutoCheck, RollbackAlsoChecks) {
    begin();
    write("counter", "scene.json", counter_json("2.0.0"));
    ASSERT_TRUE(ws->install("counter", ctx).ok);
    ToolOutput out = ws->rollback("counter", ctx);
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    const Value* check = field(out.result, "check");
    ASSERT_NE(check, nullptr);
    EXPECT_NE(str(*check, "tree").find("button#inc"), std::string::npos);
    EXPECT_TRUE(check->find("lua_errors")->as_list().empty());
}

TEST_F(AutoCheck, SceneFaultingDuringSettleIsReported) {
    begin();
    write("counter", "main.lua",
          std::string("wisp.timer.after(100, function() error('late boom') end)\n") + kCounter);
    const auto t0 = std::chrono::steady_clock::now();
    ToolOutput out = ws->install("counter", ctx);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    EXPECT_LT(ms, 30000);
    const Value* check = field(out.result, "check");
    ASSERT_NE(check, nullptr);
    const Value& errs = *check->find("lua_errors");
    ASSERT_EQ(errs.as_list().size(), 1u) << wisp::to_json(*check);
    EXPECT_NE(errs.as_list()[0].as_string().find("late boom"), std::string::npos);
    EXPECT_EQ(str(*check, "tree").rfind("scene_error", 0), 0u) << str(*check, "tree");
    auto tree = inspector->tree();
    ASSERT_FALSE(tree);
    EXPECT_EQ(tree.error().message.rfind("scene_error", 0), 0u);
}

// An install whose open fails while another scene is in front describes
// no tree rather than the other scene's.
TEST_F(AutoCheck, FailedOpenDoesNotDescribeAnotherScene) {
    install("other", R"(return ui.screen { ui.label { ref = "elsewhere", text = "other" } })");
    begin();
    ASSERT_TRUE(rt->open("other"));
    ASSERT_TRUE(wait_until([&] { return rt->foreground() == "other"; }, 2000));
    write("counter", "scene.json", counter_json("2.0.0"));
    // Its data directory becomes unreadable, so the open after the
    // install cannot load the scene's store.
    const fs::path meta = fs::path(repo->data_dir("counter")) / "meta.json";
    fs::create_directories(meta);
    ToolOutput out = ws->install("counter", ctx);
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    EXPECT_FALSE(str(out.result, "open_error").empty()) << wisp::to_json(out.result);
    const Value* check = field(out.result, "check");
    ASSERT_NE(check, nullptr);
    EXPECT_EQ(str(*check, "tree").find("elsewhere"), std::string::npos) << str(*check, "tree");
    EXPECT_TRUE(str(*check, "tree").empty()) << str(*check, "tree");
}
