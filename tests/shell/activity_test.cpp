// The agent's steps as the screen names them (wisp/shell/activity.hpp,
// mvp spec 6.2): the friendly label, its one short target, and the
// summary line.

#include "wisp/shell/activity.hpp"

#include <gtest/gtest.h>

#include <string>

#include "wisp/ui/strings.hpp"

using wisp::shell::activity_label;
using wisp::shell::activity_prep_label;
using wisp::shell::activity_summary;
using wisp::shell::activity_target;
using wisp::ui::Lang;

namespace {

std::string s(const wisp::os::LargeString& v) { return std::string(v.data(), v.size()); }

}  // namespace

TEST(Activity, KnownToolsGetTheirLabelAndTarget) {
    EXPECT_EQ(
        s(activity_label("scene.write_file",
                         R"({"id":"clock","path":"main.lua","content":"local x = 1"})", Lang::Zh)),
        "写入文件 main.lua");
    EXPECT_EQ(s(activity_label("scene.install", R"({"id":"clock"})", Lang::Zh)), "安装场景 clock");
    EXPECT_EQ(s(activity_label("scene.files_read", R"({"id":"clock","name":"a.txt"})", Lang::En)),
              "Read stored file a.txt");
    EXPECT_EQ(s(activity_label("todo.add", R"({"title":"买牛奶","due":"2026-11-08"})", Lang::Zh)),
              "添加待办 买牛奶");
    EXPECT_EQ(s(activity_label("scene.list", "{}", Lang::Ja)), "シーン一覧を確認");
    // A number works as a target too.
    EXPECT_EQ(s(activity_label("todo.done", R"({"id":12})", Lang::Zh)), "完成待办 12");
    // No target in the arguments: the label alone.
    EXPECT_EQ(s(activity_label("scene.write_file", "", Lang::Zh)), "写入文件");
}

TEST(Activity, EveryLanguageHasALabelForEveryTool) {
    for (const char* tool :
         {"scene.list",       "scene.list_files",  "scene.read_file",  "scene.write_file",
          "scene.edit_file",  "scene.delete_file", "scene.install",    "scene.discard_draft",
          "scene.rollback",   "scene.open",        "scene.close",      "scene.exit",
          "scene.delete",     "scene.eval",        "scene.logs",       "scene.data_read",
          "scene.data_write", "scene.files_list",  "scene.files_read", "scene.files_remove",
          "memory.add",       "memory.list",       "memory.update",    "memory.remove",
          "todo.add",         "todo.list",         "todo.update",      "todo.done",
          "todo.remove",      "settings.get",      "settings.set",     "device.status",
          "notify.list",      "notify.dismiss",    "docs.read",        "ui.tree"}) {
        for (Lang lang : wisp::ui::kAllLangs) {
            const std::string label = s(activity_label(tool, "", lang));
            EXPECT_FALSE(label.empty()) << tool;
            EXPECT_EQ(label.find(tool), std::string::npos) << tool << ": " << label;
        }
    }
}

TEST(Activity, ACutSummaryStillYieldsThePath) {
    // The summary is the arguments' first 200 code points: never valid
    // JSON when the content is long.
    const std::string cut =
        R"({"id":"clock","path":"ui/main.lua","content":"local function draw()\n  wisp.ui.label({text=\"hi)";
    EXPECT_EQ(s(activity_target(cut)), "ui/main.lua");
    EXPECT_EQ(s(activity_label("scene.edit_file", cut, Lang::Zh)), "修改文件 ui/main.lua");
    // Cut inside the path itself: what there is of it.
    EXPECT_EQ(s(activity_target(R"({"id":"clock","path":"ui/ma)")), "ui/ma");
    // Cut before any target: none.
    EXPECT_EQ(s(activity_target(R"({"id)")), "");
}

TEST(Activity, TargetComesFromKeysNotFromValues) {
    // "path" inside the content is text, not a key.
    const std::string args =
        R"({"id":"clock","content":"x = {\"path\":\"secret\"}","path":"main.lua"})";
    EXPECT_EQ(s(activity_target(args)), "main.lua");
    // path before name before title before id.
    EXPECT_EQ(s(activity_target(R"({"id":"a","title":"t","name":"n"})")), "n");
    EXPECT_EQ(s(activity_target(R"({"id":"a","title":"t"})")), "t");
    // Escapes are read, control characters dropped, a long target cut.
    EXPECT_EQ(s(activity_target(R"({"path":"a\"b\nc"})")), "a\"bc");
    const std::string long_name(200, 'x');
    EXPECT_LE(activity_target(R"({"path":")" + long_name + R"("})").size(),
              wisp::shell::kActivityTargetChars);
}

TEST(Activity, UnknownToolsFallBack) {
    EXPECT_EQ(s(activity_label("weather.get", R"({"city":"x"})", Lang::Zh)), "调用 weather.get");
    EXPECT_EQ(s(activity_label("weather.get", "", Lang::En)), "Call weather.get");
}

TEST(Activity, SettingsLabelsNeverShowAValue) {
    for (Lang lang : wisp::ui::kAllLangs) {
        for (const char* args :
             {R"({"key":"language","value":"sk-very-secret"})",
              R"({"key":"brightness","value":"sk-very-secret","name":"sk-very-secret"})",
              R"({"id":"sk-very-secret","value":"sk-very-secret"})"}) {
            const std::string set = s(activity_label("settings.set", args, lang));
            const std::string get = s(activity_label("settings.get", args, lang));
            EXPECT_EQ(set.find("sk-very"), std::string::npos) << set;
            EXPECT_EQ(get.find("sk-very"), std::string::npos) << get;
        }
    }
    // Memory text is never shown either.
    EXPECT_EQ(s(activity_label("memory.add", R"({"text":"my pin is 1234"})", Lang::En)),
              "Remember something");
}

TEST(Activity, PrepLabelCarriesTheSize) {
    EXPECT_EQ(s(activity_prep_label("scene.write_file", 0, Lang::Zh)), "写入文件");
    EXPECT_EQ(s(activity_prep_label("scene.write_file", 512, Lang::Zh)), "写入文件 · 512 B");
    EXPECT_EQ(s(activity_prep_label("scene.write_file", 3277, Lang::Zh)), "写入文件 · 3.2 KB");
    EXPECT_EQ(s(activity_prep_label("scene.write_file", 2 * 1024 * 1024 + 100, Lang::En)),
              "Write file · 2.0 MB");
}

TEST(Activity, SummaryCountsStepsAndTime) {
    EXPECT_EQ(s(activity_summary(3, 42'400, true, Lang::Zh)), "第 3 步 · 42 秒");
    EXPECT_EQ(s(activity_summary(5, 72'000, false, Lang::Zh)), "用了 5 步 · 1 分 12 秒");
    EXPECT_EQ(s(activity_summary(1, 500, true, Lang::En)), "Step 1 · 0 s");
    EXPECT_EQ(s(activity_summary(2, 61'000, false, Lang::En)), "Steps: 2 · 1 min 1 s");
}
