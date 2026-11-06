// The compiled-in widget whitelist (scene-runtime-core spec 4.3-4.5) and
// the Markdown it exports as docs/scene-api.md, which also describes
// scene.json, the hooks and the wisp.* APIs of the scene-device spec.

#include "wisp/scene/schema.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>

#include "wisp/core/notification_service.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/sensor_service.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/scene_schedule.hpp"
#include "wisp/scene/theme.hpp"
#include "wisp/scene/vm.hpp"
#include "wisp/ui/system_layer.hpp"

namespace wisp::scene {

namespace {

using SV = std::string_view;

constexpr SV kSizeWords[] = {"fill", "content"};
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
    {Prop::Pad, "pad", PropType::Pad, true, none},
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
    "0 to 4096 pixels, \"fill\" or \"content\"",                       // w
    "0 to 4096 pixels, \"fill\" or \"content\"",                       // h
    "integer 0 to 255",                                                // grow
    "0 to 512 pixels or {top, right, bottom, left} of 0 to 512 each",  // pad
    "integer 0 to 512",                                                // gap
    "one of start, center, end",
    "one of start, center, end, between, around, evenly",
    "\"#RRGGBB\" or one of accent, text, muted, surface, bg, danger, ok, warn",
    "\"#RRGGBB\" or one of accent, text, muted, surface, bg, danger, ok, warn",
    "integer 0 to 4096",                                                 // radius
    "integer 0 to 64",                                                   // border
    "one of xs, sm, md, lg, xl, xxl, or an integer 16 to 128 (pixels)",  // font
    "boolean",                                                           // hidden
    "number from 0 to 1",                                                // opa
    "a name of 1 to 64 bytes",                                           // ref
    "string",                                                            // text
    "string",                                                            // placeholder
    "boolean",                                                           // password
    "integer 0 to 65535",                                                // max_len
    "an icon name",                                                      // name
    "a path assets/<name>.png, .jpg or .jpeg",                           // src
    "number",                                                            // value
    "number",                                                            // min
    "number",                                                            // max
    "boolean",                                                           // checked
    "integer 1 to 16",                                                   // cols
    "one of v, h",                                                       // direction
    "one of line, bar",                                                  // type
    "a table of up to 4 tables of up to 128 numbers",                    // series
    "a table",                                                           // items
    "function",                                                          // row
    "integer 1 to 4096",                                                 // row_h
    "function",                                                          // draw
    "integer 0 to 512",                                                  // width
    "boolean",                                                           // wrap
    "function",                                                          // on_click
    "function",                                                          // on_long_press
    "function",                                                          // on_change
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

std::string num(uint64_t v) { return std::to_string(v); }

// The manifest, the hooks and the device-side APIs (scene-device spec 3,
// 4.3, 5.1, 6.2, 7): the numbers come from the code that enforces them.
void append_device_api(std::string& out) {
    using core::NotificationService;
    const core::SceneFilesConfig files;
    out +=
        "\n"
        "## scene.json\n"
        "\n"
        "Every scene package has a `scene.json` at its root:\n"
        "\n"
        "```json\n"
        "{\"id\": \"pomodoro\", \"name\": \"番茄钟\", \"version\": \"1.0.0\", "
        "\"schema_version\": 1,\n"
        " \"background\": true, \"files\": \"auto\",\n"
        " \"permissions\": {\"http\": [\"api.example.com\"]}}\n"
        "```\n"
        "\n"
        "Required: `id`, `name`, `version` and `schema_version`; without any of them the\n"
        "install is refused.\n"
        "\n"
        "- `id`: 1 to 32 bytes of `a-z`, `0-9`, `_` and `-`; also the scene's directory.\n"
        "- `name`: 1 to 32 bytes of UTF-8. `version`: a semantic version, at most 64 bytes.\n"
        "- `entry` (default `main.lua`) and `icon` (a file under `assets/`) are optional.\n"
        "- `schema_version`: an integer from 1 to 1000, the version of the structure of the\n"
        "  scene's data (see `scene.migrate` below).\n"
        "- `permissions.http`: the hosts `wisp.http` may reach, at most 16.\n"
        "- `background` (default `false`): going back sends the scene to the background\n"
        "  instead of closing it (see Background scenes).\n"
        "- `files` (default `\"auto\"`): where `wisp.files` keeps the scene's files: `\"auto\"`,\n"
        "  `\"sd\"` or `\"int\"` (see wisp.files). Any other value refuses the install.\n"
        "\n"
        "Unknown fields are ignored; nothing may nest deeper than 4 levels.\n"
        "\n"
        "## Scene hooks\n"
        "\n"
        "A scene sets these functions in the global `scene` table; each one that is set runs\n"
        "as a message of its own, within the usual 200 ms:\n"
        "\n"
        "- `scene.on_show()` and `scene.on_hide()`: the scene's view appears or goes (it opens,\n"
        "  the screen turns on or off, it comes back from or goes to the background).\n"
        "- `scene.on_close()`: the scene is closing; its store is flushed after it.\n"
        "- `scene.on_state(state)`: the scene is now `\"foreground\"` or `\"background\"`. It\n"
        "  is called when the scene first opens (with the state it opened in) and on every\n"
        "  move between the two, never on a close.\n"
        "- `scene.on_notification(id)`: the user tapped the scene's notification `id`. It is\n"
        "  called once the scene is in front: one in the background comes forward first, and\n"
        "  one that is not running is opened first (`wisp.scene.launch()` is then\n"
        "  `\"notification\"`).\n";
    out += "- `scene.migrate(old_version)`: see Data migration; it may run for " + num(kMigrateMs) +
           " ms.\n";
    out +=
        "\n"
        "## wisp.scene\n"
        "\n"
        "- `wisp.scene.close()`: goes back, like the back gesture: a scene that declares\n"
        "  `background` goes to the background, any other closes. Opening another scene does\n"
        "  the same to the scene in front.\n"
        "- `wisp.scene.exit()`: always closes the scene completely (`on_hide`, `on_close`).\n"
        "- `wisp.scene.args()`: the table of arguments the scene was opened with (`{}` for\n"
        "  none).\n"
        "- `wisp.scene.state()`: `\"foreground\"` or `\"background\"`.\n"
        "- `wisp.scene.launch()`: why the scene was started: `\"user\"`, `\"boot\"` (restored\n"
        "  in the background after a restart) or `\"notification\"`.\n"
        "\n"
        "## Background scenes\n"
        "\n"
        "A scene whose `scene.json` declares `\"background\": true` keeps running after the\n"
        "user goes back or opens another scene: `scene.on_hide()`, then its view goes and\n"
        "`scene.on_state(\"background\")` runs. Its VM, store, timers and widget tree\n"
        "stay; changes to its state still update the tree, which is shown again when the\n"
        "scene comes back to the front (from the scene list, a row of the pull-down panel or\n"
        "a tap on one of its notifications), and then `scene.on_show()` and\n"
        "`scene.on_state(\"foreground\")` run. Closing it from the panel (its ×), from the\n"
        "admin page or with `wisp.scene.exit()` stops it completely.\n"
        "\n"
        "- Timers and `wisp.device.on` events keep firing in the background and while the\n"
        "  screen is off, without pausing and without catching up. `wisp.http`, `wisp.store`,\n"
        "  `wisp.files`, `wisp.notify`, `wisp.log` and `wisp.device.info()` work as in front.\n"
        "- `wisp.sensor.subscribe`, `wisp.sensor.on` and `wisp.power.keep_screen_on` return\n"
        "  `nil, \"not_in_background\"`; sensor subscriptions made in front pause in the\n"
        "  background and resume in front. The keyboard and confirmation dialogs are not\n"
        "  available. A scene in the background holds no wake lock.\n";
    out += "- At most " + num(kMaxBackground) +
           " scenes run in the background. All scenes' Lua memory comes from one\n"
           "  budget of " +
           num(kVmBudgetBytes / (1024 * 1024)) +
           " MB; each scene may still use at most 2 MB, and the scene in front can always\n"
           "  get its 2 MB. When a scene would go past the count, or the scene in front needs\n"
           "  the memory, the scene in the background longest away from the front is closed\n"
           "  (`on_close` runs and its data is saved) and the user gets a notification saying\n"
           "  so.\n";
    out +=
        "- Scenes running in the background when the device powers off or restarts are\n"
        "  started again in the background once it has booted: the entry file runs and the\n"
        "  tree is built, with no view and no `on_show`; `wisp.scene.launch()` is `\"boot\"`.\n"
        "- An error in the background stops the scene (no error panel) and posts a system\n"
        "  notification.\n"
        "\n"
        "## wisp.notify\n"
        "\n"
        "```lua\n"
        "local id = wisp.notify{ title = \"番茄钟\", body = \"25 分钟到了\",\n"
        "                        sound = \"alarm\", wake = true, id = \"pomo\" }\n"
        "wisp.notify_cancel(id)\n"
        "```\n"
        "\n";
    out += "- `title`: 1 to " + num(NotificationService::kTitleMax) + " bytes; `body`: 0 to " +
           num(NotificationService::kBodyMax) + " bytes (optional).\n";
    out +=
        "- `sound`: `\"none\"` (default), `\"tone\"` (one short tone) or `\"alarm\"` (rings until\n"
        "  the screen is touched, at most 30 seconds). The volume is the system's\n"
        "  notification volume.\n"
        "- `wake` (default `false`): turns the screen on if it is off.\n";
    out += "- `id`: optional, 1 to " + num(NotificationService::kIdMax) +
           " bytes of `a-z`, `0-9`, `_` and `-`. Posting the same id again\n"
           "  replaces the earlier notification.\n";
    out +=
        "\n"
        "`wisp.notify` returns the notification's id (the one given, or one made up), or\n"
        "`nil` and a code: `rate_limited`";
    out += " (more than " + num(NotificationService::kPerMinute) +
           " posts by the scene in any minute) or\n"
           "`unavailable`. A bad field is a Lua error at the call. "
           "`wisp.notify_cancel(id)` removes the\n"
           "scene's unread notification `id` and returns whether there was one.\n"
           "\n";
    out += "A notification from a scene in the background shows a banner for " +
           num(ui::SystemLayer::kBannerMs / 1000) +
           " seconds and stays in\n"
           "the pull-down panel's list until the user taps or dismisses it; each scene keeps at\n"
           "most " +
           num(NotificationService::kUnreadPerScene) +
           " unread, the oldest dropped first. A notification posted by the scene in front only\n"
           "shows the banner. A tap on either calls `scene.on_notification(id)` (see Scene\n"
           "hooks).\n";
    out +=
        "\n"
        "## wisp.power\n"
        "\n"
        "- `wisp.power.keep_screen_on(on)`: `true` keeps the screen on and bright while the\n"
        "  scene is in front (a `screen` wake lock), `false` lets it dim and turn off again.\n"
        "  Returns `true`, or `nil` and `not_in_background` or `unavailable`. The lock goes\n"
        "  by itself when the scene leaves the front or closes, and a short press of the\n"
        "  power key still turns the screen off regardless. It brings no reminder: the\n"
        "  scene is in front, so the user can already see the lock.\n"
        "\n"
        "## wisp.sensor\n"
        "\n"
        "For the scene in front only.\n"
        "\n"
        "```lua\n"
        "local h = wisp.sensor.subscribe(\"accel\", { rate = 25 }, function(samples)\n"
        "  -- samples = { {x =, y =, z =, t =}, ... }\n"
        "end)\n"
        "h:stop()\n"
        "local g = wisp.sensor.on(\"shake\", function() end)  -- or \"flip\"; g:stop()\n"
        "wisp.sensor.available()                             -- { accel = true, gyro = true }\n"
        "```\n"
        "\n";
    out +=
        "- `wisp.sensor.subscribe(kind, opts, fn)`: `kind` is `\"accel\"` or `\"gyro\"`;\n"
        "  `opts.rate` is one of ";
    for (size_t i = 0; i < core::SensorService::kRates.size(); ++i) {
        if (i) out += i + 1 == core::SensorService::kRates.size() ? " or " : ", ";
        out += num(core::SensorService::kRates[i]);
    }
    out +=
        " samples a second (default 25; `opts` may be left out).\n"
        "  `fn(samples)` gets the samples about every 100 ms, in a batch: `x`, `y`, `z` in\n"
        "  m/s² (`accel`) or degrees per second (`gyro`), `t` in milliseconds.\n"
        "- `wisp.sensor.on(gesture, fn)`: `fn()` runs on each `\"shake\"` (three strong jolts\n"
        "  within half a second) or `\"flip\"` (turned face down or face up, then still for\n"
        "  300 ms).\n"
        "- Both return a handle, or `nil` and a code: `not_in_background`, `unavailable` (no\n"
        "  such sensor) or another error's name. `h:stop()` ends it and returns whether it\n"
        "  was still running.\n"
        "- `wisp.sensor.available()` lists the sensors there are; `{}` without an IMU.\n"
        "- Subscriptions pause while the screen is off and in the background, resume in\n"
        "  front, and end when the scene closes.\n"
        "\n"
        "## wisp.files\n"
        "\n"
        "Each scene has its own flat directory of files, for data too large for `wisp.store`\n"
        "(downloads, recordings, logs). Every call is asynchronous: `cb(err, ...)` runs\n"
        "later as a message of its own, with `err` `nil` on success.\n"
        "\n"
        "```lua\n"
        "wisp.files.write(name, data, cb)          -- replaces the file; cb(err, true)\n"
        "wisp.files.append(name, data, cb)         -- cb(err, true)\n"
        "wisp.files.read(name, offset, len, cb)    -- cb(err, data)\n"
        "wisp.files.list(cb)                       -- cb(err, { {name =, size =, mtime =}, ... }, "
        "truncated)\n"
        "wisp.files.remove(name, cb)               -- cb(err, true)\n"
        "wisp.files.info(cb)                       -- cb(err, { location =, want =, quota =, "
        "used = })\n"
        "```\n"
        "\n"
        "- Names: 1 to 64 bytes of `A-Z`, `a-z`, `0-9`, `.`, `_` and `-`, not starting with `.`\n"
        "  and not ending in `.tmp` or `.new`; no directories. Anything else is `bad_name`.\n";
    out += "- One `write` or `append` takes at most " + num(core::kMaxFileWrite / 1024) +
           " KB and one `read` at most " + num(files.max_read / 1024) +
           " KB\n"
           "  (`too_large`); write more with several `append`s or `wisp.http.download`. `write`\n"
           "  replaces the file whole or not at all.\n";
    out += "- `list` gives at most " + num(files.max_list) +
           " names, in name order; `truncated` is `true` when there were\n"
           "  more.\n";
    out +=
        "- Where the files live is chosen at the scene's first write, from `files` in\n"
        "  `scene.json`: `\"auto\"` takes the SD card when one is in and internal storage\n";
    out +=
        "  otherwise; `\"sd\"` only the SD card; `\"int\"` always internal storage. On the card\n"
        "  a scene may use " +
        num(files.sd_quota / (1024 * 1024)) + " MB, in internal storage " +
        num(files.int_quota / 1024) +
        " KB (`quota_exceeded`, and nothing\n"
        "  is written). The choice is kept: a later change of `files` moves nothing.\n";
    out +=
        "- `files: \"sd\"` with no card in means the files are unavailable: every call returns\n"
        "  `unavailable`, and nothing falls back to internal storage. The same goes for files\n"
        "  already on a card that has been taken out; they come back with the card.\n"
        "- `info` tells `location` (`\"sd\"` or `\"int\"`; absent before the first write),\n"
        "  `want` (the `files` declaration), `quota` and `used` in bytes.\n"
        "- The system never deletes a scene's files by itself. They go only when the user\n"
        "  deletes the scene and confirms deleting its data with it, when the scene calls\n"
        "  `wisp.files.remove`, or when the user deletes one on the admin page. A scene\n"
        "  reinstalled with the same id finds its old files where it left them.\n"
        "- Other codes: `not_found` (no such file), `unavailable`, `busy` (the storage worker\n"
        "  has too much queued: try again later), `no_memory`, or the name of a storage\n"
        "  error.\n"
        "\n"
        "## wisp.http.download and deep standby\n"
        "\n"
        "- `wisp.http.download(url, name, opts, cb)` GETs `url` (a host from\n"
        "  `permissions.http`, as for `wisp.http.get`; `opts` as for `get`, may be left out)\n"
        "  straight into the scene's file `name`, which it replaces once the whole body has\n"
        "  arrived. It is not limited to the 256 KB of a normal response, only by the\n"
        "  scene's quota. `cb(err, res)`: `res = {status, size, deferred}`. A response that\n"
        "  is not 2xx leaves the file as it was and gives `size = 0`. Errors are those of\n"
        "  `wisp.http` and of `wisp.files`.\n"
        "- In deep standby (the screen has been off for a while) Wi-Fi is off. Requests and\n"
        "  downloads then wait in a queue, at most 4 per scene (`deferred_queue_full` past\n"
        "  it), until the system turns Wi-Fi on for a short network window, at most every 15\n"
        "  minutes. A request that waited gets `res.deferred = true`; one that waited more\n"
        "  than an hour fails with `deferred_timeout`.\n"
        "\n"
        "## Data migration\n"
        "\n"
        "Raise `schema_version` in `scene.json` when the structure of the scene's data\n"
        "changes. When a scene opens (or is restored at boot) and its data is from a lower\n"
        "version, `scene.migrate(old_version)` runs after the entry file and before the\n";
    out += "widget tree is built, for at most " + num(kMigrateMs) +
           " ms. What the entry file and the migration write\n"
           "to `wisp.store` is held back and saved, with the new version, only when the\n"
           "migration returns; an error or a timeout drops all of it, leaves the data as it\n"
           "was and puts the scene in error with `migrate failed: <message>`. A scene with no\n"
           "`scene.migrate` just has its version raised. Data from a higher version than the\n"
           "scene's (an older scene installed over a newer one) is refused: the scene does not\n"
           "open.\n";
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
        case Prop::Font:
            return {kFontPxMin, kFontPxMax};
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
        "# Scene API\n"
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
        "local d = wisp.device.info()\n"
        "local s = d.safe_area\n"
        "return ui.screen { align = \"center\", justify = \"center\",\n"
        "  -- {top, right, bottom, left}: keeps the column inside the safe area\n"
        "  pad = { s.y1, d.width - 1 - s.x2, d.height - 1 - s.y2, s.x1 },\n"
        "  ui.column { gap = 12, align = \"center\",\n"
        "    ui.label { text = bind(count, function(v) return \"clicked \" .. v end) },\n"
        "    ui.button { text = \"+1\", on_click = function() count:set(count:get() + 1) end "
        "},\n"
        "  },\n"
        "}\n"
        "```\n"
        "\n"
        "Rules:\n"
        "\n"
        "- The root must be `ui.screen`. It fills the whole display and has no padding of its\n"
        "  own: the scene lays itself out (see Screen shape below).\n";
    char line[512];
    std::snprintf(line, sizeof(line),
                  "- At most %zu widgets per tree, counting the rows every list has built: rows\n"
                  "  past it are not built, and the scene's log says so. A string value is at\n"
                  "  most %zu KB.\n",
                  kMaxNodes, kMaxPropStringBytes / 1024);
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
        "- `\"fill\"` across a `column` or `row` (a column child's `w`, a row child's `h`) is\n"
        "  the parent's whole size, so inside a parent that is itself sized by its content\n"
        "  (the default) it can come out 0: give that parent a size or `\"fill\"` too.\n"
        "- `scene.on_show()` first runs right after `main.lua` returns, before the tree is on\n"
        "  the screen; what it changes is still applied after the tree is built.\n"
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
    out += ", or integer pixels 16 to 128 (see Font sizes) |\n| align | ";
    append_list(out, kAligns);
    out += " |\n| justify | ";
    append_list(out, kJustifies);
    out +=
        " |\n"
        "| pad | integer pixels on every side, or `{top, right, bottom, left}` (CSS order), "
        "each 0 to 512 |\n"
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
        "## Font sizes\n"
        "\n"
        "`font` is a token or a size in pixels from 16 to 128. `xs`, `sm` and `md` are the\n"
        "16-pixel body font, `lg` is 24, `xl` 32 and `xxl` 48. Sizes 16 to 19 are the body\n"
        "font too; from 20 on the text is drawn from a vector font in steps of 20, 24, 28, 32,\n"
        "40, 48, 64, 80, 96 and 128 pixels, and a size between two steps gets the lower one\n"
        "(50 is 48). Symbols (`ui.icon`) come in 16, 24, 32 and 48 pixels, again the largest\n"
        "that is not above the size.\n"
        "\n"
        "Large sizes suit short text such as a number or a time. Each glyph is drawn the\n"
        "first time it is shown at a size, which takes a few milliseconds at the largest\n"
        "sizes, and only a few glyphs per size are kept (13 at 128 pixels), so a long text in\n"
        "a large size is redrawn slowly.\n"
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
        "### list, canvas, chart, image, icon\n"
        "\n"
        "- `ui.list`: only the rows around the visible part exist. `row(item, index)` runs for\n"
        "  each row as it scrolls into view and its widgets are dropped when it scrolls away;\n"
        "  bindings inside a row live as long as the row. Every row is `row_h` pixels tall "
        "(default\n"
        "  48) and as wide as the list, whatever its own `w` and `h`. A row cannot use `ref` or\n"
        "  hold another `ui.list`. Setting `items` builds the visible rows again.\n"
        "- `ui.canvas`: `draw(g)` runs once the canvas is built and again whenever a state it\n"
        "  read changes. `g` only works while `draw` runs, for at most 512 calls:\n"
        "  `g:fill(color)`, `g:line(x1, y1, x2, y2, opts)`, `g:circle(cx, cy, r, opts)`,\n"
        "  `g:arc(cx, cy, r, start, end, opts)` (degrees, clockwise from 3 o'clock),\n"
        "  `g:rect(x, y, w, h, opts)`, `g:text(x, y, text, opts)`. `opts` is optional:\n"
        "  `color` (a colour, default `text`), `width` (lines and outlines, 0 to 512, default\n"
        "  1), `radius` (rect corners, 0 to 4096), `fill` (boolean: circle and rect filled),\n"
        "  `font` (text: a token or 16 to 128 pixels, as for the property). A canvas is `fill`\n"
        "  in both directions unless given a size. A canvas of more than 524,288 pixels stays\n"
        "  empty (\"ui.canvas: too large\" in the scene's log). The `g:text` that takes the\n"
        "  update past 1 MB is the error \"ui update too large\".\n"
        "- `ui.chart`: `series` is up to 4 tables of up to 128 numbers, drawn in `accent`, `ok`,\n"
        "  `warn` and `danger`, between `min` and `max` (default 0 and 100); `type` is `line`\n"
        "  (default) or `bar`.\n"
        "- `ui.image`: `src` is a PNG or a baseline (not progressive) JPEG in the scene's\n"
        "  `assets/`, at most 1024 pixels a side and 524,288 pixels in all. A file that is\n"
        "  missing, cannot be decoded or is too large leaves the image empty and is reported in\n"
        "  the scene's log. The decoded images and canvases of a scene take at most 4 MB\n"
        "  together; the one that would pass it stays empty (\"too large\").\n"
        "- `ui.icon`: the symbol `name` in the current `font`.\n"
        "\n"
        "## Screen shape\n"
        "\n"
        "The screen may be a rectangle, a rounded rectangle or a circle, and a scene gets all of\n"
        "it: `ui.screen` fills the display, and pixels outside the visible shape are simply not\n"
        "shown. Lay out from what the device reports instead of fixed margins:\n"
        "\n"
        "- `wisp.device.info()` has `width`, `height`, `shape` (`rect`, `rounded_rect` or\n"
        "  `circle`), `corner_radius` (the rounded rectangle's radius, half the smaller side for\n"
        "  a circle, 0 for a rectangle) and `safe_area` (`x1`, `y1`, `x2`, `y2`, inclusive: the\n"
        "  largest centred rectangle that is visible everywhere, with a small margin).\n"
        "- `wisp.device.info().status_area` (`x1`, `y1`, `x2`, `y2`, inclusive) is the system's\n"
        "  status capsule at the top centre of the safe area. It is given whether or not the\n"
        "  capsule shows, so a scene that keeps clear of it never has its layout jump.\n"
        "- `wisp.device.row_span(y)` returns `x1, x2`, the first and last visible pixel of\n"
        "  screen row `y`, or `nil` when the row shows nothing or is off the screen.\n"
        "\n"
        "A centred block can pad the screen by the safe area, as in the example above. Content\n"
        "near the top or bottom edge can be inset by `row_span` at its own rows and use the\n"
        "width the safe area would waste. Scrolling content may pass through the invisible\n"
        "corners, but when scrolled to either end its first and last item must be whole on the\n"
        "visible screen: pad the list (per side) so that `row_span` at the item's top and\n"
        "bottom rows covers its left and right edges.\n";
    std::snprintf(line, sizeof(line),
                  "\n"
                  "A swipe to the right that starts within %d pixels of the left edge of the\n"
                  "visible screen (rows within the safe area's height) is the system's back\n"
                  "gesture, at any angle: a touch there reaches the scene like any other, and\n"
                  "only once it has moved %d pixels to the right -- however far up or down --\n"
                  "does the system take it, and the widget under it then gets no click.\n"
                  "Dragging back to the left of that distance before release cancels it and\n"
                  "nothing happens. A touch on the on-screen keyboard never starts it.\n",
                  static_cast<int>(ui::SystemLayer::kBackZoneWidth),
                  static_cast<int>(ui::SystemLayer::kBackTriggerDx));
    out += line;
    out += "\nA swipe down that starts within " + num(ui::SystemLayer::kTopZoneHeight) +
           " pixels of the top of the visible screen\n"
           "(measured from the first visible pixel of the column it starts in) is the\n"
           "system's pull-down gesture, at any angle: the touch reaches the scene like\n"
           "any other until it has moved " +
           num(ui::SystemLayer::kTopTriggerDy) +
           " pixels down, when the system opens its\n"
           "pull-down panel and the widget under the finger gets no click\n"
           "(`PRESS_LOST`). It never starts while the keyboard, the error panel or a\n"
           "confirmation dialog is up. A tap on the status capsule (at `status_area`, shown while "
           "scenes run\n"
           "in the background or notifications are unread) opens the panel too.\n";
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
        "  - `h:focus()`: focuses an `input` and opens the on-screen keyboard.\n"
        "\n"
        "## wisp.store\n"
        "\n"
        "A scene's own persistent data (scene-runtime-core spec 5.4): a key/value space plus up\n"
        "to 16 named collections of JSON records, kept under the scene's `data/` and never seen\n"
        "by any other scene.\n"
        "\n"
        "- `wisp.store.get(key)` returns the stored value, or `nil` if `key` has none.\n"
        "- `wisp.store.set(key, value)` returns `true`, or `nil` and a code; `value == nil`\n"
        "  deletes `key`.\n"
        "- `wisp.store.batch(fn)` calls `fn` with every `wisp.store` write inside it held back\n"
        "  until `fn` returns: if `fn` raises, or any one of those writes would itself fail,\n"
        "  none of them take effect and the call returns `nil, \"batch_aborted\"`; otherwise\n"
        "  they all do and the call returns `true`. Batches may nest.\n"
        "- `wisp.store.collection(name)` returns a handle `c` for one of up to 16 collections\n"
        "  (`name` is 1 to 32 bytes of lowercase letters, digits, `_` or `-`; `\"kv\"` is\n"
        "  reserved for the key/value space above):\n"
        "  - `c:insert(record)` returns the new record's `id` (an integer), or `nil` and a code.\n"
        "  - `c:update(id, record)` returns `true`, or `nil` and a code.\n"
        "  - `c:delete(id)` returns `true`, or `nil` and a code (`\"not_found\"` for an `id` that\n"
        "    does not exist).\n"
        "  - `c:find(filter?, opts?)` returns a list of matching records, each with its `id`\n"
        "    field set.\n"
        "\n"
        "`record` is a table with string keys. `id` in it is reserved: `insert` and `update`\n"
        "strip it out before storing (so a record `find` returns can be passed straight back to\n"
        "`update`), and it is always the id the store itself hands out, never a stored field.\n"
        "`filter` (a table) keeps only records equal on every field it gives; `filter.id` tests\n"
        "the record id rather than a stored field. `opts.sort` orders by one top-level field\n"
        "(numbers by value, strings by bytes; a record missing it sorts last whatever\n"
        "`opts.desc` says); `sort = \"id\"`, or no `sort` at all, means id order, which is\n"
        "insertion order -- newest first once `opts.desc` is set. `opts.limit` and\n"
        "`opts.offset` page the result.\n"
        "\n"
        "Codes: `quota_exceeded`, `record_too_large`, `too_many_collections`,\n"
        "`not_serializable`, `too_deep`, `not_found`, `ids_exhausted`, `batch_aborted`. A key,\n"
        "collection name or record of the wrong type, length or shape is a Lua error instead,\n"
        "at the line that made the call.\n";
    append_device_api(out);
    return out;
}

}  // namespace wisp::scene
