#include "wisp/scene/theme.hpp"

#include <cstdint>

#include "wisp/ui/theme.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::scene {

namespace {

struct Token {
    std::string_view name;
    uint32_t rgb;
};

// The device theme's colours under the names scenes use. A scene's
// surface is the theme's second step, so a card stands off the page.
constexpr Token kTokens[] = {
    {"accent", ui::theme::kAccent}, {"text", ui::theme::kText},
    {"muted", ui::theme::kTextDim}, {"surface", ui::theme::kSurface2},
    {"bg", ui::theme::kBg},         {"danger", ui::theme::kErr},
    {"ok", ui::theme::kOk},         {"warn", ui::theme::kWarn},
};

constexpr uint32_t kTextRgb = ui::theme::kText;

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

int theme_font_px(std::string_view token) {
    if (token == "lg") return 24;
    if (token == "xl") return 32;
    if (token == "xxl") return 48;
    return 16;
}

const lv_font_t* theme_font(std::string_view token) { return ui::font(theme_font_px(token)); }

const lv_font_t* theme_font(int px) { return ui::font(px); }

}  // namespace wisp::scene
