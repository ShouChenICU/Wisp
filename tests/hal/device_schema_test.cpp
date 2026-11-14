#include "wisp/hal/device_schema.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "wisp/base/json.hpp"

using wisp::ErrorCode;
using wisp::Map;
using wisp::Value;
using wisp::hal::CmdSpec;
using wisp::hal::DeviceSchema;
using wisp::hal::ParamSpec;
using wisp::hal::ParamType;
using wisp::test::kFakeGenericSchema;

namespace {

const CmdSpec& cmd(std::string_view name) {
    const CmdSpec* c = wisp::hal::find_cmd(kFakeGenericSchema, name);
    EXPECT_NE(c, nullptr) << name;
    return *c;
}

wisp::Result<Map> pulse(Map args) {
    return wisp::hal::validate_args(cmd("pulse"), Value(std::move(args)));
}

std::string message_of(const wisp::Result<Map>& r) {
    return r ? std::string("<ok>") : r.error().message;
}

}  // namespace

TEST(DeviceSchema, RangeEdgesAreInclusive) {
    EXPECT_TRUE(pulse({{"ms", 10}}));
    EXPECT_TRUE(pulse({{"ms", 2000}}));
    for (int ms : {9, 2001}) {
        auto r = pulse({{"ms", ms}});
        ASSERT_FALSE(r) << ms;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(r.error().message, "'ms' must be between 10 and 2000");
    }
}

TEST(DeviceSchema, IntegralDoublesAreInts) {
    auto r = pulse({{"ms", 150.0}});
    ASSERT_TRUE(r) << message_of(r);
    EXPECT_EQ(r.value().at("ms").type(), Value::Type::Int);
    EXPECT_EQ(r.value().at("ms").as_int(), 150);

    auto half = pulse({{"ms", 150.5}});
    ASSERT_FALSE(half);
    EXPECT_EQ(half.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(half.error().message, "'ms' must be int");
}

TEST(DeviceSchema, DefaultsAreFilled) {
    auto r = pulse({{"ms", 150}});
    ASSERT_TRUE(r) << message_of(r);
    EXPECT_EQ(r.value(), (Map{{"ms", 150}, {"strength", 80}}));

    auto given = pulse({{"ms", 150}, {"strength", 0}});
    ASSERT_TRUE(given);
    EXPECT_EQ(given.value().at("strength"), Value(0));
}

TEST(DeviceSchema, MissingAndUnknownArgumentsAreNamed) {
    auto missing = pulse({});
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(missing.error().message, "missing argument 'ms'");

    auto unknown = pulse({{"ms", 150}, {"speed", 1}});
    ASSERT_FALSE(unknown);
    EXPECT_EQ(unknown.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(unknown.error().message, "unknown argument 'speed'");
}

TEST(DeviceSchema, TypesAreChecked) {
    auto as_string = pulse({{"ms", "150"}});
    ASSERT_FALSE(as_string);
    EXPECT_EQ(as_string.error().message, "'ms' must be int");

    auto as_bool = pulse({{"ms", true}});
    ASSERT_FALSE(as_bool);
    EXPECT_EQ(as_bool.error().message, "'ms' must be int");

    auto bad_mode = wisp::hal::validate_args(cmd("set_mode"), Value(Map{{"mode", "x"}}));
    ASSERT_FALSE(bad_mode);
    EXPECT_EQ(bad_mode.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(bad_mode.error().message, "'mode' must be one of: soft, hard");
    EXPECT_TRUE(wisp::hal::validate_args(cmd("set_mode"), Value(Map{{"mode", "hard"}})));

    auto not_object = wisp::hal::validate_args(cmd("pulse"), Value(wisp::List{}));
    ASSERT_FALSE(not_object);
    EXPECT_EQ(not_object.error().message, "args must be an object");
}

TEST(DeviceSchema, BoolAndNumberTypes) {
    const ParamSpec args[] = {
        {.name = "on", .type = ParamType::Bool},
        {.name = "gain", .type = ParamType::Number, .min = -1.5, .max = 1.5},
    };
    const CmdSpec c{.name = "set", .desc = "", .args = args};

    auto ok = wisp::hal::validate_args(c, Value(Map{{"on", false}, {"gain", 1}}));
    ASSERT_TRUE(ok) << message_of(ok);
    // A Number keeps the type it was given.
    EXPECT_EQ(ok.value().at("gain").type(), Value::Type::Int);
    EXPECT_TRUE(wisp::hal::validate_args(c, Value(Map{{"on", true}, {"gain", -1.5}})));

    EXPECT_EQ(message_of(wisp::hal::validate_args(c, Value(Map{{"on", 1}, {"gain", 0}}))),
              "'on' must be bool");
    EXPECT_EQ(message_of(wisp::hal::validate_args(c, Value(Map{{"on", true}, {"gain", "1"}}))),
              "'gain' must be number");
    EXPECT_EQ(message_of(wisp::hal::validate_args(c, Value(Map{{"on", true}, {"gain", 1.6}}))),
              "'gain' must be between -1.5 and 1.5");
}

TEST(DeviceSchema, StringLengthIsInBytes) {
    const ParamSpec args[] = {{.name = "text", .type = ParamType::String, .max_len = 4}};
    const CmdSpec c{.name = "show", .desc = "", .args = args};
    auto with = [&](std::string s) {
        return wisp::hal::validate_args(c, Value(Map{{"text", Value(std::move(s))}}));
    };
    EXPECT_TRUE(with("abcd"));
    EXPECT_EQ(message_of(with("abcde")), "'text' must be at most 4 bytes");
    EXPECT_EQ(message_of(with("日本")), "'text' must be at most 4 bytes");  // 6 bytes
    EXPECT_EQ(message_of(with("\xC3")), "'text' must be valid UTF-8");
    EXPECT_EQ(message_of(with("ab\xFF")), "'text' must be valid UTF-8");
}

TEST(DeviceSchema, NullArgsMeanEmptyObject) {
    auto r = wisp::hal::validate_args(cmd("read_raw"), Value());
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message, "missing argument 'reg'");

    const CmdSpec no_args{.name = "stop", .desc = "", .args = {}};
    auto none = wisp::hal::validate_args(no_args, Value());
    ASSERT_TRUE(none);
    EXPECT_TRUE(none.value().empty());
}

TEST(DeviceSchema, RenderKeepsOrderAndShape) {
    EXPECT_EQ(
        wisp::to_json(wisp::hal::render_schema(kFakeGenericSchema)),
        R"({"cmds":[)"
        R"({"args":[{"max":2000,"min":10,"name":"ms","type":"int","unit":"ms"},)"
        R"({"default":80,"max":100,"min":0,"name":"strength","optional":true,"type":"int"}],)"
        R"("changes_state":true,"desc":"Vibrate once","min_interval_ms":100,"name":"pulse"},)"
        R"({"args":[{"choices":["soft","hard"],"name":"mode","type":"enum"}],)"
        R"("changes_state":true,"desc":"Pick the drive mode","min_interval_ms":0,"name":"set_mode"},)"
        R"({"args":[{"max":255,"min":0,"name":"reg","type":"int"}],)"
        R"("changes_state":false,"desc":"Read one register","min_interval_ms":0,"name":"read_raw"}],)"
        R"("props":[{"desc":"Drive level","max":100,"min":0,"name":"level","type":"int","unit":"%"}]})");
}

TEST(DeviceSchema, RenderShowsNumberRangesStringLengthsAndNoEmptyFields) {
    const ParamSpec props[] = {
        {.name = "temp_c", .type = ParamType::Number, .min = -40.5, .max = 85},
        {.name = "label", .type = ParamType::String, .max_len = 12},
        {.name = "on", .type = ParamType::Bool},
    };
    const DeviceSchema schema{.props = props, .cmds = {}};
    EXPECT_EQ(wisp::to_json(wisp::hal::render_schema(schema)),
              R"({"cmds":[],"props":[{"max":85,"min":-40.5,"name":"temp_c","type":"number"},)"
              R"({"max_len":12,"name":"label","type":"string"},{"name":"on","type":"bool"}]})");
    const Value rendered = wisp::hal::render_schema(schema);
    EXPECT_EQ(rendered.find("props")->as_list()[0].find("max")->type(), Value::Type::Double);
}

TEST(DeviceSchema, FakeSchemaPassesTheChecks) {
    auto r = wisp::hal::check_schema(kFakeGenericSchema);
    EXPECT_TRUE(r) << (r ? "" : r.error().message);
    EXPECT_NE(wisp::hal::find_prop(kFakeGenericSchema, "level"), nullptr);
    EXPECT_EQ(wisp::hal::find_prop(kFakeGenericSchema, "pulse"), nullptr);
    EXPECT_EQ(wisp::hal::find_cmd(kFakeGenericSchema, "level"), nullptr);
}

namespace {

std::string check_message(const DeviceSchema& schema) {
    auto r = wisp::hal::check_schema(schema);
    if (r) return "<ok>";
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    return r.error().message;
}

}  // namespace

TEST(DeviceSchema, CheckRejectsBadTables) {
    const ParamSpec int_arg[] = {{.name = "ms", .type = ParamType::Int}};

    {  // Names follow ^[a-z][a-z0-9_]{0,31}$.
        const CmdSpec cmds[] = {{.name = "Pulse", .desc = "", .args = {}}};
        EXPECT_EQ(check_message({.props = {}, .cmds = cmds}),
                  "cmd 'Pulse': name must match [a-z][a-z0-9_]{0,31}");
        const std::string long_name(33, 'a');
        const ParamSpec props[] = {{.name = long_name.c_str(), .type = ParamType::Int}};
        EXPECT_EQ(check_message({.props = props, .cmds = {}}),
                  "prop '" + long_name + "': name must match [a-z][a-z0-9_]{0,31}");
        const ParamSpec bad_arg[] = {{.name = "9ms", .type = ParamType::Int}};
        const CmdSpec with_bad_arg[] = {{.name = "pulse", .desc = "", .args = bad_arg}};
        EXPECT_EQ(check_message({.props = {}, .cmds = with_bad_arg}),
                  "cmd 'pulse' arg '9ms': name must match [a-z][a-z0-9_]{0,31}");
        const ParamSpec null_name[] = {{.name = nullptr, .type = ParamType::Int}};
        EXPECT_EQ(check_message({.props = null_name, .cmds = {}}),
                  "prop '': name must match [a-z][a-z0-9_]{0,31}");
    }
    {  // Counts.
        std::vector<std::string> names;
        for (int i = 0; i < 17; ++i) names.push_back("p" + std::to_string(i));
        std::vector<ParamSpec> props;
        for (const auto& n : names) props.push_back({.name = n.c_str(), .type = ParamType::Int});
        EXPECT_EQ(check_message({.props = props, .cmds = {}}), "17 props (at most 16)");
        props.pop_back();
        EXPECT_EQ(check_message({.props = props, .cmds = {}}), "<ok>");

        std::vector<ParamSpec> args(props.begin(), props.begin() + 9);
        const CmdSpec nine[] = {{.name = "go", .desc = "", .args = args}};
        EXPECT_EQ(check_message({.props = {}, .cmds = nine}), "cmd 'go': 9 args (at most 8)");

        std::vector<CmdSpec> cmds;
        for (const auto& n : names) cmds.push_back({.name = n.c_str(), .desc = "", .args = {}});
        EXPECT_EQ(check_message({.props = {}, .cmds = cmds}), "17 cmds (at most 16)");
    }
    {  // Duplicates within one list; a prop and a cmd may share a name.
        const CmdSpec cmds[] = {{.name = "pulse", .desc = "", .args = {}},
                                {.name = "pulse", .desc = "", .args = {}}};
        EXPECT_EQ(check_message({.props = {}, .cmds = cmds}), "duplicate cmd 'pulse'");
        const ParamSpec props[] = {{.name = "level", .type = ParamType::Int},
                                   {.name = "level", .type = ParamType::Bool}};
        EXPECT_EQ(check_message({.props = props, .cmds = {}}), "duplicate prop 'level'");
        const ParamSpec args[] = {{.name = "ms", .type = ParamType::Int},
                                  {.name = "ms", .type = ParamType::Int}};
        const CmdSpec dup_args[] = {{.name = "pulse", .desc = "", .args = args}};
        EXPECT_EQ(check_message({.props = {}, .cmds = dup_args}),
                  "cmd 'pulse': duplicate arg 'ms'");
        const CmdSpec shared[] = {{.name = "level", .desc = "", .args = {}}};
        EXPECT_EQ(check_message({.props = int_arg, .cmds = shared}), "<ok>");
    }
    {  // A command needs a description.
        const CmdSpec cmds[] = {{.name = "pulse", .desc = nullptr, .args = {}}};
        EXPECT_EQ(check_message({.props = {}, .cmds = cmds}), "cmd 'pulse': desc is null");
    }
    {  // Ranges.
        const ParamSpec props[] = {{.name = "x", .type = ParamType::Number, .min = 2, .max = 1}};
        EXPECT_EQ(check_message({.props = props, .cmds = {}}), "prop 'x': min > max");
        const ParamSpec frac[] = {{.name = "x", .type = ParamType::Int, .min = 0.5, .max = 2}};
        EXPECT_EQ(check_message({.props = frac, .cmds = {}}),
                  "prop 'x': an int range needs integral bounds");
        const ParamSpec on_bool[] = {{.name = "x", .type = ParamType::Bool, .min = 0, .max = 1}};
        EXPECT_EQ(check_message({.props = on_bool, .cmds = {}}),
                  "prop 'x': a range is only for int and number");
    }
    {  // Enums.
        const ParamSpec empty[] = {{.name = "mode", .type = ParamType::Enum}};
        EXPECT_EQ(check_message({.props = empty, .cmds = {}}), "prop 'mode': enum without choices");
        static constexpr const char* kWithNull[] = {"soft", nullptr};
        const ParamSpec null_choice[] = {
            {.name = "mode", .type = ParamType::Enum, .choices = kWithNull}};
        EXPECT_EQ(check_message({.props = null_choice, .cmds = {}}), "prop 'mode': null choice");
        static constexpr const char* kSoft[] = {"soft"};
        const ParamSpec on_int[] = {{.name = "x", .type = ParamType::Int, .choices = kSoft}};
        EXPECT_EQ(check_message({.props = on_int, .cmds = {}}),
                  "prop 'x': choices are only for enum");
    }
    {  // optional is for args only, and a default must pass its own arg's rules.
        const ParamSpec props[] = {{.name = "x", .type = ParamType::Int, .optional = true}};
        EXPECT_EQ(check_message({.props = props, .cmds = {}}),
                  "prop 'x': props are never optional");
        const ParamSpec args[] = {{.name = "strength",
                                   .type = ParamType::Int,
                                   .min = 0,
                                   .max = 100,
                                   .optional = true,
                                   .def = Value(101)}};
        const CmdSpec cmds[] = {{.name = "pulse", .desc = "", .args = args}};
        EXPECT_EQ(check_message({.props = {}, .cmds = cmds}),
                  "cmd 'pulse' arg 'strength': bad default: 'strength' must be between 0 and 100");
        const ParamSpec typed[] = {
            {.name = "mode", .type = ParamType::String, .optional = true, .def = Value(1)}};
        const CmdSpec typed_cmds[] = {{.name = "set", .desc = "", .args = typed}};
        EXPECT_EQ(check_message({.props = {}, .cmds = typed_cmds}),
                  "cmd 'set' arg 'mode': bad default: 'mode' must be string");
        const ParamSpec no_default[] = {{.name = "x", .type = ParamType::Int, .optional = true}};
        const CmdSpec fine[] = {{.name = "set", .desc = "", .args = no_default}};
        EXPECT_EQ(check_message({.props = {}, .cmds = fine}), "<ok>");
    }
    {  // Null strings that would be read.
        const ParamSpec props[] = {{.name = "x", .type = ParamType::Int, .desc = nullptr}};
        EXPECT_EQ(check_message({.props = props, .cmds = {}}), "prop 'x': desc is null");
        const ParamSpec units[] = {{.name = "x", .type = ParamType::Int, .unit = nullptr}};
        EXPECT_EQ(check_message({.props = units, .cmds = {}}), "prop 'x': unit is null");
    }
}

TEST(DeviceSchema, CapsOfAddsTheSchema) {
    wisp::test::FakeGeneric generic("haptic.main", &kFakeGenericSchema,
                                    wisp::Properties{{"standby", "off"}});
    const wisp::Properties caps = wisp::hal::caps_of(generic);
    ASSERT_EQ(caps.count("schema"), 1u);
    EXPECT_EQ(caps.at("schema"), wisp::hal::render_schema(kFakeGenericSchema));
    EXPECT_EQ(caps.at("standby"), Value("off"));
    EXPECT_EQ(caps.size(), 2u);

    wisp::test::FakeImu imu("imu.main");
    EXPECT_EQ(wisp::hal::caps_of(imu), imu.info().caps);
}

TEST(DeviceSchema, DeviceDefaultsAreUnsupported) {
    wisp::test::FakeGeneric plain("haptic.main");
    const wisp::hal::Device& device = plain;
    EXPECT_EQ(device.describe(), nullptr);
    wisp::test::FakeImu imu("imu.main");
    wisp::hal::Device& as_device = imu;
    EXPECT_EQ(as_device.get("x").error().code, ErrorCode::Unsupported);
    EXPECT_EQ(as_device.invoke("x", Map{}).error().code, ErrorCode::Unsupported);
}

TEST(DeviceSchema, FakeGenericRecordsCalls) {
    wisp::test::FakeGeneric fake("haptic.main", &kFakeGenericSchema, {}, "main", true);
    EXPECT_EQ(fake.describe(), &kFakeGenericSchema);
    EXPECT_EQ(fake.info().caps.at("role"), Value("main"));
    EXPECT_EQ(fake.info().caps.at("standby"), Value("off"));
    EXPECT_EQ(fake.get("level").value(), Value(1));
    EXPECT_TRUE(fake.invoke("pulse", Map{{"ms", 150}}).value().is_null());
    EXPECT_TRUE(fake.set_power(wisp::hal::PowerLevel::Standby));
    EXPECT_EQ(fake.gets(), std::vector<std::string>{"level"});
    ASSERT_EQ(fake.invocations().size(), 1u);
    EXPECT_EQ(fake.invocations()[0].first, "pulse");
    EXPECT_EQ(fake.set_power_calls(), 1);
    EXPECT_FALSE(fake.powered());
}
