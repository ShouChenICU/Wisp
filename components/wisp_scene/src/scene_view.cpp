// SceneView (see wisp/scene/scene_view.hpp).

#include "wisp/scene/scene_view.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <variant>

#include "scene_view_internal.hpp"
#include "wisp/core/log.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/schema.hpp"
#include "wisp/scene/theme.hpp"

namespace wisp::scene {

using namespace view;

namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// What the schema takes for a Color prop. theme_color() maps anything
// else to `text`; a value that is not a colour is dropped instead.
bool color_ok(std::string_view s) {
    for (std::string_view t : prop_spec(Prop::Bg).choices)
        if (s == t) return true;
    if (s.size() != 7 || s[0] != '#') return false;
    for (size_t i = 1; i < 7; ++i)
        if (hex_digit(s[i]) < 0) return false;
    return true;
}

// The part `fg` colours besides the text, or 0.
lv_style_selector_t indicator_part(Widget w) {
    switch (w) {
        case Widget::Slider:
        case Widget::Bar:
        case Widget::Switch:
        case Widget::Arc:
            return LV_PART_INDICATOR;
        default:
            return 0;
    }
}

bool is_layout(Widget w) {
    switch (w) {
        case Widget::Screen:
        case Widget::Column:
        case Widget::Row:
        case Widget::Stack:
        case Widget::Grid:
        case Widget::Scroll:
            return true;
        default:
            return false;
    }
}

bool has_prop(Widget w, Prop p) {
    for (Prop q : widget_spec(w).props)
        if (q == p) return true;
    return false;
}

lv_flex_align_t align_place(std::string_view s) {
    if (s == "center") return LV_FLEX_ALIGN_CENTER;
    if (s == "end") return LV_FLEX_ALIGN_END;
    return LV_FLEX_ALIGN_START;
}

lv_flex_align_t justify_place(std::string_view s) {
    if (s == "center") return LV_FLEX_ALIGN_CENTER;
    if (s == "end") return LV_FLEX_ALIGN_END;
    if (s == "between") return LV_FLEX_ALIGN_SPACE_BETWEEN;
    if (s == "around") return LV_FLEX_ALIGN_SPACE_AROUND;
    if (s == "evenly") return LV_FLEX_ALIGN_SPACE_EVENLY;
    return LV_FLEX_ALIGN_START;
}

// Writes `v` (a String prop: a string, or a number as Lua prints it) as
// text into `buf`; returns a NUL-terminated string, nullptr for any other
// type.
const char* text_of(const PropValue& v, char* buf, size_t size) {
    if (const auto* s = as_string(v)) return s->c_str();
    if (const auto* i = std::get_if<int64_t>(&v)) {
        std::snprintf(buf, size, "%" PRId64, *i);
        return buf;
    }
    if (const auto* d = std::get_if<double>(&v)) {
        if (std::isnan(*d)) return std::signbit(*d) ? "-nan" : "nan";
        if (std::isinf(*d)) return *d > 0 ? "inf" : "-inf";
        std::snprintf(buf, size, "%.14g", *d);
        // Lua marks a float that looks like an integer: 2.0, not 2.
        if (std::strspn(buf, "-0123456789") == std::strlen(buf)) {
            size_t n = std::strlen(buf);
            if (n + 2 < size) std::memcpy(buf + n, ".0", 3);
        }
        return buf;
    }
    return nullptr;
}

}  // namespace

SceneView::SceneView(ui::SystemLayer& system, const hal::ScreenShape& shape, ViewCallbacks cb)
    : system_(system), shape_(shape), cb_(std::move(cb)) {
    hal::Area safe = hal::safe_rect(shape_);
    safe_ = {std::max<int32_t>(0, safe.x1), std::max<int32_t>(0, safe.y1),
             std::max<int32_t>(0, shape_.width - 1 - safe.x2),
             std::max<int32_t>(0, shape_.height - 1 - safe.y2)};
    screen_ = lv_obj_create(nullptr);
    group_ = lv_group_create();
    reset_screen_style();
}

SceneView::~SceneView() {
    ++applying_;
    lv_obj_delete(screen_);
    lv_group_delete(group_);
    nodes_.clear();
    refs_.clear();
}

void SceneView::set_assets(std::shared_ptr<const core::ScenePackage> package) {
    assets_ = std::move(package);
}

lv_obj_t* SceneView::object(NodeId node) const {
    Node* n = find(node);
    return n ? n->obj : nullptr;
}

std::optional<NodeId> SceneView::find_ref(std::string_view ref) const {
    auto it = refs_.find(ref);
    if (it == refs_.end()) return std::nullopt;
    return it->second;
}

SceneView::Node* SceneView::find(NodeId id) const {
    auto it = nodes_.find(id);
    return it == nodes_.end() ? nullptr : it->second.get();
}

void SceneView::apply(const UiOp& op) {
    ++applying_;
    if (const auto* s = std::get_if<SetProp>(&op))
        apply_set(*s);
    else if (const auto* b = std::get_if<Build>(&op))
        apply_build(*b);
    else if (const auto* sc = std::get_if<ScrollTo>(&op))
        apply_scroll(*sc);
    else if (const auto* f = std::get_if<Focus>(&op))
        apply_focus(*f);
    else if (const auto* r = std::get_if<ListReset>(&op))
        apply_list_reset(*r);
    else if (const auto* rows = std::get_if<ListRows>(&op))
        apply_list_rows(*rows);
    else if (const auto* d = std::get_if<CanvasDraw>(&op))
        apply_canvas(*d);
    --applying_;
}

// --- Build ---

bool SceneView::build_ok(const Build& b) const {
    const auto& nodes = b.nodes;
    if (nodes.empty() || nodes.size() > kMaxNodes) return false;
    if (nodes[0].widget != static_cast<WidgetId>(Widget::Screen) || nodes[0].parent != 0)
        return false;
    std::unordered_map<NodeId, WidgetId, std::hash<NodeId>, std::equal_to<NodeId>,
                       os::LargeAllocator<std::pair<const NodeId, WidgetId>>>
        seen;
    seen.reserve(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) {
        const NodeDesc& n = nodes[i];
        if (n.id == 0 || n.widget >= kWidgetCount) return false;
        if (i > 0) {
            if (n.widget == static_cast<WidgetId>(Widget::Screen)) return false;
            auto parent = seen.find(n.parent);
            if (parent == seen.end()) return false;
            if (!widget_spec(static_cast<Widget>(parent->second)).children) return false;
        }
        if (!seen.emplace(n.id, n.widget).second) return false;
    }
    return true;
}

void SceneView::reset_screen_style() {
    lv_obj_remove_style_all(screen_);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(screen_, nullptr);
    lv_obj_set_style_bg_color(screen_, theme_color("bg"), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_text_color(screen_, theme_color("text"), LV_PART_MAIN);
    lv_obj_set_style_text_font(screen_, theme_font("md"), LV_PART_MAIN);
    lv_obj_set_flex_flow(screen_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_left(screen_, safe_.left, LV_PART_MAIN);
    lv_obj_set_style_pad_top(screen_, safe_.top, LV_PART_MAIN);
    lv_obj_set_style_pad_right(screen_, safe_.right, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(screen_, safe_.bottom, LV_PART_MAIN);
}

void SceneView::clear_tree() {
    lv_obj_clean(screen_);
    lv_obj_remove_event_cb(screen_, event_cb);
    nodes_.clear();
    refs_.clear();
    root_ = 0;
    reset_screen_style();
}

void SceneView::apply_build(const Build& b) {
    if (!build_ok(b)) {
        WLOG_W(kTag, "malformed widget tree (%zu nodes) dropped", b.nodes.size());
        return;
    }
    // Every node is allocated before the old tree goes, so running out
    // of memory leaves that tree as it was.
    os::LargeVector<os::LargeUnique<Node>> fresh;
    fresh.reserve(b.nodes.size());
    for (size_t i = 0; i < b.nodes.size(); ++i) {
        fresh.push_back(os::make_large<Node>());
        if (!fresh.back()) {
            WLOG_E(kTag, "out of memory building a tree of %zu widgets", b.nodes.size());
            return;
        }
    }
    clear_tree();
    nodes_.reserve(b.nodes.size());
    building_ = true;
    for (size_t i = 0; i < b.nodes.size(); ++i) {
        const NodeDesc& d = b.nodes[i];
        Node& n = *fresh[i];
        Node* parent = find(d.parent);
        nodes_.emplace(d.id, std::move(fresh[i]));
        init_node(n, d, parent);
    }
    building_ = false;
    // Grids are laid out once their children are all there.
    for (auto& [id, node] : nodes_)
        if (node->widget == Widget::Grid) layout_grid(*node);
}

void SceneView::init_node(Node& n, const NodeDesc& d, Node* parent) {
    n.view = this;
    n.id = d.id;
    n.parent = d.parent;
    n.widget = static_cast<Widget>(d.widget);
    n.events = d.events;
    if (parent) {
        n.obj = create_object(n, parent->obj);
    } else {
        n.obj = screen_;
        n.flow = Flow::Column;
        root_ = n.id;
    }
    lv_obj_set_user_data(n.obj, &n);
    for (const auto& [prop, value] : d.props) {
        if (prop == static_cast<PropId>(Prop::Ref)) {
            if (const auto* s = as_string(value)) refs_.emplace(*s, n.id);
            continue;
        }
        set_prop(n, prop, value);
    }
    update(n, kAllDerived);
    attach_events(n);
    if (parent && parent->widget == Widget::Stack) lv_obj_set_align(n.obj, stack_align(*parent));
}

lv_obj_t* SceneView::create_object(Node& n, lv_obj_t* parent) {
    lv_obj_t* obj = nullptr;
    switch (n.widget) {
        case Widget::Button:
            obj = lv_button_create(parent);
            lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(obj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            n.flow = Flow::Row;
            return obj;
        case Widget::Label:
            obj = lv_label_create(parent);
            lv_label_set_text(obj, "");
            return obj;
        case Widget::Checkbox:
            obj = lv_checkbox_create(parent);
            lv_checkbox_set_text(obj, "");
            return obj;
        case Widget::Switch:
            return lv_switch_create(parent);
        case Widget::Slider:
            return lv_slider_create(parent);
        case Widget::Bar:
            return lv_bar_create(parent);
        case Widget::Arc:
            // A progress ring: no knob, not draggable.
            obj = lv_arc_create(parent);
            lv_obj_remove_style(obj, nullptr, LV_PART_KNOB);
            lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
            return obj;
        case Widget::Input:
            obj = lv_textarea_create(parent);
            lv_textarea_set_one_line(obj, true);
            return obj;
        case Widget::Icon:
            obj = lv_label_create(parent);
            lv_label_set_text(obj, "");
            return obj;
        case Widget::Image:
            return lv_image_create(parent);
        case Widget::Chart:
            obj = lv_chart_create(parent);
            lv_chart_set_axis_range(obj, LV_CHART_AXIS_PRIMARY_Y, 0, kScaledSteps);
            return obj;
        case Widget::Canvas:
            obj = lv_canvas_create(parent);
            lv_obj_add_event_cb(obj, canvas_event_cb, LV_EVENT_SIZE_CHANGED, &n);
            return obj;
        default:
            break;
    }
    // Layout widgets, spacer and list: a bare object without the theme's
    // padding, border or background. Only
    // scroll and list take touches they do not handle, so a drag anywhere
    // on them scrolls; the rest let presses through to what is behind.
    obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    if (n.widget != Widget::Scroll && n.widget != Widget::List)
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    switch (n.widget) {
        case Widget::Column:
            n.flow = Flow::Column;
            lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
            break;
        case Widget::Scroll:
            n.flow = Flow::Column;
            lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_scroll_dir(obj, LV_DIR_VER);
            break;
        case Widget::Row:
            n.flow = Flow::Row;
            lv_obj_set_flex_flow(obj, LV_FLEX_FLOW_ROW);
            break;
        case Widget::List:
            lv_obj_set_scroll_dir(obj, LV_DIR_VER);
            n.filler = lv_obj_create(obj);
            lv_obj_remove_style_all(n.filler);
            lv_obj_remove_flag(n.filler, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(n.filler, 1, 0);
            lv_obj_add_event_cb(obj, list_event_cb, LV_EVENT_SCROLL, &n);
            lv_obj_add_event_cb(obj, list_event_cb, LV_EVENT_SIZE_CHANGED, &n);
            break;
        default:
            break;
    }
    return obj;
}

void SceneView::attach_events(Node& n) {
    const bool touch = (n.events & (kEventClick | kEventLongPress)) != 0;
    if (touch) {
        lv_obj_add_flag(n.obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(n.obj, event_cb, LV_EVENT_PRESSED, &n);
        lv_obj_add_event_cb(n.obj, event_cb, LV_EVENT_CLICKED, &n);
        lv_obj_add_event_cb(n.obj, event_cb, LV_EVENT_LONG_PRESSED, &n);
    }
    switch (n.widget) {
        case Widget::Input:
            lv_obj_add_event_cb(n.obj, event_cb, LV_EVENT_FOCUSED, &n);
            if (!touch) lv_obj_add_event_cb(n.obj, event_cb, LV_EVENT_CLICKED, &n);
            lv_obj_add_event_cb(n.obj, event_cb, LV_EVENT_VALUE_CHANGED, &n);
            break;
        case Widget::Slider:
        case Widget::Checkbox:
        case Widget::Switch:
            lv_obj_add_event_cb(n.obj, event_cb, LV_EVENT_VALUE_CHANGED, &n);
            break;
        default:
            break;
    }
}

// --- Props ---

void SceneView::apply_set(const SetProp& s) {
    Node* n = find(s.node);
    if (n == nullptr) {
        WLOG_D(kTag, "update for node %" PRIu32 " that is gone", s.node);
        return;
    }
    if (s.prop == static_cast<PropId>(Prop::Ref)) return;  // fixed when built
    update(*n, set_prop(*n, s.prop, s.value));
}

uint32_t SceneView::set_prop(Node& n, uint16_t prop_id, const PropValue& v) {
    if (prop_id >= kPropCount || !has_prop(n.widget, static_cast<Prop>(prop_id))) {
        WLOG_D(kTag, "node %" PRIu32 " has no prop %u", n.id, static_cast<unsigned>(prop_id));
        return 0;
    }
    const Prop p = static_cast<Prop>(prop_id);
    const bool unset = std::holds_alternative<std::monostate>(v);
    lv_obj_t* obj = n.obj;
    const auto wrong = [&]() -> uint32_t {
        WLOG_D(kTag, "node %" PRIu32 ": bad value for '%.*s'", n.id,
               static_cast<int>(prop_spec(p).name.size()), prop_spec(p).name.data());
        return 0;
    };
    switch (p) {
        case Prop::W:
        case Prop::H: {
            Size size;
            if (const auto* s = as_string(v)) {
                if (*s == "fill")
                    size.kind = SizeKind::Fill;
                else if (*s == "content")
                    size.kind = SizeKind::Content;
                else
                    return wrong();
            } else if (auto px = as_int(v, p)) {
                size = {SizeKind::Px, *px};
            } else if (!unset) {
                return wrong();
            }
            (p == Prop::W ? n.w : n.h) = size;
            return kSizeDerived;
        }
        case Prop::Grow: {
            if (unset) {
                n.grow = -1;
            } else if (auto g = as_int(v, p)) {
                n.grow = *g;
            } else {
                return wrong();
            }
            return kSizeDerived;
        }
        case Prop::Pad: {
            if (unset) {
                n.pad_given = false;
            } else if (const auto* s = as_string(v)) {
                if (*s != "safe") return wrong();
                n.pad_given = true;
                n.pad_safe = true;
            } else if (auto pad = as_int(v, p)) {
                n.pad_given = true;
                n.pad_safe = false;
                n.pad = *pad;
            } else {
                return wrong();
            }
            return kPadDerived;
        }
        case Prop::Gap: {
            auto gap = unset ? std::optional<int32_t>(0) : as_int(v, p);
            if (!gap) return wrong();
            lv_obj_set_style_pad_row(obj, *gap, LV_PART_MAIN);
            lv_obj_set_style_pad_column(obj, *gap, LV_PART_MAIN);
            return 0;
        }
        case Prop::Align:
        case Prop::Justify: {
            const auto* s = as_string(v);
            if (!s && !unset) return wrong();
            std::string_view word = s ? std::string_view(*s) : std::string_view("start");
            if (p == Prop::Align)
                n.align = align_place(word);
            else
                n.justify = justify_place(word);
            return kAlignDerived;
        }
        case Prop::Bg: {
            if (unset) {
                lv_obj_remove_local_style_prop(obj, LV_STYLE_BG_COLOR, LV_PART_MAIN);
                lv_obj_remove_local_style_prop(obj, LV_STYLE_BG_OPA, LV_PART_MAIN);
                return 0;
            }
            const auto* s = as_string(v);
            if (!s || !color_ok(*s)) return wrong();
            lv_obj_set_style_bg_color(obj, theme_color(*s), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
            return 0;
        }
        case Prop::Fg: {
            const auto* s = as_string(v);
            if (!s && !unset) return wrong();
            if (s && !color_ok(*s)) return wrong();
            lv_style_selector_t indicator = indicator_part(n.widget);
            if (unset) {
                lv_obj_remove_local_style_prop(obj, LV_STYLE_TEXT_COLOR, LV_PART_MAIN);
                if (indicator != 0) {
                    lv_obj_remove_local_style_prop(obj, LV_STYLE_BG_COLOR, indicator);
                    lv_obj_remove_local_style_prop(obj, LV_STYLE_ARC_COLOR, indicator);
                }
                return 0;
            }
            lv_color_t c = theme_color(*s);
            lv_obj_set_style_text_color(obj, c, LV_PART_MAIN);
            if (n.widget == Widget::Arc)
                lv_obj_set_style_arc_color(obj, c, LV_PART_INDICATOR);
            else if (indicator != 0)
                lv_obj_set_style_bg_color(obj, c, indicator);
            return 0;
        }
        case Prop::Radius: {
            if (unset) {
                lv_obj_remove_local_style_prop(obj, LV_STYLE_RADIUS, LV_PART_MAIN);
                return 0;
            }
            auto r = as_int(v, p);
            if (!r) return wrong();
            lv_obj_set_style_radius(obj, *r, LV_PART_MAIN);
            return 0;
        }
        case Prop::Border: {
            if (unset) {
                lv_obj_remove_local_style_prop(obj, LV_STYLE_BORDER_WIDTH, LV_PART_MAIN);
                return 0;
            }
            auto b = as_int(v, p);
            if (!b) return wrong();
            lv_obj_set_style_border_width(obj, *b, LV_PART_MAIN);
            if (*b > 0) {
                lv_obj_set_style_border_color(obj, theme_color("muted"), LV_PART_MAIN);
                lv_obj_set_style_border_opa(obj, LV_OPA_COVER, LV_PART_MAIN);
            }
            return 0;
        }
        case Prop::Font: {
            if (unset) {
                lv_obj_remove_local_style_prop(obj, LV_STYLE_TEXT_FONT, LV_PART_MAIN);
                return 0;
            }
            const auto* s = as_string(v);
            if (!s) return wrong();
            lv_obj_set_style_text_font(obj, theme_font(*s), LV_PART_MAIN);
            return 0;
        }
        case Prop::Hidden: {
            auto b = unset ? std::optional<bool>(false) : as_bool(v);
            if (!b) return wrong();
            if (*b)
                lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
            return 0;
        }
        case Prop::Opa: {
            if (unset) {
                lv_obj_remove_local_style_prop(obj, LV_STYLE_OPA, LV_PART_MAIN);
                return 0;
            }
            double o = 1.0;
            if (const auto* i = std::get_if<int64_t>(&v))
                o = static_cast<double>(*i);
            else if (const auto* d = std::get_if<double>(&v))
                o = std::isnan(*d) ? 1.0 : *d;
            else
                return wrong();
            o = std::clamp(o, 0.0, 1.0);
            lv_obj_set_style_opa(obj, static_cast<lv_opa_t>(std::lround(o * 255.0)), LV_PART_MAIN);
            return 0;
        }
        case Prop::Text:
            if (!unset && !std::holds_alternative<os::LargeString>(v) &&
                !std::holds_alternative<int64_t>(v) && !std::holds_alternative<double>(v))
                return wrong();
            set_text(n, v);
            return 0;
        case Prop::Placeholder: {
            char buf[32];
            const char* text = unset ? "" : text_of(v, buf, sizeof(buf));
            if (!text) return wrong();
            lv_textarea_set_placeholder_text(obj, text);
            return 0;
        }
        case Prop::Password: {
            auto b = unset ? std::optional<bool>(false) : as_bool(v);
            if (!b) return wrong();
            lv_textarea_set_password_mode(obj, *b);
            return 0;
        }
        case Prop::MaxLen: {
            auto len = unset ? std::optional<int32_t>(0) : as_int(v, p);
            if (!len) return wrong();
            lv_textarea_set_max_length(obj, static_cast<uint32_t>(*len));
            return 0;
        }
        case Prop::Value:
        case Prop::Min:
        case Prop::Max: {
            double fallback = p == Prop::Max ? 100.0 : 0.0;
            auto x = unset ? std::optional<double>(fallback) : as_number(v);
            if (!x) return wrong();
            (p == Prop::Value ? n.value : p == Prop::Min ? n.min : n.max) = *x;
            return kRangeDerived;
        }
        case Prop::Checked: {
            auto b = unset ? std::optional<bool>(false) : as_bool(v);
            if (!b) return wrong();
            if (*b)
                lv_obj_add_state(obj, LV_STATE_CHECKED);
            else
                lv_obj_remove_state(obj, LV_STATE_CHECKED);
            return 0;
        }
        case Prop::Cols: {
            auto c = unset ? std::optional<int32_t>(1) : as_int(v, p);
            if (!c) return wrong();
            n.cols = *c;
            return kGridDerived;
        }
        case Prop::Direction: {
            const auto* s = as_string(v);
            if (!s) return wrong();
            const bool h = *s == "h";
            n.flow = h ? Flow::Row : Flow::Column;
            lv_obj_set_flex_flow(obj, h ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
            lv_obj_set_scroll_dir(obj, h ? LV_DIR_HOR : LV_DIR_VER);
            return kAlignDerived;
        }
        case Prop::RowH: {
            auto r = unset ? std::optional<int32_t>(kDefaultRowH) : as_int(v, p);
            if (!r) return wrong();
            n.row_h = *r;
            return kListDerived;
        }
        case Prop::Width: {
            auto w = unset ? std::optional<int32_t>(0) : as_int(v, p);
            if (!w) return wrong();
            lv_obj_set_style_arc_width(obj, *w, LV_PART_MAIN);
            lv_obj_set_style_arc_width(obj, *w, LV_PART_INDICATOR);
            return 0;
        }
        case Prop::Wrap: {
            auto b = unset ? std::optional<bool>(true) : as_bool(v);
            if (!b) return wrong();
            lv_label_set_long_mode(obj, *b ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
            return 0;
        }
        case Prop::Name:
            if (!unset && !as_string(v)) return wrong();
            set_icon(n, v);
            return 0;
        case Prop::Src:
            if (!unset && !as_string(v)) return wrong();
            set_image(n, v);
            return 0;
        case Prop::Type: {
            const auto* s = as_string(v);
            if (!s && !unset) return wrong();
            n.bar = s && *s == "bar";
            lv_chart_set_type(obj, n.bar ? LV_CHART_TYPE_BAR : LV_CHART_TYPE_LINE);
            return 0;
        }
        case Prop::Series:
            if (!unset && !as_string(v)) return wrong();
            set_series(n, v);
            return kRangeDerived;
        default:
            // Items, row, draw and the handlers: they stay on the scene
            // task.
            return 0;
    }
}

void SceneView::set_text(Node& n, const PropValue& v) {
    char buf[40];
    const char* text =
        std::holds_alternative<std::monostate>(v) ? "" : text_of(v, buf, sizeof(buf));
    if (text == nullptr) return;
    switch (n.widget) {
        case Widget::Label:
            lv_label_set_text(n.obj, text);
            break;
        case Widget::Button:
            if (n.text == nullptr) {
                n.text = lv_label_create(n.obj);
                lv_obj_move_to_index(n.text, 0);
            }
            lv_label_set_text(n.text, text);
            break;
        case Widget::Checkbox:
            lv_checkbox_set_text(n.obj, text);
            break;
        case Widget::Input:
            // Rewriting the same text would move the cursor while typing.
            if (std::strcmp(lv_textarea_get_text(n.obj), text) != 0)
                lv_textarea_set_text(n.obj, text);
            break;
        default:
            break;
    }
}

// --- Derived state ---

void SceneView::update(Node& n, uint32_t what) {
    if (what & kSizeDerived) apply_size(n);
    if (what & kPadDerived) apply_pad(n);
    if (what & kAlignDerived) apply_flex_align(n);
    if (what & kRangeDerived) apply_range(n);
    if (what & kListDerived) apply_list_height(n);
    if ((what & kGridDerived) && n.widget == Widget::Grid && !building_) layout_grid(n);
}

void SceneView::apply_size(Node& n) {
    if (n.id == root_) {
        lv_obj_set_size(n.obj, shape_.width, shape_.height);
        return;
    }
    if (n.row_of != 0) {
        place_row(n);
        return;
    }
    Node* parent = find(n.parent);
    const Flow axis = parent ? parent->flow : Flow::None;
    const bool in_grid = parent && parent->widget == Widget::Grid;

    Size w = n.w, h = n.h;
    if (w.kind == SizeKind::Unset || h.kind == SizeKind::Unset) {
        Size def;
        if (n.widget == Widget::Scroll || n.widget == Widget::List || n.widget == Widget::Canvas)
            def.kind = SizeKind::Fill;
        else if (is_layout(n.widget))
            def.kind = SizeKind::Content;
        else if (n.widget == Widget::Spacer)
            def = {SizeKind::Px, 0};
        if (w.kind == SizeKind::Unset) w = def;
        if (h.kind == SizeKind::Unset) h = def;
    }

    int32_t grow = n.grow;
    const auto set_axis = [&](const Size& s, bool horizontal) {
        const bool main = (axis == Flow::Row) == horizontal && axis != Flow::None;
        // Without a local size the widget's class default applies again.
        const auto unset = [&] {
            lv_obj_remove_local_style_prop(n.obj, horizontal ? LV_STYLE_WIDTH : LV_STYLE_HEIGHT,
                                           LV_PART_MAIN);
        };
        switch (s.kind) {
            case SizeKind::Px:
                horizontal ? lv_obj_set_width(n.obj, s.px) : lv_obj_set_height(n.obj, s.px);
                break;
            case SizeKind::Content:
                horizontal ? lv_obj_set_width(n.obj, LV_SIZE_CONTENT)
                           : lv_obj_set_height(n.obj, LV_SIZE_CONTENT);
                break;
            case SizeKind::Fill:
                if (main) {
                    if (grow < 0) grow = 1;
                } else if (in_grid) {
                    unset();
                } else {
                    horizontal ? lv_obj_set_width(n.obj, lv_pct(100))
                               : lv_obj_set_height(n.obj, lv_pct(100));
                }
                break;
            case SizeKind::Unset:
                unset();
                break;
        }
    };
    set_axis(w, true);
    set_axis(h, false);
    if (grow < 0 && n.widget == Widget::Spacer) grow = 1;
    lv_obj_set_style_flex_grow(n.obj, static_cast<uint8_t>(std::max<int32_t>(grow, 0)),
                               LV_PART_MAIN);
    if (in_grid && !building_) layout_grid(*parent);
}

void SceneView::apply_pad(Node& n) {
    Insets pad;
    const bool root = n.id == root_;
    if (n.pad_given) {
        // Below the root, "safe" adds nothing: the root's own padding
        // already keeps everything inside it in the safe area.
        if (!n.pad_safe)
            pad = Insets{n.pad, n.pad, n.pad, n.pad};
        else if (root)
            pad = safe_;
    } else if (!root) {
        // Keeps the widget's own padding (a button's, an input's).
        if (is_layout(n.widget) || n.widget == Widget::Spacer || n.widget == Widget::List) return;
        lv_obj_remove_local_style_prop(n.obj, LV_STYLE_PAD_LEFT, LV_PART_MAIN);
        lv_obj_remove_local_style_prop(n.obj, LV_STYLE_PAD_TOP, LV_PART_MAIN);
        lv_obj_remove_local_style_prop(n.obj, LV_STYLE_PAD_RIGHT, LV_PART_MAIN);
        lv_obj_remove_local_style_prop(n.obj, LV_STYLE_PAD_BOTTOM, LV_PART_MAIN);
        return;
    }
    if (root) {
        pad.left = std::max(pad.left, safe_.left);
        pad.top = std::max(pad.top, safe_.top);
        pad.right = std::max(pad.right, safe_.right);
        pad.bottom = std::max(pad.bottom, safe_.bottom);
    }
    lv_obj_set_style_pad_left(n.obj, pad.left, LV_PART_MAIN);
    lv_obj_set_style_pad_top(n.obj, pad.top, LV_PART_MAIN);
    lv_obj_set_style_pad_right(n.obj, pad.right, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(n.obj, pad.bottom, LV_PART_MAIN);
}

void SceneView::apply_flex_align(Node& n) {
    if (n.widget == Widget::Stack) {
        place_in_stack(n);
    } else if (n.widget == Widget::Grid) {
        if (!building_) layout_grid(n);
    } else if (n.flow != Flow::None && n.widget != Widget::Button) {
        lv_obj_set_flex_align(n.obj, n.justify, n.align, n.align);
    }
}

lv_align_t SceneView::stack_align(const Node& stack) {
    if (stack.align == LV_FLEX_ALIGN_CENTER) return LV_ALIGN_CENTER;
    if (stack.align == LV_FLEX_ALIGN_END) return LV_ALIGN_BOTTOM_RIGHT;
    return LV_ALIGN_TOP_LEFT;
}

void SceneView::place_in_stack(Node& stack) {
    const lv_align_t where = stack_align(stack);
    uint32_t count = lv_obj_get_child_count(stack.obj);
    for (uint32_t i = 0; i < count; ++i)
        lv_obj_set_align(lv_obj_get_child(stack.obj, static_cast<int32_t>(i)), where);
}

void SceneView::layout_grid(Node& grid) {
    const int32_t cols = std::max<int32_t>(grid.cols, 1);
    const uint32_t count = lv_obj_get_child_count(grid.obj);
    const int32_t rows = static_cast<int32_t>((count + cols - 1) / cols);
    grid.grid.assign(static_cast<size_t>(cols + rows + 2), 0);
    for (int32_t c = 0; c < cols; ++c) grid.grid[c] = LV_GRID_FR(1);
    grid.grid[cols] = LV_GRID_TEMPLATE_LAST;
    for (int32_t r = 0; r < rows; ++r) grid.grid[cols + 1 + r] = LV_GRID_CONTENT;
    grid.grid[cols + 1 + rows] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(grid.obj, grid.grid.data(), grid.grid.data() + cols + 1);

    lv_grid_align_t place = LV_GRID_ALIGN_START;
    if (grid.align == LV_FLEX_ALIGN_CENTER)
        place = LV_GRID_ALIGN_CENTER;
    else if (grid.align == LV_FLEX_ALIGN_END)
        place = LV_GRID_ALIGN_END;
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(grid.obj, static_cast<int32_t>(i));
        lv_grid_align_t x = place, y = place;
        // A child that fills stretches across its cell.
        if (const auto* c = static_cast<const Node*>(lv_obj_get_user_data(child))) {
            if (c->w.kind == SizeKind::Fill) x = LV_GRID_ALIGN_STRETCH;
            if (c->h.kind == SizeKind::Fill) y = LV_GRID_ALIGN_STRETCH;
        }
        lv_obj_set_grid_cell(child, x, static_cast<int32_t>(i % cols), 1, y,
                             static_cast<int32_t>(i / cols), 1);
    }
}

bool SceneView::direct_range(const Node& n) {
    const auto whole = [](double d) {
        return std::trunc(d) == d && std::fabs(d) <= static_cast<double>(kDirectLimit);
    };
    return whole(n.min) && whole(n.max);
}

void SceneView::apply_range(Node& n) {
    if (n.widget == Widget::Chart) {
        apply_chart(n);
        return;
    }
    if (n.widget != Widget::Slider && n.widget != Widget::Bar && n.widget != Widget::Arc) return;
    int32_t lo = 0, hi = kScaledSteps, val = 0;
    if (direct_range(n)) {
        lo = static_cast<int32_t>(n.min);
        hi = static_cast<int32_t>(n.max);
        double v = std::clamp(std::round(n.value), static_cast<double>(std::min(lo, hi)),
                              static_cast<double>(std::max(lo, hi)));
        val = static_cast<int32_t>(v);
    } else {
        double span = n.max - n.min;
        double f = span == 0 ? 0.0 : (n.value - n.min) / span;
        f = std::isnan(f) ? 0.0 : std::clamp(f, 0.0, 1.0);
        val = static_cast<int32_t>(std::lround(f * kScaledSteps));
    }
    switch (n.widget) {
        case Widget::Slider:
            lv_slider_set_range(n.obj, lo, hi);
            lv_slider_set_value(n.obj, val, LV_ANIM_OFF);
            break;
        case Widget::Bar:
            lv_bar_set_range(n.obj, lo, hi);
            lv_bar_set_value(n.obj, val, LV_ANIM_OFF);
            break;
        default:
            lv_arc_set_range(n.obj, lo, hi);
            lv_arc_set_value(n.obj, val);
            break;
    }
}

// --- Other ops ---

void SceneView::apply_scroll(const ScrollTo& s) {
    Node* n = find(s.node);
    if (n == nullptr) {
        WLOG_D(kTag, "scroll for node %" PRIu32 " that is gone", s.node);
        return;
    }
    if (n->widget == Widget::List) {
        lv_obj_update_layout(n->obj);
        int64_t y = static_cast<int64_t>(s.index) * n->row_h;
        // The furthest the list can scroll: where it is plus what is left
        // below.
        int64_t max_y =
            lv_obj_get_scroll_y(n->obj) + std::max<int32_t>(0, lv_obj_get_scroll_bottom(n->obj));
        y = std::clamp<int64_t>(y, 0, std::max<int64_t>(max_y, 0));
        lv_obj_scroll_to_y(n->obj, static_cast<int32_t>(y), LV_ANIM_OFF);
    } else if (n->widget == Widget::Scroll) {
        uint32_t count = lv_obj_get_child_count(n->obj);
        if (count == 0) return;
        uint32_t index = std::min(s.index, count - 1);
        lv_obj_update_layout(n->obj);
        lv_obj_scroll_to_view(lv_obj_get_child(n->obj, static_cast<int32_t>(index)), LV_ANIM_OFF);
    } else {
        WLOG_D(kTag, "scroll for node %" PRIu32 " that does not scroll", s.node);
    }
}

void SceneView::apply_focus(const Focus& f) {
    Node* n = find(f.node);
    if (n == nullptr || n->widget != Widget::Input) {
        WLOG_D(kTag, "focus for node %" PRIu32 " that is not an input", f.node);
        return;
    }
    if (lv_obj_get_group(n->obj) != group_) lv_group_add_obj(group_, n->obj);
    lv_group_focus_obj(n->obj);
    system_.show_keyboard(n->obj);
}

// --- Events ---

void SceneView::report(Node& n, UiEvent ev, PropValue value) {
    if (cb_.on_event) cb_.on_event(n.id, ev, std::move(value));
}

void SceneView::event_cb(lv_event_t* e) {
    auto* n = static_cast<Node*>(lv_event_get_user_data(e));
    SceneView* view = n->view;
    if (view->applying_ > 0) return;
    switch (lv_event_get_code(e)) {
        case LV_EVENT_PRESSED:
            n->long_pressed = false;
            break;
        case LV_EVENT_LONG_PRESSED:
            if (n->events & kEventLongPress) {
                n->long_pressed = true;
                view->report(*n, UiEvent::LongPress, PropValue{});
            }
            break;
        case LV_EVENT_CLICKED:
            if (n->widget == Widget::Input) view->system_.show_keyboard(n->obj);
            if ((n->events & kEventClick) && !n->long_pressed)
                view->report(*n, UiEvent::Click, PropValue{});
            break;
        case LV_EVENT_FOCUSED:
            view->system_.show_keyboard(n->obj);
            view->report(*n, UiEvent::Focus, PropValue{});
            break;
        case LV_EVENT_VALUE_CHANGED:
            view->report_change(*n);
            break;
        default:
            break;
    }
}

void SceneView::report_change(Node& n) {
    switch (n.widget) {
        case Widget::Input: {
            // Copied straight into PSRAM; past the scene's string limit
            // the scene could not take it anyway.
            const char* text = lv_textarea_get_text(n.obj);
            size_t len = std::strlen(text);
            if (len > kMaxPropStringBytes) {
                warn(n, "ui.input: value too large");
                break;
            }
            report(n, UiEvent::Change, PropValue{os::LargeString(text, len)});
            break;
        }
        case Widget::Checkbox:
        case Widget::Switch:
            report(n, UiEvent::Change, PropValue{lv_obj_has_state(n.obj, LV_STATE_CHECKED)});
            break;
        case Widget::Slider: {
            int32_t v = lv_slider_get_value(n.obj);
            if (direct_range(n)) {
                n.value = v;
                report(n, UiEvent::Change, PropValue{static_cast<int64_t>(v)});
            } else {
                n.value = n.min + (n.max - n.min) * (static_cast<double>(v) / kScaledSteps);
                report(n, UiEvent::Change, PropValue{n.value});
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace wisp::scene
