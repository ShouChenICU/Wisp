// ConversationStore (agent-core spec 3.5): current.jsonl and its archives.

#include "wisp/agent/conversation.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::ErrorCode;
using wisp::agent::ConversationStore;
using wisp::agent::Message;
using wisp::agent::Role;
using wisp::agent::Source;
using wisp::agent::ToolCallRec;

namespace fs = std::filesystem;

namespace {

std::string read_raw(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

void write_raw(const fs::path& path, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << contents;
}

std::vector<std::string> lines_of(const std::string& all) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        if (nl == std::string::npos) {
            out.push_back(all.substr(pos));
            break;
        }
        out.push_back(all.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

Message user(uint32_t turn, std::string text, Source source = Source::Web) {
    Message m;
    m.role = Role::User;
    m.turn = turn;
    m.ts = 1767225600;
    m.text = text.c_str();
    m.source = source;
    return m;
}

Message marker(uint32_t turn, const char* kind, const char* status = "") {
    Message m;
    m.role = Role::Marker;
    m.turn = turn;
    m.marker = kind;
    m.status = status;
    return m;
}

class Conversation : public ::testing::Test {
protected:
    void SetUp() override {
        root = wisp::test::temp_dir().string();
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", root, false));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        ASSERT_TRUE(storage->init());
        dir = storage->write_root(wisp::core::Area::System).value() + "/agent";
        store = std::make_unique<ConversationStore>(*storage, dir);
        ASSERT_TRUE(store->load());
    }

    // A fresh store over the same files: a reboot.
    void reboot() {
        store = std::make_unique<ConversationStore>(*storage, dir);
        ASSERT_TRUE(store->load());
    }

    fs::path current() const { return fs::path(dir) / "current.jsonl"; }

    std::string root;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::string dir;
    std::unique_ptr<ConversationStore> store;
};

}  // namespace

TEST_F(Conversation, AppendsOneJsonLinePerMessage) {
    for (uint64_t i = 1; i <= 3; ++i) {
        auto seq = store->append(user(1, "hello " + std::to_string(i)));
        ASSERT_TRUE(seq);
        EXPECT_EQ(seq.value(), i);
    }
    const std::string all = read_raw(current());
    ASSERT_FALSE(all.empty());
    EXPECT_EQ(all.back(), '\n');
    auto lines = lines_of(all);
    ASSERT_EQ(lines.size(), 3u);
    for (size_t i = 0; i < lines.size(); ++i) {
        auto v = wisp::from_json(lines[i]);
        ASSERT_TRUE(v) << lines[i];
        EXPECT_EQ(v.value().find("seq")->as_int(), static_cast<int64_t>(i + 1));
        EXPECT_EQ(v.value().find("role")->as_string(), "user");
    }
    EXPECT_EQ(store->bytes(), all.size());
}

TEST_F(Conversation, LoadRestoresEveryField) {
    ASSERT_TRUE(store->append(marker(1, "turn_begin")));
    ASSERT_TRUE(store->append(user(1, "做个计时器", Source::Voice)));
    Message a;
    a.role = Role::Assistant;
    a.turn = 1;
    a.ts = 1767225601;
    a.text = "ok";
    a.calls.push_back(ToolCallRec{"call_1", "scene.list_files", R"({"id":"timer"})"});
    a.calls.push_back(ToolCallRec{"call_2", "scene.install", "{\"id\":"});
    ASSERT_TRUE(store->append(a));
    Message t;
    t.role = Role::Tool;
    t.turn = 1;
    t.call_id = "call_1";
    t.tool = "scene.list_files";
    t.ok = false;
    t.text = R"({"ok":false,"error":"not_found"})";
    ASSERT_TRUE(store->append(t));
    Message n;
    n.role = Role::Note;
    n.turn = 1;
    n.text = "已安装 计时器";
    ASSERT_TRUE(store->append(n));
    Message s;
    s.role = Role::Summary;
    s.turn = 1;
    s.text = "earlier: a timer";
    s.covers_first = 2;
    s.covers_last = 4;
    ASSERT_TRUE(store->append(s));
    ASSERT_TRUE(store->append(marker(1, "turn_end", "done")));

    reboot();
    auto m = store->messages();
    ASSERT_EQ(m.size(), 7u);
    for (size_t i = 0; i < m.size(); ++i) EXPECT_EQ(m[i].seq, i + 1);

    EXPECT_EQ(m[0].role, Role::Marker);
    EXPECT_EQ(m[0].marker, "turn_begin");
    EXPECT_EQ(m[0].turn, 1u);

    EXPECT_EQ(m[1].role, Role::User);
    EXPECT_EQ(m[1].text, "做个计时器");
    EXPECT_EQ(m[1].source, Source::Voice);
    EXPECT_EQ(m[1].ts, 1767225600);
    const std::string line = lines_of(read_raw(current()))[1];
    EXPECT_NE(line.find(R"("source":"voice")"), std::string::npos) << line;
    EXPECT_EQ(line.find("origin"), std::string::npos) << line;

    EXPECT_EQ(m[2].role, Role::Assistant);
    EXPECT_EQ(m[2].text, "ok");
    EXPECT_EQ(m[2].ts, 1767225601);
    ASSERT_EQ(m[2].calls.size(), 2u);
    EXPECT_EQ(m[2].calls[0].id, "call_1");
    EXPECT_EQ(m[2].calls[0].name, "scene.list_files");
    EXPECT_EQ(m[2].calls[0].args, R"({"id":"timer"})");
    EXPECT_EQ(m[2].calls[1].args, "{\"id\":");

    EXPECT_EQ(m[3].role, Role::Tool);
    EXPECT_EQ(m[3].call_id, "call_1");
    EXPECT_EQ(m[3].tool, "scene.list_files");
    EXPECT_FALSE(m[3].ok);
    EXPECT_EQ(m[3].text, R"({"ok":false,"error":"not_found"})");

    EXPECT_EQ(m[4].role, Role::Note);
    EXPECT_EQ(m[4].text, "已安装 计时器");

    EXPECT_EQ(m[5].role, Role::Summary);
    EXPECT_EQ(m[5].text, "earlier: a timer");
    EXPECT_EQ(m[5].covers_first, 2u);
    EXPECT_EQ(m[5].covers_last, 4u);

    EXPECT_EQ(m[6].role, Role::Marker);
    EXPECT_EQ(m[6].marker, "turn_end");
    EXPECT_EQ(m[6].status, "done");

    EXPECT_FALSE(store->interrupted_turn().has_value());
    EXPECT_EQ(store->last_turn(), 1u);
    // Numbering carries on after a reload.
    auto next = store->append(user(2, "again"));
    ASSERT_TRUE(next);
    EXPECT_EQ(next.value(), 8u);
}

TEST_F(Conversation, SourceDefaultsToWebWhenMissing) {
    write_raw(current(), R"({"seq":1,"ts":0,"role":"user","turn":1,"text":"hi"})"
                         "\n");
    reboot();
    auto m = store->messages();
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].source, Source::Web);
    EXPECT_EQ(m[0].text, "hi");
}

TEST_F(Conversation, TornTailDroppedAndRewritten) {
    ASSERT_TRUE(store->append(user(1, "one")));
    ASSERT_TRUE(store->append(user(1, "two")));
    {
        std::ofstream f(current(), std::ios::binary | std::ios::app);
        f << R"({"seq":3,"ts":0,"role":"us)";
    }
    reboot();
    ASSERT_EQ(store->messages().size(), 2u);
    std::string all = read_raw(current());
    ASSERT_EQ(all.back(), '\n');
    EXPECT_EQ(lines_of(all).size(), 2u);
    EXPECT_EQ(store->bytes(), all.size());

    auto seq = store->append(user(1, "three"));
    ASSERT_TRUE(seq);
    EXPECT_EQ(seq.value(), 3u);
    auto lines = lines_of(read_raw(current()));
    ASSERT_EQ(lines.size(), 3u);
    for (const auto& l : lines) EXPECT_TRUE(wisp::from_json(l)) << l;
}

TEST_F(Conversation, UnfinishedTurnMarkedInterrupted) {
    ASSERT_TRUE(store->append(marker(2, "turn_begin")));
    ASSERT_TRUE(store->append(marker(2, "turn_end", "done")));
    ASSERT_TRUE(store->append(marker(3, "turn_begin")));
    ASSERT_TRUE(store->append(user(3, "half way")));
    reboot();
    ASSERT_TRUE(store->interrupted_turn().has_value());
    EXPECT_EQ(*store->interrupted_turn(), 3u);
    auto m = store->messages();
    ASSERT_EQ(m.size(), 5u);
    EXPECT_EQ(m.back().role, Role::Marker);
    EXPECT_EQ(m.back().marker, "turn_end");
    EXPECT_EQ(m.back().status, "interrupted");
    EXPECT_EQ(m.back().turn, 3u);
    EXPECT_EQ(lines_of(read_raw(current())).size(), 5u);

    // Once marked, the next boot finds nothing unfinished.
    reboot();
    EXPECT_FALSE(store->interrupted_turn().has_value());
    EXPECT_EQ(store->messages().size(), 5u);
}

TEST_F(Conversation, ArchiveRotatesKeepingFive) {
    for (int i = 1; i <= 7; ++i) {
        ASSERT_TRUE(store->append(user(1, "conversation " + std::to_string(i))));
        ASSERT_TRUE(store->archive());
    }
    auto list = store->archives();
    ASSERT_TRUE(list);
    std::vector<uint32_t> ns;
    for (const auto& a : list.value()) ns.push_back(a.n);
    EXPECT_EQ(ns, (std::vector<uint32_t>{7, 6, 5, 4, 3}));
    EXPECT_FALSE(fs::exists(fs::path(dir) / "archive-1.jsonl"));
    EXPECT_FALSE(fs::exists(fs::path(dir) / "archive-2.jsonl"));
    EXPECT_TRUE(fs::exists(fs::path(dir) / "archive-7.jsonl"));
    EXPECT_TRUE(store->messages().empty());
    EXPECT_EQ(store->bytes(), 0u);
    EXPECT_EQ(read_raw(current()), "");

    auto seven = store->read_archive(7);
    ASSERT_TRUE(seven);
    ASSERT_EQ(seven.value().size(), 1u);
    EXPECT_EQ(seven.value()[0].text, "conversation 7");

    // Numbering survives a reboot.
    reboot();
    ASSERT_TRUE(store->append(user(1, "eight")));
    ASSERT_TRUE(store->archive());
    EXPECT_EQ(store->archives().value().front().n, 8u);
}

TEST_F(Conversation, ArchiveListShowsFirstUserMessage) {
    ASSERT_TRUE(store->append(marker(1, "turn_begin")));
    std::string long_text;
    for (int i = 0; i < 150; ++i) long_text += "汉";
    ASSERT_TRUE(store->append(user(1, long_text)));
    ASSERT_TRUE(store->append(user(2, "second")));
    ASSERT_TRUE(store->archive());
    auto list = store->archives();
    ASSERT_TRUE(list);
    ASSERT_EQ(list.value().size(), 1u);
    std::string expected;
    for (int i = 0; i < 100; ++i) expected += "汉";
    EXPECT_EQ(list.value()[0].first, expected);
    EXPECT_EQ(list.value()[0].n, 1u);
}

TEST_F(Conversation, ReadArchiveUnknownIsNotFound) {
    auto r = store->read_archive(42);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST_F(Conversation, ClearAllRemovesEverything) {
    ASSERT_TRUE(store->append(user(1, "a")));
    ASSERT_TRUE(store->archive());
    ASSERT_TRUE(store->append(user(1, "b")));
    ASSERT_TRUE(store->clear_all());
    EXPECT_TRUE(store->messages().empty());
    EXPECT_EQ(store->bytes(), 0u);
    auto list = store->archives();
    ASSERT_TRUE(list);
    EXPECT_TRUE(list.value().empty());
    EXPECT_FALSE(fs::exists(current()));
    EXPECT_FALSE(fs::exists(fs::path(dir) / "archive-1.jsonl"));

    // Still usable afterwards.
    auto seq = store->append(user(1, "c"));
    ASSERT_TRUE(seq);
    EXPECT_EQ(seq.value(), 1u);
}

TEST_F(Conversation, LongResultsKeptInFull) {
    std::string big = R"({"ok":true,"content":")";
    while (big.size() < 65536 - 2) big += static_cast<char>('a' + big.size() % 26);
    big += "\"}";
    ASSERT_EQ(big.size(), 65536u);
    Message t;
    t.role = Role::Tool;
    t.turn = 1;
    t.call_id = "c";
    t.tool = "scene.read_file";
    t.text = big.c_str();
    ASSERT_TRUE(store->append(t));
    reboot();
    auto m = store->messages();
    ASSERT_EQ(m.size(), 1u);
    ASSERT_EQ(m[0].text.size(), big.size());
    EXPECT_TRUE(std::string_view(m[0].text.data(), m[0].text.size()) == big);
}

TEST_F(Conversation, RolloverPastOneMiB) {
    std::string chunk(65536, 'x');
    Message t;
    t.role = Role::Tool;
    t.turn = 1;
    t.call_id = "c";
    t.tool = "scene.read_file";
    t.text = chunk.c_str();
    while (store->bytes() <= wisp::agent::kRolloverBytes) {
        EXPECT_FALSE(store->needs_rollover());
        ASSERT_TRUE(store->append(t));
    }
    EXPECT_GT(store->bytes(), 1048576u);
    EXPECT_TRUE(store->needs_rollover());
    reboot();
    EXPECT_TRUE(store->needs_rollover());
}

TEST_F(Conversation, FindBySeq) {
    ASSERT_TRUE(store->append(user(1, "a")));
    ASSERT_TRUE(store->append(user(1, "b")));
    auto m = store->find(2);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(m->text, "b");
    EXPECT_FALSE(store->find(3).has_value());
}

TEST_F(Conversation, ToValueCutsToolResults) {
    Message t;
    t.seq = 9;
    t.role = Role::Tool;
    t.turn = 2;
    t.call_id = "c";
    t.tool = "scene.read_file";
    t.text = std::string(500, 'r').c_str();
    wisp::Value v = wisp::agent::to_value(t, 200);
    EXPECT_EQ(v.find("role")->as_string(), "tool");
    EXPECT_EQ(v.find("name")->as_string(), "scene.read_file");
    EXPECT_EQ(v.find("result")->as_string().size(), 200u);
    EXPECT_EQ(v.find("result_len")->as_int(), 500);

    wisp::Value u = wisp::agent::to_value(user(1, "hi", Source::DeviceText), 200);
    EXPECT_EQ(u.find("source")->as_string(), "device_text");
    EXPECT_EQ(u.find("text")->as_string(), "hi");
}
