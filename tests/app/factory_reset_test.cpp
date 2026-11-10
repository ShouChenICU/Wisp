// Factory reset (connectivity spec 6.1): what it erases and keeps, the order
// of its steps, and the on-device confirmation the admin route asks for.

#include "factory_reset.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "app/ui_net_harness.hpp"
#include "support/fake_http.hpp"
#include "support/lvgl_probe.hpp"
#include "support/wait.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/memory_store.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/reminders.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/flash_guard.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/shell/scene_recents.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/theme.hpp"

using wisp::net::Method;
using wisp::test::all_label_glyphs_resolvable;
using wisp::test::count_obj_of_class;
using wisp::test::find_button_with_label;
using wisp::test::find_label_text_containing;
using wisp::test::find_obj_of_class;
using wisp::test::make_req;
using wisp::test::run_on_ui_blocking;
using wisp::test::run_on_ui_thread;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;
using wisp::test::wait_until;
using wisp::test::with_csrf;
using wisp::test::with_token;
using wisp::ui::Lang;
using wisp::ui::StrId;

namespace {

// Pairs a browser through the service, as the code flow would.
std::string pair_browser(UiNetHarness& h) {
    wisp::net::PairingService* pairing = h.app->pairing();
    EXPECT_TRUE(pairing->request_code(wisp::os::now_ms()));
    auto code = pairing->active_code(wisp::os::now_ms()).value_or("");
    auto token = pairing->confirm(code, wisp::net::ClientInfo{}, wisp::os::now_ms());
    EXPECT_TRUE(token);
    return token ? token.value() : std::string();
}

int msgboxes(UiNetHarness& h) {
    return std::stoi(run_on_ui_thread(h.app->ui(), [] {
        return std::to_string(count_obj_of_class(lv_layer_top(), &lv_msgbox_class));
    }));
}

bool dialog_says(UiNetHarness& h, const std::string& text) {
    return !run_on_ui_thread(h.app->ui(), [&] {
                return find_label_text_containing(lv_layer_top(), text.c_str());
            }).empty();
}

bool click(UiNetHarness& h, const std::string& text) {
    return run_on_ui_thread(h.app->ui(), [&] {
               lv_obj_t* btn = find_button_with_label(lv_layer_top(), text.c_str());
               if (btn == nullptr) return "";
               lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
               return "clicked";
           }) == "clicked";
}

wisp::net::HttpResponse post_reset(UiNetHarness& h, const std::string& token) {
    return h.server->handle(
        with_csrf(with_token(make_req(Method::Post, "/api/v1/system/factory-reset"), token)));
}

std::vector<std::string> reset_states(UiNetHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.topic("system.factory_reset")) {
        out.push_back(e.payload.at("state").as_string());
    }
    return out;
}

// Runs `fn` on the app's storage worker -- the flash thread while the app
// runs -- and waits for it.
void on_flash(wisp::app::App& app, const std::function<void()>& fn) {
    ASSERT_TRUE(app.storage_worker()->run_sync(fn, wisp::os::kForever));
}

wisp::Result<std::vector<uint8_t>> read_on_flash(wisp::app::App& app, const std::string& path) {
    auto out = wisp::Result<std::vector<uint8_t>>::err(wisp::ErrorCode::Busy, "not run");
    on_flash(app, [&] { out = app.storage().read_file(path); });
    return out;
}

wisp::Result<void> write_on_flash(wisp::app::App& app, const std::string& path,
                                  std::span<const uint8_t> data) {
    auto out = wisp::Result<void>::err(wisp::ErrorCode::Busy, "not run");
    on_flash(app, [&] { out = app.storage().write_atomic(path, data); });
    return out;
}

}  // namespace

TEST(FactoryReset, ClearsEverythingButLibraryAndReboots) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;

    ASSERT_TRUE(app.system_settings().set({{"language", "en"}}));
    ASSERT_TRUE(wisp::core::Settings(*app.platform().settings, "power").set("screen_off_s", 30));
    ASSERT_EQ(app.wifi()->saved_ssids().size(), 1u);
    pair_browser(h);
    ASSERT_EQ(app.pairing()->clients().size(), 1u);
    ASSERT_TRUE(app.models().create({.kind = wisp::core::ModelKind::Llm,
                                     .name = "Chat",
                                     .preset = "openai",
                                     .model = "m",
                                     .api_key = "sk-test"}));
    const std::string system = app.storage().write_root(wisp::core::Area::System).value();
    // The profile is saved on the storage worker.
    ASSERT_TRUE(app.storage_worker()->drain(2000));
    ASSERT_TRUE(std::filesystem::exists(system + "/secrets/models.json"));
    const std::string library =
        app.storage().write_root(wisp::core::Area::Library).value() + "/scene.json";
    const std::string kept = R"({"keep":true})";
    ASSERT_TRUE(write_on_flash(
        app, library,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(kept.data()), kept.size())));

    wisp::app::factory_reset(app);

    EXPECT_EQ(h.reboots.load(), 1);
    EXPECT_EQ(reset_states(h), std::vector<std::string>{"done"});
    // Settings: every key gone, on disk too.
    on_flash(app, [&] {
        wisp::port::sim::JsonFileSettingsBackend fresh(h.settings_path());
        EXPECT_EQ(wisp::core::Settings(fresh, "sys").get_string("lang", "none"), "none");
        EXPECT_EQ(wisp::core::Settings(fresh, "power").get_int("screen_off_s", -1), -1);
    });
    // Wi-Fi, pairings and the model profiles, in memory and on disk.
    EXPECT_TRUE(app.wifi()->saved_ssids().empty());
    auto wifi_json = read_on_flash(app, system + "/net/wifi.json");
    ASSERT_TRUE(wifi_json);
    EXPECT_EQ(std::string(wifi_json.value().begin(), wifi_json.value().end()).find("Home"),
              std::string::npos);
    EXPECT_TRUE(app.pairing()->clients().empty());
    auto clients_json = read_on_flash(app, system + "/net/clients.json");
    if (clients_json) {
        EXPECT_EQ(
            std::string(clients_json.value().begin(), clients_json.value().end()).find("hash"),
            std::string::npos);
    }
    EXPECT_TRUE(app.models().get().profiles.empty());
    EXPECT_FALSE(app.models().active_for_request(wisp::core::ModelKind::Llm).has_value());
    EXPECT_FALSE(std::filesystem::exists(system + "/secrets/models.json"));
    // Library data stays.
    auto scene = read_on_flash(app, library);
    ASSERT_TRUE(scene);
    EXPECT_EQ(std::string(scene.value().begin(), scene.value().end()), kept);
}

// The agent's conversations and archives live under <System>/agent/.
TEST(FactoryReset, ClearsAgentDirectory) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;
    const std::string agent = app.storage().write_root(wisp::core::Area::System).value() + "/agent";
    const std::string line = "{\"seq\":1}\n";
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(line.data()),
                                         line.size());
    ASSERT_TRUE(write_on_flash(app, agent + "/current.jsonl", bytes));
    ASSERT_TRUE(write_on_flash(app, agent + "/archive-1.jsonl", bytes));

    wisp::app::factory_reset(app);

    EXPECT_FALSE(std::filesystem::exists(agent));
}

// lua.run's files live under <Library>/agent/ (agent-tools spec 4.2): the
// internal library's and the card's.
TEST(FactoryReset, ClearsAgentFiles) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;
    const std::vector<std::string> roots = app.storage().roots(wisp::core::Area::Library);
    ASSERT_FALSE(roots.empty());
    const std::string text = "x";
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(text.data()),
                                         text.size());
    for (const std::string& root : roots)
        ASSERT_TRUE(write_on_flash(app, root + "/agent/files/a.txt", bytes));
    const std::string kept = roots.back() + "/scenes/x.json";
    ASSERT_TRUE(write_on_flash(app, kept, bytes));

    wisp::app::factory_reset(app);

    for (const std::string& root : roots) EXPECT_FALSE(std::filesystem::exists(root + "/agent"));
    EXPECT_TRUE(std::filesystem::exists(kept));
}

// The agent's memory notes live under <System>/memory/ (mvp spec 5.1).
TEST(FactoryReset, ClearsMemoryDirectory) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;
    const std::string memory =
        app.storage().write_root(wisp::core::Area::System).value() + "/memory";
    const std::string body = R"({"version":1,"next_id":2,"notes":[{"id":1,"text":"x",)"
                             R"("updated":0}]})";
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(body.data()),
                                         body.size());
    ASSERT_TRUE(write_on_flash(app, memory + "/notes.json", bytes));

    wisp::app::factory_reset(app);

    EXPECT_FALSE(std::filesystem::exists(memory));
}

// The same race ReminderScheduler::forget() closes (reminders.hpp): forget()
// must run before the restart so that a memory.add call still in flight
// (here, the tool's own underlying call) cannot write the erased directory
// back.
TEST(FactoryReset, MemoryToolCannotRecreateNotesAfterReset) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;
    wisp::core::MemoryStore* memory = app.memory();
    ASSERT_NE(memory, nullptr);
    const std::string dir = app.storage().write_root(wisp::core::Area::System).value() + "/memory";
    on_flash(app, [&] { ASSERT_TRUE(memory->add("取药单号8848")); });
    ASSERT_TRUE(std::filesystem::exists(dir));

    wisp::app::factory_reset(app);

    EXPECT_FALSE(std::filesystem::exists(dir));
    // What the memory.add tool would run next, were a call still queued.
    on_flash(app, [&] { EXPECT_FALSE(memory->add("again")); });
    EXPECT_FALSE(std::filesystem::exists(dir));
}

TEST(FactoryReset, ClearsTodoDirectory) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;
    const std::string todo = app.storage().write_root(wisp::core::Area::System).value() + "/todo";
    const std::string body = R"({"version":1,"next_id":2,"todos":[{"id":1,"title":"x",)"
                             R"("done":false,"created":0,"repeat":"none"}]})";
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(body.data()),
                                         body.size());
    ASSERT_TRUE(write_on_flash(app, todo + "/todos.json", bytes));

    wisp::app::factory_reset(app);

    EXPECT_FALSE(std::filesystem::exists(todo));
}

// The scene list's order lives under <System>/shell/ (mvp spec 6.4); a
// scene opened just before the reset does not write it back.
TEST(FactoryReset, ClearsShellDirectory) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;
    const std::string shell = app.storage().write_root(wisp::core::Area::System).value() + "/shell";
    const std::string body = R"({"version":1,"recent":{"todo":1}})";
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(body.data()),
                                         body.size());
    ASSERT_TRUE(write_on_flash(app, shell + "/recents.json", bytes));
    app.bus().publish({"scene.state", "test", 0, {{"id", "clock"}, {"state", "running"}}});
    ASSERT_TRUE(app.bus().wait_idle(1000));

    wisp::app::factory_reset(app);

    EXPECT_FALSE(std::filesystem::exists(shell));
    wisp::os::sleep_ms(wisp::shell::SceneRecents::kSaveDelayMs + 300);
    on_flash(app, [] {});
    EXPECT_FALSE(std::filesystem::exists(shell));
}

// The todos live in memory too, and their titles in the reminder notices:
// after the reset neither the todo file nor the notifications file holds
// them, whatever the scheduler is asked to do before the restart (a
// timer pass, a todo tool, a reminder action).
TEST(FactoryReset, ErasesTodosAndTheirNoticesForGood) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;
    wisp::core::ReminderScheduler* reminders = app.reminders();
    wisp::core::NotificationService* notes = app.notifications();
    ASSERT_NE(reminders, nullptr);
    ASSERT_NE(notes, nullptr);
    const std::string system = app.storage().write_root(wisp::core::Area::System).value();
    const std::string todo = system + "/todo";
    const std::string notices = system + "/notifications.json";
    const std::string secret = "取药单号8848";

    on_flash(app, [&] {
        ASSERT_TRUE(reminders->add(secret, std::time(nullptr) + 3600,
                                   wisp::core::RepeatRule{wisp::core::Repeat::Daily, 0}));
    });
    wisp::core::Notification n;
    n.source = wisp::core::kReminderSource;
    n.id = "todo-1";
    n.title = secret;
    n.action = wisp::core::notice::kActionTodoDone;
    n.action_arg = "1";
    ASSERT_TRUE(notes->post(std::move(n), false));
    ASSERT_TRUE(notes->flush(5000));
    auto file_text = [&](const std::string& path) {
        auto r = read_on_flash(app, path);
        return r ? std::string(r.value().begin(), r.value().end()) : std::string();
    };
    ASSERT_NE(file_text(notices).find(secret), std::string::npos);
    ASSERT_TRUE(std::filesystem::exists(todo));

    wisp::app::factory_reset(app);

    EXPECT_FALSE(std::filesystem::exists(todo));
    EXPECT_EQ(notes->unread_count(), 0u);
    // What a todo tool (add), a reminder action (the router queues done()
    // or snooze() on the worker) and a timer pass would run.
    on_flash(app, [&] {
        EXPECT_FALSE(reminders->add("again", std::nullopt, {}));
        EXPECT_FALSE(reminders->done(1));
        EXPECT_FALSE(reminders->snooze(1));
        reminders->fire_due();
    });
    app.bus().publish(
        wisp::core::Event{"system.time", "test", 0, {{"state", wisp::Value("synced")}}});
    (void)app.bus().wait_idle(2000);
    (void)notes->flush(5000);
    ASSERT_TRUE(app.storage_worker()->drain(5000));
    EXPECT_FALSE(std::filesystem::exists(todo));
    EXPECT_EQ(file_text(notices).find(secret), std::string::npos);
    EXPECT_TRUE(reminders->list(wisp::core::TodoFilter::All).empty());
}

// The browser that asked must still hear "done": the event goes out (and
// the admin socket gets a chance to send it) before the tokens -- and
// with them that socket -- are revoked.
TEST(FactoryReset, AnnouncesDoneBeforeRevokingTokens) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    pair_browser(h);
    std::atomic<bool> done_seen_at_revoke{false};
    std::atomic<int> revokes{0};
    int listener = h.app->pairing()->add_revoke_listener([&](const auto& id) {
        if (id) return;
        ++revokes;
        std::lock_guard<std::mutex> lock(h.events_mutex);
        for (const auto& e : h.events) {
            if (e.topic == "system.factory_reset") done_seen_at_revoke.store(true);
        }
    });

    wisp::app::factory_reset(*h.app);
    h.app->pairing()->remove_revoke_listener(listener);
    EXPECT_EQ(revokes.load(), 1);
    EXPECT_TRUE(done_seen_at_revoke.load());
    EXPECT_EQ(h.reboots.load(), 1);
}

TEST(FactoryReset, CancelPublishesCancelled) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    auto res = post_reset(h, token);
    EXPECT_EQ(res.status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
    EXPECT_TRUE(dialog_says(h, "将清除 Wi-Fi、配对和模型配置并重启，场景数据保留。"));

    ASSERT_TRUE(click(h, "否"));
    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested", "cancelled"}));
    EXPECT_EQ(msgboxes(h), 0);
    wisp::os::sleep_ms(100);
    EXPECT_EQ(h.reboots.load(), 0);
    EXPECT_EQ(h.app->wifi()->saved_ssids().size(), 1u);
    EXPECT_EQ(h.app->pairing()->clients().size(), 1u);
}

TEST(FactoryReset, ConfirmOnDeviceResetsOffTheUiThread) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
    ASSERT_TRUE(click(h, "是"));
    ASSERT_TRUE(wait_until([&] { return h.reboots.load() == 1; }, 5000));
    EXPECT_FALSE(h.rebooted_on_ui_thread.load());
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested", "done"}));
    EXPECT_TRUE(h.app->wifi()->saved_ssids().empty());
    EXPECT_TRUE(h.app->pairing()->clients().empty());
    EXPECT_EQ(msgboxes(h), 0);
    // The revoked token no longer works.
    EXPECT_EQ(post_reset(h, token).status, 401);
}

TEST(FactoryReset, RepeatedRequestsShowOneDialog) {
    UiNetOptions opts;
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
    h.app->bus().wait_idle(2000);
    run_on_ui_blocking(h.app->ui(), [] {});
    EXPECT_EQ(msgboxes(h), 1);

    ASSERT_TRUE(click(h, "否"));
    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 3; }, 3000));
    EXPECT_EQ(reset_states(h).back(), "cancelled");
    EXPECT_EQ(msgboxes(h), 0);
}

// If the reset task cannot start, nothing is erased, the browser hears
// "cancelled", and the next request asks again.
TEST(FactoryReset, FailedTaskStartCancelsAndAllowsAnotherRequest) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);
    std::atomic<int> spawns{0};
    wisp::app::set_reset_spawner(
        *h.app,
        [&](const wisp::os::TaskConfig&, std::function<void()>) -> std::unique_ptr<wisp::os::Task> {
            spawns.fetch_add(1);
            return nullptr;
        });

    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
    ASSERT_TRUE(click(h, "是"));
    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested", "cancelled"}));
    EXPECT_EQ(spawns.load(), 1);
    wisp::os::sleep_ms(100);
    EXPECT_EQ(h.reboots.load(), 0);
    EXPECT_EQ(h.app->wifi()->saved_ssids().size(), 1u);
    EXPECT_EQ(h.app->pairing()->clients().size(), 1u);

    // Not stuck in "confirmed": a new request shows the dialog again, and
    // with the task able to start this time the reset goes through.
    wisp::app::set_reset_spawner(*h.app, nullptr);
    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return msgboxes(h) == 1; }, 3000));
    ASSERT_TRUE(click(h, "是"));
    ASSERT_TRUE(wait_until([&] { return h.reboots.load() == 1; }, 5000));
    EXPECT_EQ(reset_states(h),
              (std::vector<std::string>{"requested", "cancelled", "requested", "done"}));
}

// One confirm dialog at a time: with the power-off question up, a reset
// request is answered "cancelled" and no second dialog appears.
TEST(FactoryReset, RequestDuringPowerOffDialogIsCancelled) {
    UiNetOptions opts;
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    h.app->bus().publish(wisp::core::Event{"button.power.long", "button.power"});
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "关机"); }, 3000));
    ASSERT_EQ(msgboxes(h), 1);

    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested", "cancelled"}));
    run_on_ui_blocking(h.app->ui(), [] {});
    EXPECT_EQ(msgboxes(h), 1);
    EXPECT_FALSE(dialog_says(h, "恢复出厂设置"));
    EXPECT_TRUE(dialog_says(h, "关机"));

    // Once the power-off question is answered, a reset can be asked.
    ASSERT_TRUE(click(h, "否"));
    ASSERT_TRUE(wait_until([&] { return msgboxes(h) == 0; }, 3000));
    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
}

// The other order: with the reset question up, a long press on the power
// key is ignored, and the key's next short press turns the screen off as
// it would with no dialog of its own pending.
TEST(FactoryReset, PowerKeyDuringResetDialogIsIgnored) {
    UiNetOptions opts;
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));

    h.app->bus().publish(wisp::core::Event{"button.power.long", "button.power"});
    h.app->bus().wait_idle(2000);
    run_on_ui_blocking(h.app->ui(), [] {});
    EXPECT_EQ(msgboxes(h), 1);
    EXPECT_TRUE(dialog_says(h, "恢复出厂设置"));
    EXPECT_FALSE(dialog_says(h, "关机"));

    wisp::app::PowerManager* pm = h.app->power_manager();
    ASSERT_NE(pm, nullptr);
    h.app->bus().publish(wisp::core::Event{"button.power.short", "button.power"});
    ASSERT_TRUE(wait_until([&] { return pm->standby(); }, 3000));
    // The reset question is still waiting for an answer.
    EXPECT_EQ(msgboxes(h), 1);
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested"}));
}

// A request made before the screen exists (its event published before
// anyone listens) is shown once the screen is built, exactly once.
TEST(FactoryReset, RequestBeforeTheScreenExistsIsShown) {
    UiNetHarness h;
    wisp::app::request_factory_reset(*h.app);
    ASSERT_TRUE(h.start("system."));
    // Already up when start() returns (the screen's build took it), not
    // only after the next resync.
    EXPECT_TRUE(dialog_says(h, "恢复出厂设置"));
    // Past the event and at least one resync: still one dialog.
    h.app->bus().wait_idle(2000);
    wisp::os::sleep_ms(1200);
    EXPECT_EQ(msgboxes(h), 1);
}

// The bus drops its oldest events when full: a request whose event was
// lost still shows the dialog through the screen's periodic resync.
TEST(FactoryReset, RequestWithALostEventIsShown) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    wisp::os::Semaphore entered(1, 0);
    wisp::os::Semaphore release(1, 0);
    auto block_sub = h.app->bus().subscribe("test.block", [&](const wisp::core::Event&) {
        entered.give();
        release.take(wisp::os::kForever);
    });
    h.app->bus().publish(wisp::core::Event{"test.block", "test"});
    ASSERT_TRUE(entered.take(3000));

    wisp::app::request_factory_reset(*h.app);
    for (int i = 0; i < 256; ++i) {
        h.app->bus().publish(wisp::core::Event{"test.flood", "test"});
    }
    bool shown = wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000);
    release.give();
    h.app->bus().unsubscribe(block_sub);
    EXPECT_TRUE(shown);
    h.app->bus().wait_idle(2000);
    run_on_ui_blocking(h.app->ui(), [] {});
    EXPECT_EQ(msgboxes(h), 1);
}

// When the resync shows the dialog first and the event arrives late --
// after the user already said "no" -- the event finds the request taken:
// the dialog does not come back.
TEST(FactoryReset, LateEventDoesNotReopenTheDialog) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    wisp::os::Semaphore entered(1, 0);
    wisp::os::Semaphore release(1, 0);
    auto block_sub = h.app->bus().subscribe("test.block", [&](const wisp::core::Event&) {
        entered.give();
        release.take(wisp::os::kForever);
    });
    h.app->bus().publish(wisp::core::Event{"test.block", "test"});
    ASSERT_TRUE(entered.take(3000));

    wisp::app::request_factory_reset(*h.app);  // event held back, not dropped
    bool shown = wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000);
    bool clicked = shown && click(h, "否");
    release.give();
    h.app->bus().unsubscribe(block_sub);
    ASSERT_TRUE(shown);
    ASSERT_TRUE(clicked);

    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested", "cancelled"}));
    h.app->bus().wait_idle(2000);
    run_on_ui_blocking(h.app->ui(), [] {});
    wisp::os::sleep_ms(1200);  // and one more resync
    EXPECT_EQ(msgboxes(h), 0);
}

// Without a screen nobody could confirm, so the route says so up front.
TEST(FactoryReset, HeadlessDeviceRefusesTheRequest) {
    UiNetOptions opts;
    opts.with_display = false;
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);
    EXPECT_EQ(post_reset(h, token).status, 503);
}

// The request wakes a sleeping screen so the dialog is seen; the dialog
// fits the safe area and follows a language change while open.
TEST(FactoryReset, DialogFitsEveryShapeAndLanguage) {
    for (const char* shape : {"rounded_rect", "circle"}) {
        SCOPED_TRACE(shape);
        UiNetOptions opts;
        opts.shape = shape;
        opts.with_server = true;
        UiNetHarness h(opts);
        ASSERT_TRUE(h.start("system."));
        const std::string token = pair_browser(h);
        wisp::app::PowerManager* pm = h.app->power_manager();
        run_on_ui_blocking(h.app->ui(), [&] { pm->enter_standby(); });

        EXPECT_EQ(post_reset(h, token).status, 202);
        ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
        EXPECT_FALSE(pm->standby());

        auto shape_caps = wisp::hal::screen_shape_from_caps(h.display->info().caps);
        ASSERT_TRUE(shape_caps);
        const wisp::hal::Area safe = wisp::hal::safe_rect(shape_caps.value());
        for (Lang lang : {Lang::Zh, Lang::Ja, Lang::En}) {
            SCOPED_TRACE(wisp::ui::lang_code(lang));
            ASSERT_TRUE(h.app->system_settings().set({{"language", wisp::ui::lang_code(lang)}}));
            const std::string title = wisp::ui::tr(StrId::ResetTitle, lang);
            ASSERT_TRUE(wait_until([&] { return dialog_says(h, title); }, 3000));
            EXPECT_EQ(msgboxes(h), 1);
            std::string problem = run_on_ui_thread(h.app->ui(), [&] {
                lv_obj_update_layout(lv_layer_top());
                lv_obj_t* box = find_obj_of_class(lv_layer_top(), &lv_msgbox_class);
                lv_area_t a;
                lv_obj_get_coords(box, &a);
                if (a.x1 < safe.x1 || a.y1 < safe.y1 || a.x2 > safe.x2 || a.y2 > safe.y2) {
                    return "dialog " + std::to_string(a.x1) + "," + std::to_string(a.y1) + "-" +
                           std::to_string(a.x2) + "," + std::to_string(a.y2) +
                           " leaves the safe area";
                }
                return all_label_glyphs_resolvable(lv_layer_top()) ? std::string()
                                                                   : std::string("missing glyph");
            });
            EXPECT_EQ(problem, "");
        }
        // Erasing the device is a danger button; keeping it, a secondary one.
        EXPECT_EQ(run_on_ui_thread(
                      h.app->ui(),
                      [] {
                          auto fill = [](const char* text) {
                              lv_obj_t* btn = find_button_with_label(lv_layer_top(), text);
                              if (btn == nullptr) return uint32_t{0};
                              return lv_color_to_u32(lv_obj_get_style_bg_color(btn, LV_PART_MAIN)) &
                                     0xFFFFFFu;
                          };
                          const bool yes =
                              fill(wisp::ui::tr(StrId::Yes, Lang::En)) == wisp::ui::theme::kErr;
                          const bool no =
                              fill(wisp::ui::tr(StrId::No, Lang::En)) == wisp::ui::theme::kSurface3;
                          return std::string(yes ? "danger" : "?") + "|" + (no ? "secondary" : "?");
                      }),
                  "danger|secondary");
        ASSERT_TRUE(click(h, wisp::ui::tr(StrId::No, Lang::En)));
        ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
        EXPECT_EQ(h.reboots.load(), 0);
    }
}

TEST(FactoryReset, ErasesOnTheFlashThread) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;
    pair_browser(h);
    ASSERT_NE(app.storage_worker(), nullptr);

    // Revoking every browser is one step of the erase; its listeners run
    // on the thread doing it.
    std::atomic<int> revokes{0};
    std::atomic<int> off_worker{0};
    wisp::app::App* papp = &app;
    const int handle = app.pairing()->add_revoke_listener([&, papp](const auto&) {
        revokes.fetch_add(1);
        if (!papp->storage_worker()->on_worker() || !wisp::os::on_flash_thread()) {
            off_worker.fetch_add(1);
        }
    });

    wisp::app::factory_reset(app);
    app.pairing()->remove_revoke_listener(handle);

    EXPECT_EQ(h.reboots.load(), 1);
    EXPECT_EQ(revokes.load(), 1);
    EXPECT_EQ(off_worker.load(), 0);
    EXPECT_TRUE(app.wifi()->saved_ssids().empty());
    EXPECT_TRUE(app.pairing()->clients().empty());
}

TEST(FactoryReset, NeverRestartsWithoutErasing) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;
    wisp::core::StorageWorker* worker = app.storage_worker();
    ASSERT_NE(worker, nullptr);

    // The worker is stuck in a job and its queue is full.
    auto entered = std::make_shared<wisp::os::Semaphore>(1, 0);
    auto release = std::make_shared<wisp::os::Semaphore>(1, 0);
    ASSERT_TRUE(worker->submit([entered, release] {
        entered->give();
        release->take(wisp::os::kForever);
    }));
    ASSERT_TRUE(entered->take(2000));
    while (worker->submit([] {})) {
    }

    wisp::app::set_reset_erase_budget(app, 300);
    wisp::app::factory_reset(app);
    EXPECT_EQ(h.reboots.load(), 0);
    EXPECT_EQ(reset_states(h), std::vector<std::string>{"failed"});
    EXPECT_EQ(app.wifi()->saved_ssids().size(), 1u);

    // Once the queue moves again, a new request goes through.
    release->give();
    ASSERT_TRUE(worker->drain(5000));
    wisp::app::factory_reset(app);
    EXPECT_EQ(h.reboots.load(), 1);
    EXPECT_TRUE(app.wifi()->saved_ssids().empty());
}
