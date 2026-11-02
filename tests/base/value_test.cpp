// Value's own value-semantics contract (copy/move), independent of JSON
// round-tripping (see value_json_test.cpp for that).

#include "wisp/base/value.hpp"

#include <gtest/gtest.h>

#include <utility>

using wisp::Value;

TEST(Value, MoveConstructLeavesSourceNull) {
    Value v = wisp::List{1, 2, 3};
    Value moved(std::move(v));

    EXPECT_EQ(moved.type(), Value::Type::List);
    EXPECT_EQ(moved.as_list().size(), 3u);
    // Not just "unspecified": a moved-from Value must report Type::Null,
    // the same as any freshly default-constructed one -- see
    // value.hpp's doc comment on the move constructor for why a
    // defaulted move would leave this observably broken (claiming
    // Type::List while holding a null shared_ptr).
    EXPECT_EQ(v.type(), Value::Type::Null);
    EXPECT_TRUE(v.is_null());
}

TEST(Value, MoveConstructMapLeavesSourceNull) {
    Value v = wisp::Map{{"k", Value("v")}};
    Value moved(std::move(v));

    EXPECT_EQ(moved.type(), Value::Type::Map);
    EXPECT_EQ(moved.as_map().size(), 1u);
    EXPECT_TRUE(v.is_null());
}

TEST(Value, MoveAssignLeavesSourceNull) {
    Value v = wisp::List{1, 2, 3};
    Value target;
    target = std::move(v);

    EXPECT_EQ(target.type(), Value::Type::List);
    EXPECT_EQ(target.as_list().size(), 3u);
    EXPECT_TRUE(v.is_null());
}

TEST(Value, SelfMoveAssignIsSafe) {
    Value v = wisp::List{1, 2, 3};
    Value& self_ref = v;
    v = std::move(self_ref);

    // Self-move-assignment must not corrupt the value (the `this ==
    // &other` guard in operator=(Value&&) exists for exactly this).
    EXPECT_EQ(v.type(), Value::Type::List);
    EXPECT_EQ(v.as_list().size(), 3u);
}

TEST(Value, MoveConstructScalarTypesStayIndependent) {
    Value v(std::string("hello"));
    Value moved(std::move(v));

    EXPECT_EQ(moved.as_string(), "hello");
    EXPECT_TRUE(v.is_null());
}
