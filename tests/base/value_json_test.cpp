#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "support/utf8.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"

TEST(Json, RoundTripNested) {
    wisp::Value v = wisp::Map{{"b", wisp::List{1, 2.5, "x", true, wisp::Value()}},
                              {"a", wisp::Map{{"k", "v"}}}};
    auto s = wisp::to_json(v);
    EXPECT_EQ(s, R"({"a":{"k":"v"},"b":[1,2.5,"x",true,null]})");
    auto back = wisp::from_json(s);
    ASSERT_TRUE(back);
    EXPECT_EQ(back.value(), v);
}
TEST(Json, IntAndDoubleStayDistinct) {
    auto v = wisp::from_json(R"({"i":1,"d":1.5})").value();
    EXPECT_EQ(v.find("i")->type(), wisp::Value::Type::Int);
    EXPECT_EQ(v.find("d")->type(), wisp::Value::Type::Double);
}
TEST(Json, LargeInt64) {
    wisp::Value v = int64_t{9007199254740993};
    EXPECT_EQ(wisp::from_json(wisp::to_json(v)).value().as_int(), 9007199254740993);
}
TEST(Json, EscapesStrings) { EXPECT_EQ(wisp::to_json(wisp::Value("a\"b\n中")), "\"a\\\"b\\n中\""); }
TEST(Json, InvalidInputIsError) {
    auto r = wisp::from_json("{bad");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
}
TEST(Value, FindOnNonMapIsNull) { EXPECT_EQ(wisp::Value(3).find("x"), nullptr); }

TEST(Value, DefaultIsNull) {
    wisp::Value v;
    EXPECT_EQ(v.type(), wisp::Value::Type::Null);
    EXPECT_TRUE(v.is_null());
}
TEST(Value, BoolConstruct) {
    wisp::Value v(true);
    EXPECT_EQ(v.type(), wisp::Value::Type::Bool);
    EXPECT_TRUE(v.as_bool());
}
TEST(Value, IntConstructFromIntAndInt64) {
    wisp::Value a(42);
    wisp::Value b(int64_t{42});
    EXPECT_EQ(a.type(), wisp::Value::Type::Int);
    EXPECT_EQ(b.type(), wisp::Value::Type::Int);
    EXPECT_EQ(a.as_int(), 42);
    EXPECT_EQ(a, b);
}
TEST(Value, IntReadableAsDouble) {
    wisp::Value v(5);
    EXPECT_DOUBLE_EQ(v.as_double(), 5.0);
}
TEST(Value, DoubleConstruct) {
    wisp::Value v(1.5);
    EXPECT_EQ(v.type(), wisp::Value::Type::Double);
    EXPECT_DOUBLE_EQ(v.as_double(), 1.5);
}
TEST(Value, StringConstructFromLiteralAndStdString) {
    wisp::Value a("hi");
    wisp::Value b(std::string("hi"));
    EXPECT_EQ(a.type(), wisp::Value::Type::String);
    EXPECT_EQ(a.as_string(), "hi");
    EXPECT_EQ(a, b);
}
TEST(Value, ListAndMapAccessors) {
    wisp::Value list = wisp::List{1, 2};
    EXPECT_EQ(list.type(), wisp::Value::Type::List);
    EXPECT_EQ(list.as_list().size(), 2u);

    wisp::Value map = wisp::Map{{"k", 1}};
    EXPECT_EQ(map.type(), wisp::Value::Type::Map);
    EXPECT_EQ(map.as_map().size(), 1u);
}
TEST(Value, FindReturnsExistingKeyOrNullptr) {
    wisp::Value map = wisp::Map{{"k", "v"}};
    ASSERT_NE(map.find("k"), nullptr);
    EXPECT_EQ(map.find("k")->as_string(), "v");
    EXPECT_EQ(map.find("missing"), nullptr);
}
TEST(Value, EqualityAcrossDifferentTypesIsFalse) {
    EXPECT_FALSE(wisp::Value(1) == wisp::Value("1"));
    EXPECT_FALSE(wisp::Value() == wisp::Value(false));
}
TEST(Value, CopyIsDeepForListAndMap) {
    wisp::Value original = wisp::List{1, 2};
    wisp::Value copy = original;
    EXPECT_EQ(copy, original);
    EXPECT_NE(&copy.as_list(), &original.as_list());
}

TEST(Json, EmptyListAndMap) {
    wisp::Value v = wisp::Map{{"l", wisp::List{}}, {"m", wisp::Map{}}};
    EXPECT_EQ(wisp::to_json(v), R"({"l":[],"m":{}})");
    auto back = wisp::from_json(wisp::to_json(v));
    ASSERT_TRUE(back);
    EXPECT_EQ(back.value(), v);
}
TEST(Json, TopLevelScalarRoundTrips) {
    EXPECT_EQ(wisp::to_json(wisp::Value(true)), "true");
    EXPECT_EQ(wisp::to_json(wisp::Value()), "null");
    auto back = wisp::from_json("true");
    ASSERT_TRUE(back);
    EXPECT_EQ(back.value(), wisp::Value(true));
}

// ArduinoJson leaves most C0 control bytes raw, which JSON.parse() in the
// browser rejects; every one of them must come out as a unicode escape.
TEST(Json, EscapesEveryControlByte) {
    EXPECT_EQ(wisp::to_json(wisp::Value("\x1b[31mred")), "\"\\u001b[31mred\"");
    std::string all;
    for (int c = 1; c < 0x20; ++c) all.push_back(static_cast<char>(c));
    const std::string json = wisp::to_json(wisp::Value(wisp::Map{{all, wisp::Value(all)}}));
    EXPECT_FALSE(wisp::test::has_control_bytes(json)) << json;
    auto back = wisp::from_json(json);
    ASSERT_TRUE(back) << json;
    const wisp::Value* v = back.value().find(all);
    ASSERT_NE(v, nullptr) << json;
    EXPECT_EQ(v->as_string(), all);

    const std::string nul = wisp::to_json(wisp::Value(std::string("a\0b", 3)));
    EXPECT_FALSE(wisp::test::has_control_bytes(nul)) << nul;
    EXPECT_EQ(nul, "\"a\\u0000b\"");
}

// Strings can hold arbitrary bytes (a log line cut mid-character, an SSID);
// a WebSocket text frame must be valid UTF-8, so bad sequences become
// U+FFFD while valid multi-byte characters pass through unchanged.
TEST(Json, ReplacesInvalidUtf8) {
    const std::string fffd = "\xEF\xBF\xBD";
    EXPECT_EQ(wisp::to_json(wisp::Value("a\xe4\xb8")), "\"a" + fffd + "\"");
    EXPECT_EQ(wisp::to_json(wisp::Value("\xe4\xb8x")), "\"" + fffd + "x\"");
    EXPECT_EQ(wisp::to_json(wisp::Value("\x80")), "\"" + fffd + "\"");
    // Overlong, surrogate, past U+10FFFF and never-valid bytes.
    for (const char* bad : {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf5", "\xff"}) {
        const std::string json = wisp::to_json(wisp::Value(bad));
        EXPECT_TRUE(wisp::test::is_valid_utf8(json)) << json;
        EXPECT_NE(json.find(fffd), std::string::npos) << json;
    }
    // Keys get the same treatment.
    const std::string key_json = wisp::to_json(wisp::Value(wisp::Map{{"k\xe4", wisp::Value(1)}}));
    EXPECT_EQ(key_json, "{\"k" + fffd + "\":1}");

    const std::string good = "中文 \xF0\x9F\x94\xA5 \xC3\xA9 \x7f";
    EXPECT_EQ(wisp::to_json(wisp::Value(good)), "\"" + good + "\"");
}

// Persistence needs the stored bytes back exactly (an SSID in GBK, say),
// so it uses the raw form, which keeps non-UTF-8 bytes as they are.
TEST(Json, RawFormPreservesBytes) {
    const std::string bytes = "\xc4\xe3\xba\xc3 \x01\x1b[0m \xe4\xb8";
    const wisp::Value v(wisp::Map{{"k\xff", wisp::Value(bytes)}});
    const std::string raw = wisp::to_json_raw(v);
    EXPECT_NE(raw.find("\xc4\xe3\xba\xc3"), std::string::npos) << raw;
    EXPECT_FALSE(wisp::test::has_control_bytes(raw)) << raw;
    auto back = wisp::from_json(raw);
    ASSERT_TRUE(back) << raw;
    EXPECT_EQ(back.value(), v);
    // The browser form of the same value is still sanitized.
    EXPECT_TRUE(wisp::test::is_valid_utf8(wisp::to_json(v)));
}

// Fixing up the output must stay linear: a log flood of control bytes
// would otherwise cost seconds per message.
TEST(Json, BrowserSafePassIsLinear) {
    // ArduinoJson strings stop at 64 KiB, so the ~480 KB of control bytes
    // are spread over a list; a quadratic pass would take minutes here.
    const std::string ctl(60000, '\x01');
    const wisp::Value v(wisp::List(8, wisp::Value(ctl)));
    const auto t0 = std::chrono::steady_clock::now();
    const std::string json = wisp::to_json(v);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_EQ(json.size(), 8 * (ctl.size() * 6 + 2) + 7 + 2);
    EXPECT_LT(elapsed, std::chrono::seconds(2));
}
