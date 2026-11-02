#include "wisp/base/value.hpp"

#include <cassert>
#include <utility>

namespace wisp {

Value::Value() : type_(Type::Null), storage_(std::monostate{}) {}

Value::Value(bool b) : type_(Type::Bool), storage_(b) {}

Value::Value(int i) : type_(Type::Int), storage_(static_cast<int64_t>(i)) {}

Value::Value(int64_t i) : type_(Type::Int), storage_(i) {}

Value::Value(double d) : type_(Type::Double), storage_(d) {}

Value::Value(const char* s) : type_(Type::String), storage_(std::string(s)) {}

Value::Value(std::string s) : type_(Type::String), storage_(std::move(s)) {}

Value::Value(wisp::List list)
    : type_(Type::List), storage_(std::make_shared<wisp::List>(std::move(list))) {}

Value::Value(wisp::Map map)
    : type_(Type::Map), storage_(std::make_shared<wisp::Map>(std::move(map))) {}

Value::Value(const Value& other) : type_(other.type_) {
    switch (other.type_) {
        case Type::List:
            storage_ = std::make_shared<wisp::List>(
                *std::get<std::shared_ptr<wisp::List>>(other.storage_));
            break;
        case Type::Map:
            storage_ =
                std::make_shared<wisp::Map>(*std::get<std::shared_ptr<wisp::Map>>(other.storage_));
            break;
        default:
            storage_ = other.storage_;
            break;
    }
}

Value::Value(Value&& other) noexcept : type_(other.type_), storage_(std::move(other.storage_)) {
    other.type_ = Type::Null;
    other.storage_ = std::monostate{};
}

Value& Value::operator=(Value&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    type_ = other.type_;
    storage_ = std::move(other.storage_);
    other.type_ = Type::Null;
    other.storage_ = std::monostate{};
    return *this;
}

Value& Value::operator=(const Value& other) {
    if (this == &other) {
        return *this;
    }
    type_ = other.type_;
    switch (other.type_) {
        case Type::List:
            storage_ = std::make_shared<wisp::List>(
                *std::get<std::shared_ptr<wisp::List>>(other.storage_));
            break;
        case Type::Map:
            storage_ =
                std::make_shared<wisp::Map>(*std::get<std::shared_ptr<wisp::Map>>(other.storage_));
            break;
        default:
            storage_ = other.storage_;
            break;
    }
    return *this;
}

bool Value::as_bool() const {
    assert(type_ == Type::Bool);
    if (type_ != Type::Bool) {
        return false;
    }
    return std::get<bool>(storage_);
}

int64_t Value::as_int() const {
    assert(type_ == Type::Int);
    if (type_ != Type::Int) {
        return 0;
    }
    return std::get<int64_t>(storage_);
}

double Value::as_double() const {
    if (type_ == Type::Int) {
        return static_cast<double>(std::get<int64_t>(storage_));
    }
    assert(type_ == Type::Double);
    if (type_ != Type::Double) {
        return 0.0;
    }
    return std::get<double>(storage_);
}

const std::string& Value::as_string() const {
    static const std::string kEmpty;
    assert(type_ == Type::String);
    if (type_ != Type::String) {
        return kEmpty;
    }
    return std::get<std::string>(storage_);
}

const wisp::List& Value::as_list() const {
    static const wisp::List kEmpty;
    assert(type_ == Type::List);
    if (type_ != Type::List) {
        return kEmpty;
    }
    return *std::get<std::shared_ptr<wisp::List>>(storage_);
}

const wisp::Map& Value::as_map() const {
    static const wisp::Map kEmpty;
    assert(type_ == Type::Map);
    if (type_ != Type::Map) {
        return kEmpty;
    }
    return *std::get<std::shared_ptr<wisp::Map>>(storage_);
}

const Value* Value::find(std::string_view key) const {
    if (type_ != Type::Map) {
        return nullptr;
    }
    const auto& map = *std::get<std::shared_ptr<wisp::Map>>(storage_);
    auto it = map.find(std::string(key));
    if (it == map.end()) {
        return nullptr;
    }
    return &it->second;
}

bool Value::operator==(const Value& other) const {
    if (type_ != other.type_) {
        return false;
    }
    switch (type_) {
        case Type::Null:
            return true;
        case Type::Bool:
            return std::get<bool>(storage_) == std::get<bool>(other.storage_);
        case Type::Int:
            return std::get<int64_t>(storage_) == std::get<int64_t>(other.storage_);
        case Type::Double:
            return std::get<double>(storage_) == std::get<double>(other.storage_);
        case Type::String:
            return std::get<std::string>(storage_) == std::get<std::string>(other.storage_);
        case Type::List:
            return *std::get<std::shared_ptr<wisp::List>>(storage_) ==
                   *std::get<std::shared_ptr<wisp::List>>(other.storage_);
        case Type::Map:
            return *std::get<std::shared_ptr<wisp::Map>>(storage_) ==
                   *std::get<std::shared_ptr<wisp::Map>>(other.storage_);
    }
    return false;
}

}  // namespace wisp
