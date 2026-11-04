#include "wisp/hal/draw_buf_policy.hpp"

namespace wisp::hal {

namespace {

const Value* find_cap(const Properties& caps, const char* key) {
    auto it = caps.find(key);
    return it == caps.end() ? nullptr : &it->second;
}

}  // namespace

DrawBufPolicy draw_buf_policy(const Properties& caps) {
    DrawBufPolicy p;

    int64_t height = 0;
    if (const Value* h = find_cap(caps, "height"); h != nullptr && h->type() == Value::Type::Int) {
        height = h->as_int();
    }

    if (const Value* v = find_cap(caps, "draw_buf_lines"); v != nullptr) {
        if (v->type() == Value::Type::Int && v->as_int() > 0) {
            if (height > 0 && v->as_int() > height) {
                p.warnings.push_back("draw_buf_lines exceeds the display height; clamped to it");
                p.lines = static_cast<uint32_t>(height);
            } else {
                p.lines = static_cast<uint32_t>(v->as_int());
            }
        } else {
            p.warnings.push_back("draw_buf_lines must be a positive integer; using the default");
        }
    }
    // A display shorter than the default needs no taller buffer.
    if (height > 0 && p.lines > height) p.lines = static_cast<uint32_t>(height);

    if (const Value* v = find_cap(caps, "draw_buf_mem"); v != nullptr) {
        if (v->type() == Value::Type::String && v->as_string() == "large") {
            p.mem = DrawBufMem::Large;
        } else if (!(v->type() == Value::Type::String && v->as_string() == "dma")) {
            p.warnings.push_back("draw_buf_mem must be \"dma\" or \"large\"; using \"dma\"");
        }
    }

    if (const Value* v = find_cap(caps, "render_mode"); v != nullptr) {
        if (!(v->type() == Value::Type::String && v->as_string() == "partial")) {
            p.warnings.push_back("render_mode: only \"partial\" is implemented; using it");
        }
    }
    return p;
}

}  // namespace wisp::hal
