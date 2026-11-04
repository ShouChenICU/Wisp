#include "wisp/app/ram_budget.hpp"

namespace wisp::app {

namespace {

int64_t read_int(const Value& map, const char* key) {
    const Value* v = map.find(key);
    return v != nullptr && v->type() == Value::Type::Int ? v->as_int() : -1;
}

}  // namespace

InternalRam internal_ram(const Value& heap_info) {
    return {read_int(heap_info, "internal_free"), read_int(heap_info, "internal_largest")};
}

bool internal_ram_low(const InternalRam& ram) {
    if (ram.free < 0) return false;
    if (ram.free < kIdleInternalFreeMin) return true;
    return ram.largest >= 0 && ram.largest < kIdleInternalLargestMin;
}

}  // namespace wisp::app
