// SceneView: ui.canvas (see wisp/scene/scene_view.hpp).

#include <algorithm>
#include <cinttypes>
#include <span>

#include "scene_view_internal.hpp"
#include "wisp/core/log.hpp"
#include "wisp/scene/theme.hpp"

namespace wisp::scene {

using namespace view;

namespace {

int32_t coord(int32_t v) { return std::clamp(v, -kMaxDrawCoord, kMaxDrawCoord); }

// Degrees on [0, 360).
int32_t angle(int32_t a) { return ((a % 360) + 360) % 360; }

void draw_command(lv_layer_t* layer, std::span<const uint8_t> bytes, const DrawCommand& c,
                  int32_t w, int32_t h) {
    const lv_color_t color = lv_color_hex(c.rgb & 0xFFFFFF);
    const int32_t width = std::clamp<int32_t>(c.width, 0, 512);
    const auto rect = [&](lv_area_t area, int32_t radius, bool fill) {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.radius = radius;
        if (fill) {
            d.bg_color = color;
            d.bg_opa = LV_OPA_COVER;
        } else {
            d.bg_opa = LV_OPA_TRANSP;
            d.border_color = color;
            d.border_width = width;
            d.border_opa = LV_OPA_COVER;
        }
        lv_draw_rect(layer, &d, &area);
    };
    switch (c.op) {
        case DrawOp::Fill:
            rect({0, 0, w - 1, h - 1}, 0, true);
            break;
        case DrawOp::Line: {
            lv_draw_line_dsc_t d;
            lv_draw_line_dsc_init(&d);
            d.p1 = {coord(c.v[0]), coord(c.v[1])};
            d.p2 = {coord(c.v[2]), coord(c.v[3])};
            d.color = color;
            d.width = width;
            d.round_start = 1;
            d.round_end = 1;
            lv_draw_line(layer, &d);
            break;
        }
        case DrawOp::Circle: {
            const int32_t x = coord(c.v[0]), y = coord(c.v[1]);
            const int32_t r = std::clamp<int32_t>(c.v[2], 0, kMaxDrawCoord);
            rect({x - r, y - r, x + r, y + r}, LV_RADIUS_CIRCLE, c.fill != 0);
            break;
        }
        case DrawOp::Arc: {
            lv_draw_arc_dsc_t d;
            lv_draw_arc_dsc_init(&d);
            d.center = {coord(c.v[0]), coord(c.v[1])};
            d.radius = static_cast<uint16_t>(std::clamp<int32_t>(c.v[2], 0, UINT16_MAX));
            const int32_t sweep = c.v[4] - c.v[3];
            d.start_angle = angle(c.v[3]);
            // A sweep of a whole turn or more is the full circle.
            d.end_angle = sweep >= 360 || sweep <= -360 ? d.start_angle + 360 : angle(c.v[4]);
            d.width = width;
            d.color = color;
            lv_draw_arc(layer, &d);
            break;
        }
        case DrawOp::Rect: {
            const int32_t x = coord(c.v[0]), y = coord(c.v[1]);
            const int32_t rw = std::clamp<int32_t>(c.v[2], 0, kMaxDrawCoord);
            const int32_t rh = std::clamp<int32_t>(c.v[3], 0, kMaxDrawCoord);
            if (rw == 0 || rh == 0) break;
            rect({x, y, x + rw - 1, y + rh - 1}, std::clamp<int32_t>(c.radius, 0, 4096),
                 c.fill != 0);
            break;
        }
        case DrawOp::Text: {
            std::string_view text = draw_text(bytes, c);
            if (text.empty()) break;
            lv_draw_label_dsc_t d;
            lv_draw_label_dsc_init(&d);
            d.text = text.data();
            d.text_length = static_cast<uint32_t>(text.size());
            d.text_local = 1;  // copied: the bytes are not NUL-terminated
            d.color = color;
            d.font = theme_font(c.font < std::size(kDrawFontSizes) ? kDrawFontSizes[c.font] : "md");
            const int32_t x = coord(c.v[0]), y = coord(c.v[1]);
            lv_area_t area{x, y, std::max(x, w - 1), std::max(y, h - 1)};
            lv_draw_label(layer, &d, &area);
            break;
        }
    }
}

}  // namespace

void SceneView::apply_canvas(const CanvasDraw& d) {
    Node* n = find(d.canvas);
    if (n == nullptr || n->widget != Widget::Canvas) {
        WLOG_D(kTag, "drawing for node %" PRIu32 " that is not a canvas", d.canvas);
        return;
    }
    std::span<const uint8_t> bytes(d.commands);
    if (draw_command_count(bytes) == 0 && bytes.size() != kDrawHeaderBytes) {
        WLOG_W(kTag, "malformed drawing for canvas %" PRIu32 " dropped", d.canvas);
        return;
    }
    n->draw.assign(d.commands.begin(), d.commands.end());
    n->canvas_drawn = false;
    // A canvas built in the same batch has no size yet; if this gives it
    // one, canvas_event_cb() draws it.
    lv_obj_update_layout(n->obj);
    if (!n->canvas_drawn) draw_canvas(*n);
}

void SceneView::canvas_event_cb(lv_event_t* e) {
    auto* n = static_cast<Node*>(lv_event_get_user_data(e));
    const lv_image_header_t& h = n->canvas_buf.header;
    if (n->canvas_mem.p != nullptr &&
        static_cast<int32_t>(h.w) == lv_obj_get_content_width(n->obj) &&
        static_cast<int32_t>(h.h) == lv_obj_get_content_height(n->obj))
        return;
    n->view->draw_canvas(*n);
}

// Frees the canvas's buffer; the canvas shows nothing until it gets one.
void SceneView::release_canvas(Node& n) {
    if (n.canvas_mem.p == nullptr) return;
    lv_image_set_src(n.obj, nullptr);
    n.canvas_buf = {};
    n.canvas_mem = {};
    media_bytes_ -= n.canvas_bytes;
    n.canvas_bytes = 0;
}

void SceneView::draw_canvas(Node& n) {
    n.canvas_drawn = true;
    const int32_t w = lv_obj_get_content_width(n.obj);
    const int32_t h = lv_obj_get_content_height(n.obj);
    if (w <= 0 || h <= 0) return;
    const lv_color_format_t cf = lv_display_get_color_format(lv_obj_get_display(n.obj));
    lv_image_header_t& header = n.canvas_buf.header;
    if (n.canvas_mem.p == nullptr || static_cast<int32_t>(header.w) != w ||
        static_cast<int32_t>(header.h) != h || header.cf != cf) {
        const uint64_t pixels = static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
        const uint32_t stride = lv_draw_buf_width_to_stride(static_cast<uint32_t>(w), cf);
        const uint64_t size = static_cast<uint64_t>(stride) * static_cast<uint64_t>(h);
        // The old buffer is about to be redrawn anyway: it goes first, so
        // the two never take memory at once.
        release_canvas(n);
        if (pixels > kMaxCanvasPixels || size > kMaxMediaBytes - media_bytes_) {
            if (!n.canvas_refused) warn(n, "ui.canvas: too large");
            n.canvas_refused = true;
            return;
        }
        LargeMem mem;
        mem.p = os::alloc(static_cast<size_t>(size), os::Mem::Large);
        if (mem.p == nullptr) {
            WLOG_W(kTag, "out of memory for a %" PRId32 "x%" PRId32 " canvas", w, h);
            return;
        }
        if (lv_draw_buf_init(&n.canvas_buf, static_cast<uint32_t>(w), static_cast<uint32_t>(h), cf,
                             stride, mem.p, static_cast<uint32_t>(size)) != LV_RESULT_OK)
            return;
        lv_canvas_set_draw_buf(n.obj, &n.canvas_buf);
        n.canvas_mem = std::move(mem);
        n.canvas_bytes = static_cast<size_t>(size);
        media_bytes_ += n.canvas_bytes;
        n.canvas_refused = false;
    }
    lv_draw_buf_clear(&n.canvas_buf, nullptr);
    std::span<const uint8_t> bytes(n.draw);
    const size_t count = draw_command_count(bytes);
    if (count > 0) {
        lv_layer_t layer;
        lv_canvas_init_layer(n.obj, &layer);
        for (size_t i = 0; i < count; ++i) {
            DrawCommand c;
            if (!read_draw_command(bytes, i, c)) continue;
            draw_command(&layer, bytes, c, w, h);
        }
        lv_canvas_finish_layer(n.obj, &layer);
    }
    lv_obj_invalidate(n.obj);
}

}  // namespace wisp::scene
