#include <gtest/gtest.h>

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
