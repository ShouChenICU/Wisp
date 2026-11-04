#include "wisp/scene/theme.hpp"

#include <cstdint>

#include "wisp/ui/ui_runtime.hpp"

namespace wisp::scene {

namespace {

struct Token {
    std::string_view name;
    uint32_t rgb;
};

constexpr Token kTokens[] = {
    {"accent", 0x4F9DFF}, {"text", 0xFFFFFF},   {"muted", 0x8E8E93}, {"surface", 0x1C1C1E},
    {"bg", 0x000000},     {"danger", 0xFF453A}, {"ok", 0x32D74B},    {"warn", 0xFFD60A},
};

constexpr uint32_t kTextRgb = 0xFFFFFF;

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

uint32_t theme_rgb(std::string_view token_or_hex) {
    for (const auto& t : kTokens)
        if (t.name == token_or_hex) return t.rgb;
    if (token_or_hex.size() == 7 && token_or_hex[0] == '#') {
        uint32_t rgb = 0;
        for (size_t i = 1; i < 7; ++i) {
            int d = hex_digit(token_or_hex[i]);
            if (d < 0) return kTextRgb;
            rgb = (rgb << 4) | static_cast<uint32_t>(d);
        }
        return rgb;
    }
    return kTextRgb;
}

lv_color_t theme_color(std::string_view token_or_hex) {
    return lv_color_hex(theme_rgb(token_or_hex));
}

const lv_font_t* theme_font(std::string_view size) {
    if (size == "lg") return ui::font(24);
    if (size == "xl") return ui::font(32);
    if (size == "xxl") return ui::font(48);
    return ui::font(16);
}

}  // namespace wisp::scene
