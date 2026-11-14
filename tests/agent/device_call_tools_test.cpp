// device.list and device.call (device model spec 4.2): thin adapters over
// core::DeviceService, so these check the adapter's own part -- the result
// shape, the 32-entry cap, the note and the error codes -- not the
// service's checks, which device_service_test covers.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

#include "support/fake_devices.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/device_service.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/device_schema.hpp"

namespace {

using wisp::ErrorCode;
using wisp::Map;
using wisp::Value;
using wisp::agent::ToolCtx;
using wisp::agent::ToolOutput;
using wisp::test::FakeGeneric;
using wisp::test::FakeImu;
using wisp::test::kFakeGenericSchema;

std::string str(const Value& v, std::string_view key) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == Value::Type::String ? f->as_string() : std::string();
}

class DeviceCallTools : public ::testing::Test {
protected:
    void SetUp() override {
        ctx.cancelled = [] { return false; };
        ctx.lang = "zh";
        haptic = std::make_shared<FakeGeneric>("haptic.main", &kFakeGenericSchema);
        ASSERT_TRUE(registry.add(haptic));
        ASSERT_TRUE(registry.add(std::make_shared<FakeImu>("imu.main")));
        ASSERT_TRUE(wisp::agent::add_device_call_tools(
            tools, wisp::agent::DeviceCallToolDeps{.devices = service}));
    }

    ToolOutput call(const char* wire, const std::string& args) {
        return tools.call(wire, args, ctx);
    }
    // The result as a Value, whichever of the two forms the tool used.
    static Value result_of(const ToolOutput& out) {
        if (out.result_json.empty()) return out.result;
        auto v = wisp::from_json(std::string_view(out.result_json.data(), out.result_json.size()));
        EXPECT_TRUE(v);
        return v ? v.value() : Value();
    }
    ToolOutput pulse(int ms) {
        return call("device_call",
                    R"({"id":"haptic.main","op":"invoke","name":"pulse","args":{"ms":)" +
                        std::to_string(ms) + "}}");
    }

    std::atomic<uint64_t> t{1000};
    wisp::core::DeviceRegistry registry;
    wisp::core::DeviceService service{registry, nullptr, [this] { return t.load(); }};
    wisp::agent::ToolRegistry tools{wisp::agent::ToolExecutors{}};
    std::shared_ptr<FakeGeneric> haptic;
    ToolCtx ctx;
};

}  // namespace

TEST_F(DeviceCallTools, ListShowsOnlyDescribedDevices) {
    ToolOutput out = call("device_list", "");
    ASSERT_TRUE(out.ok);
    EXPECT_FALSE(out.result_json.empty());  // written straight into PSRAM
    Value r = result_of(out);
    EXPECT_EQ(r.find("ok")->as_bool(), true);
    const auto& devices = r.find("devices")->as_list();
    ASSERT_EQ(devices.size(), 1u);
    EXPECT_EQ(str(devices[0], "id"), "haptic.main");
    const Value* schema = devices[0].find("schema");
    ASSERT_NE(schema, nullptr);
    EXPECT_NE(schema->find("cmds"), nullptr);
    EXPECT_EQ(r.find("truncated"), nullptr);
    EXPECT_TRUE(out.note.empty());
}

TEST_F(DeviceCallTools, ListStopsAt32) {
    for (int i = 0; i < 40; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "other.g%02d", i);
        ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>(id, &kFakeGenericSchema)));
    }
    Value r = result_of(call("device_list", ""));
    EXPECT_EQ(r.find("devices")->as_list().size(), 32u);
    ASSERT_NE(r.find("truncated"), nullptr);
    EXPECT_EQ(r.find("truncated")->as_bool(), true);
}

// Exactly the cap is everything: nothing was cut, so no "truncated".
TEST_F(DeviceCallTools, ListOfExactly32IsNotTruncated) {
    for (int i = 0; i < 31; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "other.g%02d", i);
        ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>(id, &kFakeGenericSchema)));
    }
    Value r = result_of(call("device_list", ""));
    EXPECT_EQ(r.find("devices")->as_list().size(), 32u);
    EXPECT_EQ(r.find("truncated"), nullptr);
}

// A schema whose descriptions are `desc_bytes` long, for the byte budget.
struct BigSchema {
    explicit BigSchema(size_t desc_bytes)
        : desc(desc_bytes, 'd'),
          prop{{.name = "level", .type = wisp::hal::ParamType::Int, .desc = desc.c_str()}},
          schema{.props = prop} {}
    std::string desc;
    wisp::hal::ParamSpec prop[1];
    wisp::hal::DeviceSchema schema;
};

// Whole entries only, never past the budget: a valid document whose
// entries stop before the one that would cross it.
TEST_F(DeviceCallTools, ListStopsBeforeTheByteBudget) {
    BigSchema big(20 * 1024);
    for (const char* id : {"other.a", "other.b", "other.c", "other.d"}) {
        ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>(id, &big.schema)));
    }
    ToolOutput out = call("device_list", "");
    ASSERT_TRUE(out.ok);
    EXPECT_LE(out.result_json.size(), wisp::agent::kDeviceListMaxBytes);
    Value r = result_of(out);  // parses: a whole document
    const auto& devices = r.find("devices")->as_list();
    // haptic.main and three 20 KB devices fit in 64 KB; the fourth does not.
    ASSERT_EQ(devices.size(), 4u);
    EXPECT_EQ(str(devices[0], "id"), "haptic.main");
    EXPECT_EQ(str(devices[3], "id"), "other.c");
    for (const Value& d : devices) EXPECT_NE(d.find("schema"), nullptr);
    ASSERT_NE(r.find("truncated"), nullptr);
    EXPECT_EQ(r.find("truncated")->as_bool(), true);
}

TEST_F(DeviceCallTools, ADeviceLargerThanTheBudgetIsLeftOut) {
    ASSERT_TRUE(registry.remove("haptic.main"));
    BigSchema huge(wisp::agent::kDeviceListMaxBytes + 1);
    ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>("other.huge", &huge.schema)));
    ToolOutput out = call("device_list", "");
    ASSERT_TRUE(out.ok);
    Value r = result_of(out);
    EXPECT_TRUE(r.find("devices")->as_list().empty());
    ASSERT_NE(r.find("truncated"), nullptr);
    EXPECT_EQ(r.find("truncated")->as_bool(), true);
}

TEST_F(DeviceCallTools, InvokeRunsTheCommandWithANote) {
    wisp::log::reset_for_test();
    wisp::log::init();
    ToolOutput out = pulse(1234);
    ASSERT_TRUE(out.ok) << wisp::to_json(result_of(out));
    Value r = result_of(out);
    EXPECT_EQ(r.find("ok")->as_bool(), true);
    ASSERT_NE(r.find("result"), nullptr);
    EXPECT_EQ(r.find("result")->type(), Value::Type::Null);
    EXPECT_EQ(out.note, "设备 haptic.main：pulse");
    auto calls = haptic->invocations();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].first, "pulse");
    EXPECT_EQ(calls[0].second.at("strength"), Value(int64_t{80}));
    // The service logs the command; the arguments, which may carry user
    // content, reach no log line.
    // (Tags and messages only: a timestamp could hold the digits.)
    std::string log;
    for (const wisp::LogEntry& e : wisp::log::ring_entries()) log += e.tag + ": " + e.msg + "\n";
    EXPECT_NE(log.find("haptic.main.pulse"), std::string::npos) << log;
    EXPECT_EQ(log.find("1234"), std::string::npos) << log;
    wisp::log::reset_for_test();
}

TEST_F(DeviceCallTools, QueriesHaveNoNote) {
    ToolOutput out = call(
        "device_call", R"({"id":"haptic.main","op":"invoke","name":"read_raw","args":{"reg":3}})");
    ASSERT_TRUE(out.ok);
    EXPECT_TRUE(out.note.empty());
}

TEST_F(DeviceCallTools, GetReturnsTheValue) {
    ToolOutput out = call("device_call", R"({"id":"haptic.main","op":"get","name":"level"})");
    ASSERT_TRUE(out.ok);
    Value r = result_of(out);
    ASSERT_NE(r.find("value"), nullptr);
    EXPECT_EQ(*r.find("value"), Value(1));
    EXPECT_TRUE(out.note.empty());
}

TEST_F(DeviceCallTools, ErrorsUseTheAgentsCodes) {
    ToolOutput none = call("device_call", R"({"id":"haptic.nope","op":"get","name":"level"})");
    EXPECT_FALSE(none.ok);
    EXPECT_EQ(str(result_of(none), "error"), "not_found");

    ToolOutput small = pulse(5);
    EXPECT_FALSE(small.ok);
    EXPECT_EQ(str(result_of(small), "error"), "invalid_value");
    EXPECT_NE(str(result_of(small), "message").find("'ms'"), std::string::npos)
        << str(result_of(small), "message");
    EXPECT_TRUE(small.note.empty());

    ASSERT_TRUE(pulse(150).ok);
    ToolOutput again = pulse(150);
    EXPECT_FALSE(again.ok);
    EXPECT_EQ(str(result_of(again), "error"), "rate_limited");
    EXPECT_NE(str(result_of(again), "message").find("retry_after_ms="), std::string::npos);
    EXPECT_TRUE(again.note.empty());

    haptic->on_invoke = [](std::string_view, const Map&) {
        return wisp::Result<Value>::err(ErrorCode::DeviceFailure, "bus error");
    };
    ToolOutput broken =
        call("device_call",
             R"({"id":"haptic.main","op":"invoke","name":"set_mode","args":{"mode":"soft"}})");
    EXPECT_FALSE(broken.ok);
    EXPECT_EQ(str(result_of(broken), "error"), "device_failure");
    EXPECT_TRUE(broken.note.empty());  // no note for a command that failed

    ToolOutput set = call("device_call", R"({"id":"haptic.main","op":"set","name":"level"})");
    EXPECT_FALSE(set.ok);
    EXPECT_EQ(str(result_of(set), "error"), "bad_args");
    EXPECT_EQ(str(result_of(set), "field"), "op");
}

TEST_F(DeviceCallTools, ArgsMustBeAnObject) {
    ToolOutput out =
        call("device_call", R"({"id":"haptic.main","op":"invoke","name":"pulse","args":[1]})");
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(str(result_of(out), "error"), "bad_args");
    EXPECT_EQ(str(result_of(out), "field"), "args");
    EXPECT_TRUE(haptic->invocations().empty());
}

TEST_F(DeviceCallTools, ChangesStateAsksTheSchema) {
    EXPECT_TRUE(service.changes_state("haptic.main", "pulse"));
    EXPECT_FALSE(service.changes_state("haptic.main", "read_raw"));
    EXPECT_FALSE(service.changes_state("haptic.main", "nope"));
    EXPECT_FALSE(service.changes_state("haptic.nope", "pulse"));
    EXPECT_FALSE(service.changes_state("imu.main", "pulse"));
}
