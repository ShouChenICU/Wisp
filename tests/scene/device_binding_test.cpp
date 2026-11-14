// wisp.device.list/get/call (device model spec 4.3): scenes reach the
// devices that describe themselves through core::DeviceService, only those
// their scene.json declares; the agent's own lua.run reaches every one.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/device_service.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace {

using wisp::Map;
using wisp::Value;
using wisp::test::FakeGeneric;
using wisp::test::kFakeGenericSchema;
using wisp::test::wait_until;

std::string declared_manifest(const std::string& id, const std::string& devices) {
    return R"({"id":")" + id +
           R"(","name":"Demo","version":"1.0.0","schema_version":1,"permissions":{"device":)" +
           devices + "}}";
}

size_t count_of(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + needle.size()))
        ++n;
    return n;
}

class DeviceBinding : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        haptic = std::make_shared<FakeGeneric>("haptic.main", &kFakeGenericSchema);
        ASSERT_TRUE(registry.add(haptic));
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            if (with_service) d.devices = &service;
        };
    }

    void TearDown() override {
        rt.reset();
        SceneRuntimeFixture::TearDown();
    }

    // Opens scene `id`, which runs `lua` at load time, declaring `devices`
    // (a JSON array) or, with "", nothing.
    void open_scene(const std::string& lua, const std::string& devices = R"(["haptic.main"])",
                    const std::string& id = "demo") {
        wisp::test::Files extra;
        if (!devices.empty()) extra.push_back({"scene.json", declared_manifest(id, devices)});
        install(id, lua + "\nprint('done')\nreturn ui.screen {}\n", std::move(extra));
        ASSERT_TRUE(rt->open(id));
        ASSERT_TRUE(wait_until(
            [&] {
                auto log = scene_log(id);
                return std::find(log.begin(), log.end(), "done") != log.end();
            },
            3000))
            << ::testing::PrintToString(scene_log(id));
    }

    // The fixed clock: a second call comes "at once" after the first.
    std::atomic<uint64_t> t{1000};
    bool with_service = true;
    wisp::core::DeviceService service{registry, nullptr, [this] { return t.load(); }};
    std::shared_ptr<FakeGeneric> haptic;
};

}  // namespace

TEST_F(DeviceBinding, DeclaredSceneCallsTheDevice) {
    start();
    open_scene(R"(print(wisp.device.call("haptic.main", "pulse", {ms = 150})))");
    EXPECT_EQ(scene_log("demo")[0], "true");
    auto calls = haptic->invocations();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].first, "pulse");
    // The integer stays an integer, and the service fills the default.
    EXPECT_EQ(calls[0].second.at("ms"), Value(150));
    EXPECT_EQ(calls[0].second.at("strength"), Value(80));
}

TEST_F(DeviceBinding, ResultsComeBackAsLuaValues) {
    haptic->on_invoke = [](std::string_view, const Map& args) {
        return wisp::Result<Value>::ok(Value(Map{{"echo", args.at("reg")}, {"ok", true}}));
    };
    start();
    // A float with an integral value is an Int argument (validate_args());
    // the result is a table; an empty argument table is the same as none.
    open_scene(R"(
local r = wisp.device.call("haptic.main", "read_raw", {reg = 7.0})
print(math.type(r.echo), r.echo, r.ok)
print(wisp.device.call("haptic.main", "set_mode", {}))
)");
    auto log = scene_log("demo");
    ASSERT_GE(log.size(), 2u);
    EXPECT_EQ(log[0], "integer\t7\ttrue");
    EXPECT_EQ(log[1], "nil\tinvalid_argument");  // mode is required
}

TEST_F(DeviceBinding, UndeclaredSceneIsDenied) {
    start();
    open_scene(R"(
print(wisp.device.call("haptic.main", "pulse", {ms = 150}))
print(wisp.device.call("nothing.here", "pulse", {ms = 150}))
)",
               "");
    auto log = scene_log("demo");
    ASSERT_GE(log.size(), 2u);
    EXPECT_EQ(log[0], "nil\tpermission_denied");
    // An id that does not exist is not told apart from one not granted.
    EXPECT_EQ(log[1], "nil\tpermission_denied");
    EXPECT_TRUE(haptic->invocations().empty());
    // No rate slot was taken: a grant-holder can call at once.
    ASSERT_TRUE(service.call("haptic.main", "pulse", Value(Map{{"ms", 150}}), "agent"));
}

TEST_F(DeviceBinding, ErrorsAreSnakeCaseCodes) {
    start();
    open_scene(R"(
print(wisp.device.call("haptic.main", "pulse", {ms = 5}))
print(wisp.device.call("haptic.main", "spin", nil))
print(wisp.device.call("haptic.main", "pulse", {ms = 150}))
print(wisp.device.call("haptic.main", "pulse", {ms = 150}))
print(wisp.device.call("haptic.main", "pulse", {150}))
print(wisp.device.call("haptic.main", "pulse", {ms = 150, [1] = 2}))
)");
    auto log = scene_log("demo");
    ASSERT_GE(log.size(), 6u);
    EXPECT_EQ(log[0], "nil\tinvalid_argument");
    EXPECT_EQ(log[1], "nil\tnot_found");
    EXPECT_EQ(log[2], "true");
    EXPECT_EQ(log[3], "nil\trate_limited");
    EXPECT_EQ(log[4], "nil\tinvalid_argument");  // an array is not an object
    EXPECT_EQ(log[5], "nil\tinvalid_argument");  // mixed keys cannot be one either
    EXPECT_EQ(haptic->invocations().size(), 1u);
}

TEST_F(DeviceBinding, CyclicNonFiniteOrHugeArgsAreInvalid) {
    start();
    open_scene(R"(
local t = {}
t.self = t
print(wisp.device.call("haptic.main", "pulse", t))
print(wisp.device.call("haptic.main", "pulse", {ms = 0/0}))
print(wisp.device.call("haptic.main", "pulse", {ms = math.huge}))
print(wisp.device.call("haptic.main", "pulse", {ms = -math.huge}))
print(wisp.device.call("haptic.main", "pulse", {ms = 150, pad = string.rep("x", 100000)}))
)");
    auto log = scene_log("demo");
    ASSERT_GE(log.size(), 5u);
    for (size_t i = 0; i < 5; ++i) EXPECT_EQ(log[i], "nil\tinvalid_argument") << i;
    EXPECT_TRUE(haptic->invocations().empty());
}

// Eight string arguments at hal::kMaxStringParamBytes, with 32-byte names
// and bytes that JSON writes as six characters each: the worst a schema
// allows still fits the binding's argument cap.
TEST_F(DeviceBinding, LargestArgumentsTheSchemaAllowsGetThrough) {
    static const std::string kNames[8] = {std::string(31, 'a') + "1", std::string(31, 'a') + "2",
                                          std::string(31, 'a') + "3", std::string(31, 'a') + "4",
                                          std::string(31, 'a') + "5", std::string(31, 'a') + "6",
                                          std::string(31, 'a') + "7", std::string(31, 'a') + "8"};
    static const wisp::hal::ParamSpec kArgs[8] = {
        {.name = kNames[0].c_str(), .type = wisp::hal::ParamType::String, .max_len = 256},
        {.name = kNames[1].c_str(), .type = wisp::hal::ParamType::String, .max_len = 256},
        {.name = kNames[2].c_str(), .type = wisp::hal::ParamType::String, .max_len = 256},
        {.name = kNames[3].c_str(), .type = wisp::hal::ParamType::String, .max_len = 256},
        {.name = kNames[4].c_str(), .type = wisp::hal::ParamType::String, .max_len = 256},
        {.name = kNames[5].c_str(), .type = wisp::hal::ParamType::String, .max_len = 256},
        {.name = kNames[6].c_str(), .type = wisp::hal::ParamType::String, .max_len = 256},
        {.name = kNames[7].c_str(), .type = wisp::hal::ParamType::String, .max_len = 256}};
    static const wisp::hal::CmdSpec kCmds[] = {{.name = "say", .desc = "", .args = kArgs}};
    static const wisp::hal::DeviceSchema kSchema{.props = {}, .cmds = kCmds};
    static_assert(wisp::hal::kMaxStringParamBytes == 256);
    auto text = std::make_shared<FakeGeneric>("text.main", &kSchema);
    ASSERT_TRUE(registry.add(text));
    start();
    std::string lua = "local s = string.rep('\\1', 256)\nlocal a = {}\n";
    for (const std::string& n : kNames) lua += "a." + n + " = s\n";
    lua +=
        "print(wisp.device.call('text.main', 'say', a))\n"
        "a." +
        kNames[0] +
        " = s .. 'x'\n"
        "print(wisp.device.call('text.main', 'say', a))\n";
    open_scene(lua, R"(["text.main"])");
    auto log = scene_log("demo");
    ASSERT_GE(log.size(), 2u);
    EXPECT_EQ(log[0], "true");
    EXPECT_EQ(log[1], "nil\tinvalid_argument");  // 257 bytes: the schema refuses it
    auto calls = text->invocations();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].second.size(), 8u);
    EXPECT_EQ(calls[0].second.at(kNames[7]), Value(std::string(256, '\1')));
}

TEST_F(DeviceBinding, DriverErrorsAndBusyPassThrough) {
    haptic->on_invoke = [](std::string_view, const Map&) {
        return wisp::Result<Value>::err(wisp::ErrorCode::DeviceFailure, "bus error");
    };
    auto light = std::make_shared<FakeGeneric>("light.main", &kFakeGenericSchema,
                                               wisp::Properties{}, std::string{}, true);
    ASSERT_TRUE(registry.add(light));
    service.set_power_level(wisp::hal::PowerLevel::Standby);
    start();
    open_scene(R"(
print(wisp.device.call("haptic.main", "pulse", {ms = 150}))
print(wisp.device.call("light.main", "pulse", {ms = 150}))
)",
               R"(["haptic.main","light.main"])");
    auto log = scene_log("demo");
    ASSERT_GE(log.size(), 2u);
    EXPECT_EQ(log[0], "nil\tdevice_failure");
    EXPECT_EQ(log[1], "nil\tbusy");  // "standby: off" while the screen is off
    EXPECT_TRUE(light->invocations().empty());
}

TEST_F(DeviceBinding, GetNeedsThePermissionToo) {
    haptic->on_get = [](std::string_view) { return wisp::Result<Value>::ok(Value(42)); };
    start();
    open_scene(R"(
print(wisp.device.get("haptic.main", "level"))
print(wisp.device.get("haptic.main", "nope"))
)");
    auto log = scene_log("demo");
    ASSERT_GE(log.size(), 2u);
    EXPECT_EQ(log[0], "42");
    EXPECT_EQ(log[1], "nil\tnot_found");

    open_scene(R"(print(wisp.device.get("haptic.main", "level")))", "", "plain");
    EXPECT_EQ(scene_log("plain")[0], "nil\tpermission_denied");
    // Only the granted read of a real property reached the driver.
    EXPECT_EQ(haptic->gets().size(), 1u);
}

TEST_F(DeviceBinding, ListNeedsNoPermission) {
    start();
    open_scene(R"(
local l = wisp.device.list()
print(#l, l[1].id, l[1].class, l[1].model, l[1].schema.cmds[1].name)
)",
               "");
    EXPECT_EQ(scene_log("demo")[0], "1\thaptic.main\tother\tFAKE\tpulse");
}

TEST_F(DeviceBinding, ListStopsAt32) {
    for (int i = 0; i < 40; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "other.g%02d", i);
        ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>(id, &kFakeGenericSchema)));
    }
    start();
    open_scene(R"(
local l = wisp.device.list()
print(#l, l[1].id, l[32].id)
)");
    // Sorted by id, so haptic.main comes first.
    EXPECT_EQ(scene_log("demo")[0], "32\thaptic.main\tother.g30");
}

TEST_F(DeviceBinding, HolderIsTheScene) {
    start();
    open_scene(R"(wisp.device.call("haptic.main", "pulse", {ms = 150}))");
    const std::string ring = wisp::log::ring_snapshot();
    EXPECT_EQ(count_of(ring, "device haptic.main.pulse by scene.demo"), 1u) << ring;
}

TEST_F(DeviceBinding, NonTableArgsRaise) {
    start();
    install("demo",
            "print(pcall(wisp.device.call, 'haptic.main', 'pulse', 5))\n"
            "print(pcall(wisp.device.call, 'haptic.main'))\n"
            "print(pcall(wisp.device.get, 'haptic.main'))\n"
            "print('done')\nreturn ui.screen {}\n",
            {{"scene.json", declared_manifest("demo", R"(["haptic.main"])")}});
    ASSERT_TRUE(rt->open("demo"));
    ASSERT_TRUE(wait_until([&] { return scene_log("demo").size() >= 4; }, 3000));
    auto log = scene_log("demo");
    EXPECT_NE(log[0].find("bad argument #3"), std::string::npos) << log[0];
    EXPECT_NE(log[1].find("bad argument #2"), std::string::npos) << log[1];
    EXPECT_NE(log[2].find("bad argument #2"), std::string::npos) << log[2];
    EXPECT_TRUE(haptic->invocations().empty());
}

TEST_F(DeviceBinding, NoServiceIsUnavailable) {
    with_service = false;
    start();
    open_scene(R"(
print(wisp.device.call("haptic.main", "pulse", {ms = 150}))
print(wisp.device.get("haptic.main", "level"))
print(type(wisp.device.list()), next(wisp.device.list()))
)");
    auto log = scene_log("demo");
    ASSERT_GE(log.size(), 3u);
    EXPECT_EQ(log[0], "nil\tunavailable");
    EXPECT_EQ(log[1], "nil\tunavailable");
    EXPECT_EQ(log[2], "table\tnil");
    EXPECT_TRUE(haptic->invocations().empty());
}

// A scene in the background calls devices too.
TEST_F(DeviceBinding, BackgroundSceneCallsTheDevice) {
    start();
    install("demo",
            "print(wisp.scene.state(), wisp.device.get('haptic.main', 'level'))\n"
            "return ui.screen {}\n",
            {{"scene.json", R"({"id":"demo","name":"Demo","version":"1.0.0","schema_version":1,)"
                            R"("background":true,"permissions":{"device":["haptic.main"]}})"}});
    ASSERT_TRUE(rt->open_background("demo"));
    ASSERT_TRUE(wait_until([&] { return !scene_log("demo").empty(); }, 3000));
    EXPECT_EQ(scene_log("demo")[0], "background\t1");
}

// A scene the agent calls a tool of while it is not running loads headless
// and may still call the devices it declares.
TEST_F(DeviceBinding, HeadlessSceneCallsTheDevice) {
    start();
    install("demo", R"(
wisp.tools.register("buzz", function()
  return wisp.scene.state(), wisp.device.call("haptic.main", "pulse", {ms = 150})
end)
return ui.screen {}
)",
            {{"scene.json", R"({"id":"demo","name":"Demo","version":"1.0.0","schema_version":1,)"
                            R"("permissions":{"device":["haptic.main"]},)"
                            R"("tools":[{"name":"buzz","description":"Buzz once."}]})"}});
    auto r = rt->call_tool("demo", "buzz", "{}", 5000, {});
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(r.value().ok) << r.value().error << " " << r.value().message;
    EXPECT_EQ(std::string(r.value().json.data(), r.value().json.size()), R"(["headless",true])");
    EXPECT_EQ(haptic->invocations().size(), 1u);
}

// list() under a VM memory limit it cannot fit in ends in the VM's memory
// fault, like any allocation in a binding; the next run is unaffected.
TEST_F(DeviceBinding, ListOutOfMemoryIsTheVmsMemoryFault) {
    for (int i = 0; i < 31; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "other.g%02d", i);
        ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>(id, &kFakeGenericSchema)));
    }
    start();
    wisp::scene::LuaRunOptions opts;
    opts.memory_bytes = wisp::scene::kLuaRunMinMemory;
    auto r =
        rt->run_lua("local keep = {} for i = 1, 1e6 do keep[i] = wisp.device.list() end", opts);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_FALSE(r.value().ok);
    EXPECT_EQ(r.value().error, "memory") << r.value().message;

    auto again = rt->run_lua("return #wisp.device.list()", opts);
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_TRUE(again.value().ok) << again.value().error << " " << again.value().message;
    EXPECT_EQ(std::string(again.value().json.data(), again.value().json.size()), "32");
}

TEST_F(DeviceBinding, AgentLuaRunIsGrantedAsAgent) {
    start();
    auto r = rt->run_lua("return wisp.device.call('haptic.main','pulse',{ms=150})", {});
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(r.value().ok) << r.value().error << " " << r.value().message;
    EXPECT_EQ(std::string(r.value().json.data(), r.value().json.size()), "true");
    const std::string ring = wisp::log::ring_snapshot();
    EXPECT_EQ(count_of(ring, "device haptic.main.pulse by agent"), 1u) << ring;
}
