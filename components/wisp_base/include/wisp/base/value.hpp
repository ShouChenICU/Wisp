#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace wisp {

class Value;

// A heterogeneous, ordered-by-key collection of Values, used both as
// Value's "map" variant and as a general property bag (see Properties).
using List = std::vector<Value>;
using Map = std::map<std::string, Value>;

// Alias used where a Map is conceptually a bag of named settings/attributes
// rather than JSON structure.
using Properties = Map;

// A dynamically-typed value: null, bool, int64, double, string, a List of
// Values, or a Map of string to Value. Value has value semantics: copying a
// Value deep-copies any List/Map it holds, so independent copies never
// alias each other's storage.
class Value {
   public:
    enum class Type { Null, Bool, Int, Double, String, List, Map };

    Value();
    Value(bool b);           // NOLINT(google-explicit-constructor)
    Value(int i);            // NOLINT(google-explicit-constructor)
    Value(int64_t i);        // NOLINT(google-explicit-constructor)
    Value(double d);         // NOLINT(google-explicit-constructor)
    Value(const char* s);    // NOLINT(google-explicit-constructor)
    Value(std::string s);    // NOLINT(google-explicit-constructor)
    Value(wisp::List list);  // NOLINT(google-explicit-constructor)
    Value(wisp::Map map);    // NOLINT(google-explicit-constructor)

    Value(const Value& other);
    Value(Value&& other) noexcept = default;
    Value& operator=(const Value& other);
    Value& operator=(Value&& other) noexcept = default;
    ~Value() = default;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }

    // Each accessor below is valid only for the matching Type (and, for
    // as_double(), also for Type::Int); calling it on any other type is a
    // contract violation (asserts in debug builds, returns a default value
    // otherwise).
    bool as_bool() const;
    int64_t as_int() const;
    double as_double() const;
    const std::string& as_string() const;
    const wisp::List& as_list() const;
    const wisp::Map& as_map() const;

    // Returns the value at `key` if this is a Map and it contains `key`,
    // otherwise nullptr.
    const Value* find(std::string_view key) const;

    bool operator==(const Value& other) const;
    bool operator!=(const Value& other) const { return !(*this == other); }

   private:
    using Storage = std::variant<std::monostate, bool, int64_t, double, std::string,
                                 std::shared_ptr<wisp::List>, std::shared_ptr<wisp::Map>>;

    Type type_ = Type::Null;
    Storage storage_;
};

}  // namespace wisp
