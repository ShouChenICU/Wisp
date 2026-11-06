// wisp.files and wisp.http.download on a running SceneRuntime over a
// SceneFiles, a SceneHttp and a fake client (scene-device spec 7.1, 7.2).

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_http.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/net/scene_http.hpp"

namespace {

using wisp::test::wait_until;
namespace fs = std::filesystem;

const char* kScreen = "return ui.screen { ui.label { text = \"x\" } }\n";

std::string manifest_with_hosts(const std::string& id) {
    return R"({"id":")" + id +
           R"(","name":"Demo","version":"1.0.0","schema_version":1,)"
           R"("permissions":{"http":["api.example.com"]}})";
}

bool on_disk(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

std::string read_raw(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

class FilesBinding : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        http = std::make_unique<wisp::net::SceneHttp>(client);
        ASSERT_TRUE(http->start());
        wisp::core::SceneFilesConfig fc;
        fc.data_root = in + "/scenes";
        fc.max_list = 3;  // small enough for a test to go past it
        fc.submit_io = [this](std::function<void()> job) { return worker.submit(std::move(job)); };
        files = std::make_unique<wisp::core::SceneFiles>(*storage, fc);
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.http = http.get();
            d.files = files.get();
        };
    }

    void TearDown() override {
        client.hold = false;
        release = true;
        rt.reset();
        http.reset();
        SceneRuntimeFixture::TearDown();
        files.reset();
    }

    void install_scene(const std::string& id, const std::string& lua) {
        install(id, lua + kScreen, {{"scene.json", manifest_with_hosts(id)}});
    }

    // What the scene printed, without the proxy's "http: ..." lines.
    std::vector<std::string> printed(const std::string& id) {
        std::vector<std::string> out;
        for (auto& line : scene_log(id))
            if (line.rfind("http: ", 0) != 0) out.push_back(std::move(line));
        return out;
    }

    std::vector<std::string> wait_log(const std::string& id, size_t n) {
        EXPECT_TRUE(wait_until([&] { return printed(id).size() >= n; }, 5000))
            << "logged: " << ::testing::PrintToString(scene_log(id));
        return printed(id);
    }

    fs::path files_dir(const std::string& id) const {
        return fs::path(in) / "scenes" / id / "files";
    }

    wisp::test::FakeHttpClient client;
    std::unique_ptr<wisp::net::SceneHttp> http;
    std::unique_ptr<wisp::core::SceneFiles> files;
    std::atomic<bool> release{false};
};

TEST_F(FilesBinding, CallbacksCarryResults) {
    start();
    install_scene("h", R"(
wisp.files.info(function(err, info) print("fresh", err, info.location, info.used) end)
wisp.files.write("a.txt", "hello", function(err, ok)
  print("write", err, ok)
  wisp.files.list(function(err, list, truncated)
    print("list", err, #list, list[1].name, list[1].size, math.type(list[1].mtime), list[1].mtime > 0,
          truncated)
    wisp.files.read("a.txt", 1, 3, function(err, data)
      print("read", err, data)
      wisp.files.append("a.txt", "!", function(err, ok)
        print("append", err, ok)
        wisp.files.info(function(err, info)
          print("info", err, info.location, info.quota, info.used)
          wisp.files.remove("a.txt", function(err, ok) print("remove", err, ok) end)
        end)
      end)
    end)
  end)
end)
)");
    ASSERT_TRUE(rt->open("h"));
    auto lines = wait_log("h", 7);
    EXPECT_EQ(lines, (std::vector<std::string>{"fresh\tnil\tnil\t0", "write\tnil\ttrue",
                                               "list\tnil\t1\ta.txt\t5\tinteger\ttrue\tfalse",
                                               "read\tnil\tell", "append\tnil\ttrue",
                                               "info\tnil\tint\t524288\t6", "remove\tnil\ttrue"}));
    EXPECT_FALSE(on_disk(files_dir("h") / "a.txt"));
}

// Past max_list the list is cut, and the callback says so.
TEST_F(FilesBinding, ListSaysWhenCut) {
    start();
    install_scene("h", R"(
local names = {"d", "b", "a", "c"}
local function step(i)
  if i > #names then
    wisp.files.list(function(err, list, truncated)
      print("list", err, #list, list[1].name, list[3].name, truncated)
    end)
    return
  end
  wisp.files.write(names[i], "x", function() step(i + 1) end)
end
step(1)
)");
    ASSERT_TRUE(rt->open("h"));
    EXPECT_EQ(wait_log("h", 1), (std::vector<std::string>{"list\tnil\t3\ta\tc\ttrue"}));
}

TEST_F(FilesBinding, BadNameIsReportedAsync) {
    start();
    install_scene("h", R"(
local function show(tag) return function(err, res) print(tag, err, res) end end
wisp.files.write(".x", "d", show("write"))
wisp.files.read("a/b", 0, 10, show("read"))
wisp.files.remove("", show("remove"))
wisp.http.download("https://api.example.com/f", "bad name", nil, show("download"))
print("after")
print(pcall(wisp.files.write, 5, "d", function() end))
print(pcall(wisp.files.write, "a", {}, function() end))
print(pcall(wisp.files.read, "a", 0, 1.5, function() end))
print(pcall(wisp.files.list, 5))
)");
    ASSERT_TRUE(rt->open("h"));
    auto lines = wait_log("h", 9);
    ASSERT_GE(lines.size(), 9u);
    EXPECT_EQ(lines[0], "after");
    for (size_t i = 1; i < 5; ++i) EXPECT_EQ(lines[i].rfind("false\t", 0), 0u) << lines[i];
    std::vector<std::string> answers(lines.begin() + 5, lines.end());
    EXPECT_EQ(answers,
              (std::vector<std::string>{"write\tbad_name\tnil", "read\tbad_name\tnil",
                                        "remove\tbad_name\tnil", "download\tbad_name\tnil"}));
    EXPECT_EQ(client.calls(), 0u);
}

TEST_F(FilesBinding, DownloadWritesFileWithinQuota) {
    start();
    const std::string body(300 * 1024, 'f');
    client.next = {.status = 200, .body = body};
    install_scene("h", R"(
wisp.http.download("https://api.example.com/f", "f.bin", nil, function(err, res)
  print("dl", err, res.status, res.size, res.deferred)
  wisp.files.info(function(err, info) print("info", info.used) end)
end)
)");
    ASSERT_TRUE(rt->open("h"));
    EXPECT_EQ(wait_log("h", 2),
              (std::vector<std::string>{"dl\tnil\t200\t307200\tfalse", "info\t307200"}));
    EXPECT_EQ(read_raw(files_dir("h") / "f.bin"), body);
    EXPECT_FALSE(on_disk(files_dir("h") / ".f.bin.dl"));
    ASSERT_TRUE(client.last());
    EXPECT_EQ(client.last()->timeout_ms, wisp::net::SceneHttpRequest::kDownloadTimeoutMs);
}

TEST_F(FilesBinding, DownloadQuotaExceededLeavesNoFile) {
    start();
    client.next = {.status = 200, .body = std::string(600 * 1024, 'q')};
    install_scene("h", R"(
wisp.http.download("https://api.example.com/f", "f.bin", function(err, res) print("dl", err, res) end)
)");
    ASSERT_TRUE(rt->open("h"));
    EXPECT_EQ(wait_log("h", 1), (std::vector<std::string>{"dl\tquota_exceeded\tnil"}));
    EXPECT_FALSE(on_disk(files_dir("h") / "f.bin"));
    EXPECT_FALSE(on_disk(files_dir("h") / ".f.bin.dl"));
    EXPECT_EQ(files->info("h").value().used, 0u);
}

TEST_F(FilesBinding, DownloadNon2xxWritesNothing) {
    start();
    client.next = {.status = 404, .body = "nope"};
    install_scene("h", R"(
wisp.files.write("f.bin", "old", function(err)
  wisp.http.download("https://api.example.com/f", "f.bin", nil, function(err, res)
    print("dl", err, res.status, res.size)
  end)
end)
)");
    ASSERT_TRUE(rt->open("h"));
    EXPECT_EQ(wait_log("h", 1), (std::vector<std::string>{"dl\tnil\t404\t0"}));
    EXPECT_EQ(read_raw(files_dir("h") / "f.bin"), "old");
    EXPECT_FALSE(on_disk(files_dir("h") / ".f.bin.dl"));
    EXPECT_EQ(files->info("h").value().used, 3u);
}

// An error page larger than the quota is dropped, not stored: no
// quota_exceeded, and the status comes back with size 0.
TEST_F(FilesBinding, DownloadLarge404IsDropped) {
    start();
    client.next = {.status = 404, .body = std::string(600 * 1024, 'e')};
    install_scene("h", R"(
wisp.http.download("https://api.example.com/f", "f.bin", nil, function(err, res)
  print("dl", err, res.status, res.size)
end)
)");
    ASSERT_TRUE(rt->open("h"));
    EXPECT_EQ(wait_log("h", 1), (std::vector<std::string>{"dl\tnil\t404\t0"}));
    EXPECT_FALSE(on_disk(files_dir("h") / "f.bin"));
    EXPECT_FALSE(on_disk(files_dir("h") / ".f.bin.dl"));
    EXPECT_EQ(files->info("h").value().used, 0u);
}

// A URL the scene may not fetch is refused before anything is written,
// so it does not fix where the scene's files go.
TEST_F(FilesBinding, RefusedDownloadPicksNoLocation) {
    start();
    install_scene("h", R"(
wisp.http.download("https://other.example.com/f", "f.bin", nil, function(err, res)
  print("dl", err, res)
  wisp.files.info(function(err, info) print("info", err, info.location, info.want) end)
end)
)");
    ASSERT_TRUE(rt->open("h"));
    EXPECT_EQ(wait_log("h", 2),
              (std::vector<std::string>{"dl\tpermission_denied\tnil", "info\tnil\tnil\tauto"}));
    EXPECT_FALSE(on_disk(fs::path(in) / "scenes" / "h" / "data" / "files_loc.json"));
    EXPECT_EQ(client.calls(), 0u);
}

TEST_F(FilesBinding, DownloadDeferredInDeepStandby) {
    start();
    client.next = {.status = 200, .body = "later"};
    http->set_deferring(true);
    install_scene("h", R"(
wisp.http.download("https://api.example.com/f", "f.bin", nil, function(err, res)
  print("dl", err, res.status, res.size, res.deferred)
end)
)");
    ASSERT_TRUE(rt->open("h"));
    ASSERT_TRUE(wait_until([&] { return http->deferred_count() == 1; }, 3000));
    wisp::os::sleep_ms(50);
    EXPECT_EQ(client.calls(), 0u);
    EXPECT_TRUE(printed("h").empty());
    http->set_deferring(false);
    EXPECT_EQ(wait_log("h", 1), (std::vector<std::string>{"dl\tnil\t200\t5\ttrue"}));
    EXPECT_EQ(read_raw(files_dir("h") / "f.bin"), "later");
}

TEST_F(FilesBinding, ExitDuringDownloadRemovesPartial) {
    start();
    client.next = {.status = 200, .body = std::string(256 * 1024, 'p')};
    // The server stalls after a few pieces until the test lets it go on.
    client.before_piece = [this] {
        if (client.pieces.load() < 12) return;
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    install_scene("h", R"(
wisp.http.download("https://api.example.com/f", "f.bin", nil, function(err, res) print("late", err) end)
)");
    ASSERT_TRUE(rt->open("h"));
    ASSERT_TRUE(wait_until([&] { return client.pieces.load() >= 12; }, 3000));
    ASSERT_TRUE(wait_until([&] { return on_disk(files_dir("h") / ".f.bin.dl"); }, 3000));
    ASSERT_TRUE(rt->exit("h"));
    release = true;
    EXPECT_TRUE(wait_until([&] { return !on_disk(files_dir("h") / ".f.bin.dl"); }, 3000));
    EXPECT_TRUE(wait_until([&] { return http->idle(); }, 3000));
    ASSERT_TRUE(worker.drain(3000));
    EXPECT_FALSE(on_disk(files_dir("h") / ".f.bin.dl"));
    EXPECT_FALSE(on_disk(files_dir("h") / "f.bin"));
    EXPECT_TRUE(printed("h").empty());
    EXPECT_EQ(files->info("h").value().used, 0u);
}

// wisp.files on the SD card, pulled while a write is on its way: the write
// was issued (it waits on the storage worker), then the card goes. The
// scene gets an error, never a crash; once the card is reported gone every
// call says "unavailable"; once it is back nothing half-written is left and
// the usage matches the disk.
class FilesBindingSd : public FilesBinding {
protected:
    void SetUp() override {
        FilesBinding::SetUp();
        files.reset();
        sd_dir = wisp::test::temp_dir().string();
        sd_dev = std::make_shared<wisp::test::FakeStorage>("storage.sd", sd_dir, true);
        registry.add(sd_dev);
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        ASSERT_TRUE(storage->init());
        wisp::core::SceneRepoConfig rc;
        rc.code_root = in + "/scenes";
        rc.data_root = in + "/scenes";
        repo = std::make_unique<wisp::core::SceneRepository>(*storage, rc);
        wisp::core::SceneFilesConfig fc;
        fc.data_root = in + "/scenes";
        fc.declared = [](std::string_view) {
            return wisp::Result<wisp::core::FilesWant>::ok(wisp::core::FilesWant::Sd);
        };
        fc.submit_io = [this](std::function<void()> job) { return worker.submit(std::move(job)); };
        files = std::make_unique<wisp::core::SceneFiles>(*storage, fc);
    }

    fs::path sd_files(const std::string& id) const {
        return fs::path(sd_dir) / "wisp" / "library" / "scenes" / id / "files";
    }

    // The card leaves mid-operation: its files are gone at once and its
    // mount point takes no new ones (a plain file stands in for the dead
    // mount, so nothing can be created under it); the storage notices on
    // its next refresh.
    void pull_card() {
        sd_dev->online_flag = false;
        fs::rename(sd_dir, sd_dir + ".pulled");
        std::ofstream(sd_dir) << "no card";
    }
    void report_card() {
        storage->refresh();
        files->on_sd_changed();
    }
    void insert_card() {
        fs::remove(sd_dir);
        fs::rename(sd_dir + ".pulled", sd_dir);
        sd_dev->online_flag = true;
        report_card();
    }

    // Holds the worker inside a job until the returned flag goes false.
    std::shared_ptr<std::atomic<bool>> hold_worker() {
        auto hold = std::make_shared<std::atomic<bool>>(true);
        auto holding = std::make_shared<std::atomic<bool>>(false);
        EXPECT_TRUE(worker.submit([hold, holding] {
            *holding = true;
            while (hold->load()) wisp::os::sleep_ms(1);
        }));
        EXPECT_TRUE(wait_until([&] { return holding->load(); }, 2000));
        return hold;
    }

    std::string sd_dir;
    std::shared_ptr<wisp::test::FakeStorage> sd_dev;
};

TEST_F(FilesBindingSd, CardPulledDuringAWriteOrAppend) {
    start();
    install("h", R"(
wisp.files.write("keep.txt", "kept", function(err, ok) print("first", err, ok) end)
return ui.screen {
  ui.button { ref = "w", text = "w", on_click = function()
    wisp.files.write("big.txt", string.rep("x", 4096), function(err, ok)
      print("write", err ~= nil, ok)
    end)
    wisp.files.append("keep.txt", "+more", function(err, ok)
      print("append", err ~= nil, ok)
    end)
  end },
  ui.button { ref = "again", text = "again", on_click = function()
    wisp.files.write("c.txt", "c", function(err, ok) print("again", err, ok) end)
  end },
}
)",
            {{"scene.json", manifest_with_hosts("h")}});
    ASSERT_TRUE(rt->open("h"));
    ASSERT_EQ(wait_log("h", 1), (std::vector<std::string>{"first\tnil\ttrue"}));
    ASSERT_EQ(read_raw(sd_files("h") / "keep.txt"), "kept");

    auto hold = hold_worker();
    ASSERT_TRUE(wait_label("again"));
    const size_t before = worker.pending();
    ASSERT_TRUE(rt->click_ref("w"));
    // Both the write and the append are really queued behind the held
    // job before the card goes, not just clicked: the click message still
    // has to reach the scene task before it runs the Lua handler that
    // submits them.
    ASSERT_TRUE(wait_until([&] { return worker.pending() >= before + 2; }, 2000));
    pull_card();
    *hold = false;
    auto lines = wait_log("h", 3);
    EXPECT_EQ(lines, (std::vector<std::string>{"first\tnil\ttrue", "write\ttrue\tnil",
                                               "append\ttrue\tnil"}));

    // Reported gone: every call is "unavailable".
    report_card();
    ASSERT_TRUE(rt->click_ref("again"));
    lines = wait_log("h", 4);
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[3], "again\tunavailable\tnil");

    // Back: nothing half-written, the old file as it was, the usage right.
    insert_card();
    EXPECT_FALSE(on_disk(sd_files("h") / "big.txt"));
    EXPECT_EQ(read_raw(sd_files("h") / "keep.txt"), "kept");
    for (const auto& e : fs::directory_iterator(sd_files("h")))
        EXPECT_EQ(e.path().filename().string()[0] == '.', false) << e.path();
    auto info = files->info("h");
    ASSERT_TRUE(info) << info.error().message;
    EXPECT_EQ(info.value().used, 4u);
    ASSERT_TRUE(rt->click_ref("again"));
    lines = wait_log("h", 5);
    ASSERT_EQ(lines.size(), 5u);
    EXPECT_EQ(lines[4], "again\tnil\ttrue");
    EXPECT_EQ(files->info("h").value().used, 5u);
}

}  // namespace
