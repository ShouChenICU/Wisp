// The compiled-in widget whitelist (scene-runtime-core spec 4.3-4.5) and
// the Markdown it exports as docs/scene-api.md.

#include "wisp/scene/schema.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

#include "wisp/scene/patch_batch.hpp"

namespace wisp::scene {

namespace {

using SV = std::string_view;

constexpr SV kSizeWords[] = {"fill", "content"};
constexpr SV kPadWords[] = {"safe"};
constexpr SV kColorTokens[] = {"accent", "text", "muted", "surface", "bg", "danger", "ok", "warn"};
constexpr SV kFontSizes[] = {"xs", "sm", "md", "lg", "xl", "xxl"};
constexpr SV kAligns[] = {"start", "center", "end"};
constexpr SV kJustifies[] = {"start", "center", "end", "between", "around", "evenly"};
constexpr SV kDirections[] = {"v", "h"};
constexpr SV kChartTypes[] = {"line", "bar"};
// The built-in icon set; the UI maps each to its LV_SYMBOL_*.
constexpr SV kIcons[] = {"home",   "settings", "wifi",    "battery", "bell",  "play",  "pause",
                         "stop",   "prev",     "next",    "plus",    "minus", "ok",    "close",
                         "trash",  "edit",     "refresh", "left",    "right", "up",    "down",
                         "upload", "download", "save",    "list",    "image", "audio", "warning"};

constexpr std::span<const SV> none{};

// Indexed by Prop.
constexpr PropSpec kProps[] = {
    {Prop::W, "w", PropType::Size, true, kSizeWords},
    {Prop::H, "h", PropType::Size, true, kSizeWords},
    {Prop::Grow, "grow", PropType::Int, true, none},
    {Prop::Pad, "pad", PropType::Pad, true, kPadWords},
    {Prop::Gap, "gap", PropType::Int, true, none},
    {Prop::Align, "align", PropType::Align, true, kAligns},
    {Prop::Justify, "justify", PropType::Justify, true, kJustifies},
    {Prop::Bg, "bg", PropType::Color, true, kColorTokens},
    {Prop::Fg, "fg", PropType::Color, true, kColorTokens},
    {Prop::Radius, "radius", PropType::Int, true, none},
    {Prop::Border, "border", PropType::Int, true, none},
    {Prop::Font, "font", PropType::Font, true, kFontSizes},
    {Prop::Hidden, "hidden", PropType::Bool, true, none},
    {Prop::Opa, "opa", PropType::Number, true, none},
    {Prop::Ref, "ref", PropType::String, false, none},
    {Prop::Text, "text", PropType::String, true, none},
    {Prop::Placeholder, "placeholder", PropType::String, true, none},
    {Prop::Password, "password", PropType::Bool, true, none},
    {Prop::MaxLen, "max_len", PropType::Int, true, none},
    {Prop::Name, "name", PropType::Enum, true, kIcons},
    {Prop::Src, "src", PropType::String, false, none},
    {Prop::Value, "value", PropType::Number, true, none},
    {Prop::Min, "min", PropType::Number, true, none},
    {Prop::Max, "max", PropType::Number, true, none},
    {Prop::Checked, "checked", PropType::Bool, true, none},
    {Prop::Cols, "cols", PropType::Int, false, none},
    {Prop::Direction, "direction", PropType::Enum, false, kDirections},
    {Prop::Type, "type", PropType::Enum, false, kChartTypes},
    {Prop::Series, "series", PropType::Series, true, none},
    {Prop::Items, "items", PropType::Items, true, none},
    {Prop::Row, "row", PropType::Fn, false, none},
    {Prop::RowH, "row_h", PropType::Int, true, none},
    {Prop::Draw, "draw", PropType::Fn, false, none},
    {Prop::Width, "width", PropType::Int, true, none},
    {Prop::Wrap, "wrap", PropType::Bool, true, none},
    {Prop::OnClick, "on_click", PropType::Fn, false, none},
    {Prop::OnLongPress, "on_long_press", PropType::Fn, false, none},
    {Prop::OnChange, "on_change", PropType::Fn, false, none},
};
static_assert(std::size(kProps) == kPropCount);

constexpr bool props_in_order() {
    for (size_t i = 0; i < kPropCount; ++i)
        if (static_cast<size_t>(kProps[i].prop) != i) return false;
    return true;
}
static_assert(props_in_order());

// Indexed by Prop; the "expects ..." part of type errors.
constexpr SV kExpects[] = {
    "0 to 4096 pixels, \"fill\" or \"content\"",  // w
    "0 to 4096 pixels, \"fill\" or \"content\"",  // h
    "integer 0 to 255",                           // grow
    "0 to 512 pixels or \"safe\"",                // pad
    "integer 0 to 512",                           // gap
    "one of start, center, end",
    "one of start, center, end, between, around, evenly",
    "\"#RRGGBB\" or one of accent, text, muted, surface, bg, danger, ok, warn",
    "\"#RRGGBB\" or one of accent, text, muted, surface, bg, danger, ok, warn",
    "integer 0 to 4096",                               // radius
    "integer 0 to 64",                                 // border
    "one of xs, sm, md, lg, xl, xxl",                  // font
    "boolean",                                         // hidden
    "number from 0 to 1",                              // opa
    "a name of 1 to 64 bytes",                         // ref
    "string",                                          // text
    "string",                                          // placeholder
    "boolean",                                         // password
    "integer 0 to 65535",                              // max_len
    "an icon name",                                    // name
    "a path assets/<name>.png",                        // src
    "number",                                          // value
    "number",                                          // min
    "number",                                          // max
    "boolean",                                         // checked
    "integer 1 to 16",                                 // cols
    "one of v, h",                                     // direction
    "one of line, bar",                                // type
    "a table of up to 4 tables of up to 128 numbers",  // series
    "a table",                                         // items
    "function",                                        // row
    "integer 1 to 4096",                               // row_h
    "function",                                        // draw
    "integer 0 to 512",                                // width
    "boolean",                                         // wrap
    "function",                                        // on_click
    "function",                                        // on_long_press
    "function",                                        // on_change
};
static_assert(std::size(kExpects) == kPropCount);

using P = Prop;

#define WISP_COMMON_PROPS                                                                        \
    P::W, P::H, P::Grow, P::Pad, P::Bg, P::Fg, P::Radius, P::Border, P::Font, P::Hidden, P::Opa, \
        P::Ref, P::OnClick, P::OnLongPress
#define WISP_LAYOUT_PROPS WISP_COMMON_PROPS, P::Gap, P::Align, P::Justify

constexpr Prop kLayoutProps[] = {WISP_LAYOUT_PROPS};
constexpr Prop kGridProps[] = {WISP_LAYOUT_PROPS, P::Cols};
constexpr Prop kScrollProps[] = {WISP_LAYOUT_PROPS, P::Direction};
constexpr Prop kCommonProps[] = {WISP_COMMON_PROPS};
constexpr Prop kLabelProps[] = {WISP_COMMON_PROPS, P::Text, P::Wrap};
constexpr Prop kIconProps[] = {WISP_COMMON_PROPS, P::Name};
constexpr Prop kImageProps[] = {WISP_COMMON_PROPS, P::Src};
constexpr Prop kButtonProps[] = {WISP_COMMON_PROPS, P::Text};
constexpr Prop kCheckboxProps[] = {WISP_COMMON_PROPS, P::Text, P::Checked, P::OnChange};
constexpr Prop kSwitchProps[] = {WISP_COMMON_PROPS, P::Checked, P::OnChange};
constexpr Prop kSliderProps[] = {WISP_COMMON_PROPS, P::Value, P::Min, P::Max, P::OnChange};
constexpr Prop kInputProps[] = {WISP_COMMON_PROPS, P::Text,   P::Placeholder,
                                P::Password,       P::MaxLen, P::OnChange};
constexpr Prop kBarProps[] = {WISP_COMMON_PROPS, P::Value, P::Min, P::Max};
constexpr Prop kArcProps[] = {WISP_COMMON_PROPS, P::Value, P::Min, P::Max, P::Width};
constexpr Prop kChartProps[] = {WISP_COMMON_PROPS, P::Type, P::Series, P::Min, P::Max};
constexpr Prop kListProps[] = {WISP_COMMON_PROPS, P::Items, P::Row, P::RowH};
constexpr Prop kCanvasProps[] = {WISP_COMMON_PROPS, P::Draw};

#undef WISP_LAYOUT_PROPS
#undef WISP_COMMON_PROPS

// Indexed by Widget.
constexpr WidgetSpec kWidgets[] = {
    {Widget::Screen, "screen", true, kLayoutProps},
    {Widget::Column, "column", true, kLayoutProps},
    {Widget::Row, "row", true, kLayoutProps},
    {Widget::Stack, "stack", true, kLayoutProps},
    {Widget::Grid, "grid", true, kGridProps},
    {Widget::Spacer, "spacer", false, kCommonProps},
    {Widget::Scroll, "scroll", true, kScrollProps},
    {Widget::Label, "label", false, kLabelProps},
    {Widget::Icon, "icon", false, kIconProps},
    {Widget::Image, "image", false, kImageProps},
    {Widget::Button, "button", true, kButtonProps},
    {Widget::Checkbox, "checkbox", false, kCheckboxProps},
    {Widget::Switch, "switch", false, kSwitchProps},
    {Widget::Slider, "slider", false, kSliderProps},
    {Widget::Input, "input", false, kInputProps},
    {Widget::Bar, "bar", false, kBarProps},
    {Widget::Arc, "arc", false, kArcProps},
    {Widget::Chart, "chart", false, kChartProps},
    {Widget::List, "list", false, kListProps},
    {Widget::Canvas, "canvas", false, kCanvasProps},
};
static_assert(std::size(kWidgets) == kWidgetCount);

constexpr bool widgets_in_order() {
    for (size_t i = 0; i < kWidgetCount; ++i)
        if (static_cast<size_t>(kWidgets[i].widget) != i) return false;
    return true;
}
static_assert(widgets_in_order());

// Names longer than this are compared by their first kMaxCompared bytes,
// which keeps the distance table on the stack.
constexpr size_t kMaxCompared = 64;

size_t distance(SV a, SV b) {
    a = a.substr(0, kMaxCompared);
    b = b.substr(0, kMaxCompared);
    std::array<size_t, kMaxCompared + 1> row{};
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diag = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            size_t up = row[j];
            size_t cost = a[i - 1] == b[j - 1] ? 0 : 1;
            row[j] = std::min({row[j] + 1, row[j - 1] + 1, diag + cost});
            diag = up;
        }
    }
    return row[b.size()];
}

const char* type_name(PropType t) {
    switch (t) {
        case PropType::Size:
            return "size";
        case PropType::Int:
            return "integer";
        case PropType::Number:
            return "number";
        case PropType::String:
            return "string";
        case PropType::Bool:
            return "boolean";
        case PropType::Color:
            return "color";
        case PropType::Font:
            return "font";
        case PropType::Align:
            return "align";
        case PropType::Justify:
            return "justify";
        case PropType::Pad:
            return "pad";
        case PropType::Enum:
            return "enum";
        case PropType::Fn:
            return "function";
        case PropType::Items:
            return "items";
        case PropType::Series:
            return "series";
    }
    return "?";
}

void append_list(std::string& out, std::span<const SV> words) {
    for (size_t i = 0; i < words.size(); ++i) {
        if (i) out += ", ";
        out += '`';
        out += words[i];
        out += '`';
    }
}

// Markdown would take "<name>" for an HTML tag.
void append_escaped(std::string& out, SV text) {
    for (char c : text) {
        if (c == '<')
            out += "&lt;";
        else
            out += c;
    }
}

}  // namespace

const WidgetSpec* find_widget(std::string_view name) {
    for (const auto& w : kWidgets)
        if (w.name == name) return &w;
    return nullptr;
}

const WidgetSpec& widget_spec(Widget w) { return kWidgets[static_cast<size_t>(w)]; }

const PropSpec& prop_spec(Prop p) { return kProps[static_cast<size_t>(p)]; }

IntRange prop_range(Prop p) {
    switch (p) {
        case Prop::W:
        case Prop::H:
        case Prop::Radius:
            return {0, 4096};
        case Prop::RowH:
            return {1, 4096};
        case Prop::Pad:
        case Prop::Gap:
        case Prop::Width:
            return {0, 512};
        case Prop::Grow:
            return {0, 255};
        case Prop::Border:
            return {0, 64};
        case Prop::MaxLen:
            return {0, 65535};
        case Prop::Cols:
            return {1, 16};
        default:
            return {INT32_MIN, INT32_MAX};
    }
}

std::string_view prop_expects(Prop p) { return kExpects[static_cast<size_t>(p)]; }

std::string nearest_name(std::string_view wrong, std::span<const std::string_view> candidates) {
    std::string_view best;
    size_t best_distance = SIZE_MAX;
    for (SV c : candidates) {
        size_t d = distance(wrong, c);
        if (d < best_distance || (d == best_distance && c < best)) {
            best = c;
            best_distance = d;
        }
    }
    return std::string(best);
}

std::string schema_markdown() {
    std::string out;
    out +=
        "# Scene UI API\n"
        "\n"
        "<!-- Generated by schema_markdown() in components/wisp_scene/src/schema.cpp; do not "
        "edit.\n"
        "     schema_test's DocIsUpToDate fails when this file is stale and writes the new text "
        "to\n"
        "     build/sim/scene-api.md. -->\n"
        "\n"
        "A scene's `main.lua` returns its widget tree, built with the constructors in the "
        "global\n"
        "`ui` table (scene-runtime-core spec 4). String keys are properties, array entries are\n"
        "children:\n"
        "\n"
        "```lua\n"
        "local count = state(0)\n"
        "return ui.screen {\n"
        "  ui.column { pad = \"safe\", gap = 12, align = \"center\",\n"
        "    ui.label { text = bind(count, function(v) return \"clicked \" .. v end) },\n"
        "    ui.button { text = \"+1\", on_click = function() count:set(count:get() + 1) end "
        "},\n"
        "  },\n"
        "}\n"
        "```\n"
        "\n"
        "Rules:\n"
        "\n"
        "- The root must be `ui.screen`. It always fills the display and is padded to at least "
        "the\n"
        "  safe area, so content stays visible on round and rounded screens.\n";
    char line[320];
    std::snprintf(line, sizeof(line),
                  "- At most %zu widgets per tree. A string value is at most %zu KB.\n", kMaxNodes,
                  kMaxPropStringBytes / 1024);
    out += line;
    std::snprintf(line, sizeof(line),
                  "- One update (the tree, or what one handler changes) is at most %zu MB, "
                  "counting\n"
                  "  %zu bytes per widget, property and change plus every string value: a large\n"
                  "  string shown by many widgets counts once per widget (\"ui update too "
                  "large\").\n",
                  kMaxBatchBytes / (1024 * 1024), kOpOverheadBytes);
    out += line;
    out +=
        "- A widget can be the child of only one other widget; `ui.screen` only the root.\n"
        "- Unknown widgets and properties and values of the wrong type are errors at the line\n"
        "  that built the widget.\n"
        "- A *bindable* property also takes a `state`, `computed` or `bind`; the widget follows\n"
        "  its value. Every value it takes is checked like a plain one, at the line that built\n"
        "  the widget.\n"
        "\n"
        "## Value types\n"
        "\n"
        "| Type | Values |\n"
        "|---|---|\n"
        "| size | integer pixels, `\"fill\"` or `\"content\"` |\n"
        "| integer | an integer (`120.0` counts as one) |\n"
        "| number | an integer or a float |\n"
        "| string | a string or a number |\n"
        "| boolean | `true` or `false` |\n"
        "| color | `\"#RRGGBB\"` or a theme token: ";
    append_list(out, kColorTokens);
    out += " |\n| font | ";
    append_list(out, kFontSizes);
    out += " |\n| align | ";
    append_list(out, kAligns);
    out += " |\n| justify | ";
    append_list(out, kJustifies);
    out +=
        " |\n"
        "| pad | integer pixels or `\"safe\"` (the screen's safe area) |\n"
        "| enum | one of the listed words |\n"
        "| function | a function |\n"
        "| items | a table (an array), or a state holding one |\n"
        "| series | up to 4 tables of up to 128 numbers each |\n"
        "\n"
        "## Properties\n"
        "\n"
        "| Property | Type | Bindable | Values |\n"
        "|---|---|---|---|\n";
    for (const auto& p : kProps) {
        out += "| `";
        out += p.name;
        out += "` | ";
        out += type_name(p.type);
        out += p.bindable ? " | yes | " : " | no | ";
        if (p.type == PropType::Enum)
            append_list(out, p.choices);
        else
            append_escaped(out, prop_expects(p.prop));
        out += " |\n";
    }
    out +=
        "\n"
        "`on_click` and `on_long_press` take no arguments; `on_change(value)` gets the new "
        "text,\n"
        "value or checked state. `row(item, index)` returns the widget for one list row and is\n"
        "only called for visible rows. `draw(g)` draws a canvas.\n"
        "\n"
        "## Widgets\n"
        "\n"
        "Every widget takes the common properties: ";
    for (size_t i = 0; i < std::size(kCommonProps); ++i) {
        if (i) out += ", ";
        out += '`';
        out += prop_spec(kCommonProps[i]).name;
        out += '`';
    }
    out +=
        ".\n"
        "\n"
        "| Widget | Children | Properties besides the common ones |\n"
        "|---|---|---|\n";
    for (const auto& w : kWidgets) {
        out += "| `ui.";
        out += w.name;
        out += w.children ? "` | yes | " : "` | no | ";
        bool any = false;
        for (Prop p : w.props) {
            if (std::find(std::begin(kCommonProps), std::end(kCommonProps), p) !=
                std::end(kCommonProps))
                continue;
            if (any) out += ", ";
            any = true;
            out += '`';
            out += prop_spec(p).name;
            out += '`';
            if (w.widget == Widget::List && p == Prop::Row) out += " (required)";
        }
        if (!any) out += "-";
        out += " |\n";
    }
    out +=
        "\n"
        "## Helpers\n"
        "\n"
        "- `ui.show_if(s, widget)` returns `widget`, shown while `s` (a state, computed, bind or\n"
        "  boolean) is truthy and hidden otherwise. The widget is never rebuilt.\n"
        "- `ui.ref(name)` returns a handle to the widget with `ref = name` in the current tree,\n"
        "  looked up when a method is called:\n"
        "  - `h:value()`: the current text of an `input`, value of a `slider`, or checked state "
        "of\n"
        "    a `checkbox` or `switch`.\n"
        "  - `h:scroll_to(i)`: scrolls a `list` or `scroll` to its `i`-th item (from 1).\n"
        "  - `h:focus()`: focuses an `input` and opens the on-screen keyboard.\n";
    return out;
}

}  // namespace wisp::scene
