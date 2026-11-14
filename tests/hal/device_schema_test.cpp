#include "wisp/hal/device_schema.hpp"

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>
#include <type_traits>
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

TEST(DeviceSchema, LongUnknownKeysAreTruncatedInTheMessage) {
    auto r = pulse({{"ms", 150}, {std::string(100, 'k'), 1}});
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message, "unknown argument '" + std::string(32, 'k') + "...'");

    // Cut at a character boundary: "a" and 10 three-byte characters are
    // 31 bytes; the 11th would straddle the 32-byte limit, so it goes.
    std::string cjk = "a";
    for (int i = 0; i < 11; ++i) cjk += "日";
    auto wide = pulse({{"ms", 150}, {cjk, 1}});
    ASSERT_FALSE(wide);
    EXPECT_EQ(wide.error().message, "unknown argument '" + cjk.substr(0, 31) + "...'");

    // Exactly 32 bytes is shown whole.
    auto exact = pulse({{"ms", 150}, {std::string(32, 'k'), 1}});
    ASSERT_FALSE(exact);
    EXPECT_EQ(exact.error().message, "unknown argument '" + std::string(32, 'k') + "'");
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

// A full table is a constant expression, so a driver's table is
// read-only data with no static constructor.
namespace {
using wisp::hal::ParamDefault;
constexpr const char* kLevels[] = {"low", "high"};
constexpr ParamSpec kConstArgs[] = {
    {.name = "on", .type = ParamType::Bool, .optional = true, .def = true},
    {.name = "count", .type = ParamType::Int, .min = 0, .max = 9, .optional = true, .def = 5},
    {.name = "gain", .type = ParamType::Number, .optional = true, .def = 1.5},
    {.name = "label", .type = ParamType::String, .optional = true, .def = "hi"},
    {.name = "level", .type = ParamType::Enum, .choices = kLevels, .optional = true, .def = "low"},
    {.name = "plain", .type = ParamType::Int, .optional = true},
};
constexpr CmdSpec kConstCmds[] = {{.name = "set", .desc = "Set all", .args = kConstArgs}};
constinit const DeviceSchema kConstSchema{.props = {}, .cmds = kConstCmds};

static_assert(ParamDefault(80).kind() == ParamDefault::Kind::Int);
static_assert(ParamDefault(80u).kind() == ParamDefault::Kind::Int);
static_assert(ParamDefault(int64_t{-3}).as_int() == -3);
static_assert(ParamDefault(80.0).kind() == ParamDefault::Kind::Number);
static_assert(ParamDefault(80.0f).kind() == ParamDefault::Kind::Number);
static_assert(ParamDefault(true).kind() == ParamDefault::Kind::Bool);
static_assert(ParamDefault(false).as_bool() == false);
static_assert(ParamDefault("soft").kind() == ParamDefault::Kind::String);
static_assert(ParamDefault().kind() == ParamDefault::Kind::None);
static_assert(ParamDefault().is_none());
static_assert(!std::is_constructible_v<ParamDefault, std::nullptr_t>);
static_assert(std::is_trivially_destructible_v<ParamSpec>);
static_assert(std::is_trivially_destructible_v<CmdSpec>);
static_assert(std::is_trivially_destructible_v<DeviceSchema>);
static_assert(kConstArgs[1].def.as_int() == 5);
static_assert(sizeof(ParamDefault) <= 16);
}  // namespace

TEST(DeviceSchema, DefaultsOfEveryKindRoundTrip) {
    ASSERT_TRUE(wisp::hal::check_schema(kConstSchema));
    EXPECT_EQ(wisp::to_json(wisp::hal::render_schema(kConstSchema)),
              R"({"cmds":[{"args":[)"
              R"({"default":true,"name":"on","optional":true,"type":"bool"},)"
              R"({"default":5,"max":9,"min":0,"name":"count","optional":true,"type":"int"},)"
              R"({"default":1.5,"name":"gain","optional":true,"type":"number"},)"
              R"({"default":"hi","max_len":64,"name":"label","optional":true,"type":"string"},)"
              R"({"choices":["low","high"],"default":"low","name":"level","optional":true,)"
              R"("type":"enum"},)"
              R"({"name":"plain","optional":true,"type":"int"}],)"
              R"("changes_state":true,"desc":"Set all","min_interval_ms":0,"name":"set"}],)"
              R"("props":[]})");

    auto filled = wisp::hal::validate_args(kConstCmds[0], Value());
    ASSERT_TRUE(filled) << message_of(filled);
    EXPECT_EQ(filled.value(),
              (Map{{"on", true}, {"count", 5}, {"gain", 1.5}, {"label", "hi"}, {"level", "low"}}));
    EXPECT_EQ(filled.value().at("count").type(), Value::Type::Int);
    EXPECT_EQ(filled.value().at("count").as_int(), 5);
    EXPECT_EQ(filled.value().at("gain").type(), Value::Type::Double);
    EXPECT_EQ(filled.value().count("plain"), 0u);  // no default: left out

    // The fake's Int default reaches the driver as an Int.
    auto pulse_args = pulse({{"ms", 150}});
    ASSERT_TRUE(pulse_args);
    EXPECT_EQ(pulse_args.value().at("strength").type(), Value::Type::Int);
    EXPECT_EQ(pulse_args.value().at("strength").as_int(), 80);
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
        const double nan = std::numeric_limits<double>::quiet_NaN();
        const ParamSpec nan_min[] = {
            {.name = "x", .type = ParamType::Number, .min = nan, .max = 1}};
        EXPECT_EQ(check_message({.props = nan_min, .cmds = {}}), "prop 'x': min > max");
        const ParamSpec nan_max[] = {
            {.name = "x", .type = ParamType::Number, .min = 0, .max = nan}};
        EXPECT_EQ(check_message({.props = nan_max, .cmds = {}}), "prop 'x': min > max");
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
        static constexpr const char* kTwice[] = {"soft", "hard", "soft"};
        const ParamSpec twice[] = {{.name = "mode", .type = ParamType::Enum, .choices = kTwice}};
        EXPECT_EQ(check_message({.props = twice, .cmds = {}}),
                  "prop 'mode': duplicate choice 'soft'");
        static constexpr const char* kBlank[] = {"soft", ""};
        const ParamSpec blank[] = {{.name = "mode", .type = ParamType::Enum, .choices = kBlank}};
        EXPECT_EQ(check_message({.props = blank, .cmds = {}}), "prop 'mode': empty choice");
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
                                   .def = 101}};
        const CmdSpec cmds[] = {{.name = "pulse", .desc = "", .args = args}};
        EXPECT_EQ(check_message({.props = {}, .cmds = cmds}),
                  "cmd 'pulse' arg 'strength': bad default: 'strength' must be between 0 and 100");
        const ParamSpec no_default[] = {{.name = "x", .type = ParamType::Int, .optional = true}};
        const CmdSpec fine[] = {{.name = "set", .desc = "", .args = no_default}};
        EXPECT_EQ(check_message({.props = {}, .cmds = fine}), "<ok>");
    }
    {  // A default's kind must match the declared type.
        auto arg_message = [](ParamSpec arg) {
            const ParamSpec args[] = {arg};
            const CmdSpec cmds[] = {{.name = "set", .desc = "", .args = args}};
            return check_message({.props = {}, .cmds = cmds});
        };
        EXPECT_EQ(
            arg_message({.name = "ms", .type = ParamType::Int, .optional = true, .def = 80.0}),
            "cmd 'set' arg 'ms': default must be int");
        EXPECT_EQ(
            arg_message({.name = "ms", .type = ParamType::Int, .optional = true, .def = true}),
            "cmd 'set' arg 'ms': default must be int");
        EXPECT_EQ(
            arg_message({.name = "gain", .type = ParamType::Number, .optional = true, .def = 1}),
            "<ok>");
        EXPECT_EQ(
            arg_message({.name = "gain", .type = ParamType::Number, .optional = true, .def = 0.5}),
            "<ok>");
        EXPECT_EQ(
            arg_message({.name = "gain", .type = ParamType::Number, .optional = true, .def = "1"}),
            "cmd 'set' arg 'gain': default must be number");
        EXPECT_EQ(arg_message({.name = "on", .type = ParamType::Bool, .optional = true, .def = 1}),
                  "cmd 'set' arg 'on': default must be bool");
        EXPECT_EQ(
            arg_message({.name = "mode", .type = ParamType::String, .optional = true, .def = 1}),
            "cmd 'set' arg 'mode': default must be string");
        EXPECT_EQ(arg_message({.name = "text",
                               .type = ParamType::String,
                               .max_len = 2,
                               .optional = true,
                               .def = "abc"}),
                  "cmd 'set' arg 'text': bad default: 'text' must be at most 2 bytes");
        static constexpr const char* kModes[] = {"soft", "hard"};
        EXPECT_EQ(arg_message({.name = "mode",
                               .type = ParamType::Enum,
                               .choices = kModes,
                               .optional = true,
                               .def = "medium"}),
                  "cmd 'set' arg 'mode': bad default: 'mode' must be one of: soft, hard");
        EXPECT_EQ(arg_message({.name = "mode",
                               .type = ParamType::Enum,
                               .choices = kModes,
                               .optional = true,
                               .def = 1}),
                  "cmd 'set' arg 'mode': default must be string");
        const char* null_text = nullptr;
        EXPECT_EQ(
            arg_message(
                {.name = "text", .type = ParamType::String, .optional = true, .def = null_text}),
            "cmd 'set' arg 'text': default is null");
    }
    {  // A default only for an optional arg.
        const ParamSpec props[] = {{.name = "x", .type = ParamType::Int, .def = 1}};
        EXPECT_EQ(check_message({.props = props, .cmds = {}}), "prop 'x': props take no default");
        const ParamSpec args[] = {{.name = "ms", .type = ParamType::Int, .def = 1}};
        const CmdSpec cmds[] = {{.name = "pulse", .desc = "", .args = args}};
        EXPECT_EQ(check_message({.props = {}, .cmds = cmds}),
                  "cmd 'pulse' arg 'ms': default without optional");
    }
    {  // Names are not Lua keywords: scenes write {ms = 150}.
        const ParamSpec props[] = {{.name = "end", .type = ParamType::Int}};
        EXPECT_EQ(check_message({.props = props, .cmds = {}}), "prop 'end': name is a Lua keyword");
        const CmdSpec cmds[] = {{.name = "repeat", .desc = "", .args = {}}};
        EXPECT_EQ(check_message({.props = {}, .cmds = cmds}),
                  "cmd 'repeat': name is a Lua keyword");
        const ParamSpec args[] = {{.name = "function", .type = ParamType::Int}};
        const CmdSpec with_arg[] = {{.name = "go", .desc = "", .args = args}};
        EXPECT_EQ(check_message({.props = {}, .cmds = with_arg}),
                  "cmd 'go' arg 'function': name is a Lua keyword");
        for (const char* keyword : {"and", "goto", "nil", "until", "while", "true"}) {
            const ParamSpec one[] = {{.name = keyword, .type = ParamType::Int}};
            EXPECT_NE(check_message({.props = one, .cmds = {}}), "<ok>") << keyword;
        }
        // Prefixes and longer names that merely contain a keyword are fine.
        const ParamSpec near[] = {{.name = "ends", .type = ParamType::Int},
                                  {.name = "do_it", .type = ParamType::Int},
                                  {.name = "i", .type = ParamType::Int}};
        EXPECT_EQ(check_message({.props = near, .cmds = {}}), "<ok>");
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
