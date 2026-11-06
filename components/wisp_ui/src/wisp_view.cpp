// WispView (wisp/ui/wisp_view.hpp).

#include "wisp/ui/wisp_view.hpp"

#include <algorithm>
#include <new>
#include <utility>

#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::ui {

namespace {

// Q8 pixels to whole pixels, rounded.
int32_t to_px(int32_t q8) { return (q8 + 128) >> 8; }

lv_area_t capsule_area(const WispShape::Capsule& c, int32_t ox, int32_t oy) {
    return {ox + to_px(std::min(c.a.x, c.b.x) - c.r), oy + to_px(std::min(c.a.y, c.b.y) - c.r),
            ox + to_px(std::max(c.a.x, c.b.x) + c.r) - 1,
            oy + to_px(std::max(c.a.y, c.b.y) + c.r) - 1};
}

// Two colours: the shape's pixel runs, no anti-aliasing. Each fill is a
// draw task, so runs that repeat on the rows below (an eye's straight
// middle, steep stroke) are merged into one rectangle first.
void draw_spans(lv_layer_t* layer, const WispShape& s, int32_t ox, int32_t oy, lv_color_t colour) {
    struct Rect {
        int x1, x2, y1, y2;
    };
    constexpr int kOpen = 16;
    struct Ctx {
        lv_layer_t* layer;
        lv_draw_fill_dsc_t dsc;
        int32_t ox, oy;
        Rect open[kOpen];
        int n;

        void emit(const Rect& r) {
            const lv_area_t a{ox + r.x1, oy + r.y1, ox + r.x2, oy + r.y2};
            lv_draw_fill(layer, &dsc, &a);
        }
        // Emits the rectangles that row y could no longer extend.
        void flush_before(int y) {
            int m = 0;
            for (int i = 0; i < n; ++i) {
                if (open[i].y2 < y - 1) {
                    emit(open[i]);
                } else {
                    open[m++] = open[i];
                }
            }
            n = m;
        }
        void run(int y, int x1, int x2) {
            flush_before(y);
            for (int i = 0; i < n; ++i) {
                if (open[i].y2 == y - 1 && open[i].x1 == x1 && open[i].x2 == x2) {
                    open[i].y2 = y;
                    return;
                }
            }
            if (n == kOpen) {
                emit(open[0]);
                std::copy(open + 1, open + n, open);
                --n;
            }
            open[n++] = {x1, x2, y, y};
        }
    };
    Ctx ctx{layer, {}, ox, oy, {}, 0};
    lv_draw_fill_dsc_init(&ctx.dsc);
    ctx.dsc.color = colour;
    ctx.dsc.opa = LV_OPA_COVER;
    ctx.dsc.radius = 0;
    for_each_span(s, layer->_clip_area.y1 - oy, layer->_clip_area.y2 - oy, &ctx,
                  [](void* p, int y, int x1, int x2) { static_cast<Ctx*>(p)->run(y, x1, x2); });
    for (int i = 0; i < ctx.n; ++i) ctx.emit(ctx.open[i]);
}

}  // namespace

// The model and the outline's points as LVGL takes them.
struct WispView::Impl {
    WispModel model;
    lv_point_precise_t points[WispShape::kMaxOutline + 1]{};

    Impl(int size_px, uint32_t seed, hal::PanelKind panel) : model(size_px, seed, panel) {}
};

int wisp_size_px(const hal::Area& safe, int percent) {
    const int w = safe.x2 - safe.x1 + 1;
    const int h = safe.y2 - safe.y1 + 1;
    return std::max(std::min(w, h), 0) * percent / 100;
}

lv_color_t wisp_colour(hal::PanelKind panel) {
    switch (panel) {
        case hal::PanelKind::Mono:
            return lv_color_white();
        case hal::PanelKind::EPaper:
            return lv_color_black();
        case hal::PanelKind::Lcd:
        case hal::PanelKind::Amoled:
            break;
    }
    return lv_color_hex(0x58B4FF);  // 幽蓝, the wisp's blue
}

lv_color_t wisp_background(hal::PanelKind panel) {
    return panel == hal::PanelKind::EPaper ? lv_color_white() : lv_color_black();
}

WispView::WispView(lv_obj_t* obj) : obj_(obj) {}

WispView::~WispView() {
    if (timer_ != nullptr) lv_timer_delete(timer_);
    if (impl_ != nullptr) {
        impl_->~Impl();
        os::dealloc(impl_);
    }
}

WispView* WispView::create(lv_obj_t* parent, int size_px) {
    void* mem = os::alloc(sizeof(WispView), os::Mem::Large);
    if (mem == nullptr) return nullptr;
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    auto* v = ::new (mem) WispView(obj);
    if (!v->build(size_px)) {
        v->~WispView();
        os::dealloc(mem);
        lv_obj_delete(obj);
        return nullptr;
    }
    lv_obj_center(obj);
    lv_obj_add_event_cb(obj, on_draw, LV_EVENT_DRAW_MAIN, v);
    lv_obj_add_event_cb(obj, on_delete, LV_EVENT_DELETE, v);
    v->schedule();
    return v;
}

bool WispView::build(int size_px) {
    size_px = std::max(size_px, 16);
    const auto seed = static_cast<uint32_t>(os::now_us()) ^ 0x9e3779b9u;
    const hal::PanelKind panel = display_panel(lv_obj_get_display(obj_));
    Impl* impl = os::make_large<Impl>(size_px, seed, panel).release();
    if (impl == nullptr) return false;
    if (impl_ != nullptr) {
        impl->model.set_mood(impl_->model.mood());
        impl->model.set_busy(busy_);
        impl->model.set_level(level_);
        impl_->~Impl();
        os::dealloc(impl_);
    }
    impl_ = impl;
    impl_->model.settle();
    impl_->model.build();
    ++frames_;
    lv_obj_set_size(obj_, impl_->model.width(), impl_->model.height());
    lv_obj_invalidate(obj_);
    return true;
}

const WispModel& WispView::model() const { return impl_->model; }

void WispView::set_mood(WispMood mood) {
    if (mood == impl_->model.mood()) return;
    impl_->model.set_mood(mood);
    if (impl_->model.frame_interval_ms() == 0) {
        still_frame();
        return;
    }
    schedule();
}

void WispView::set_level(uint8_t level) {
    level_ = level;
    impl_->model.set_level(level);
}

void WispView::set_busy(bool fast) {
    busy_ = fast;
    impl_->model.set_busy(fast);
}

void WispView::set_paused(bool paused) {
    if (paused == paused_) return;
    paused_ = paused;
    if (paused_) {
        if (timer_ != nullptr) {
            lv_timer_delete(timer_);
            timer_ = nullptr;
        }
        return;
    }
    if (stale_) still_frame();
    schedule();
}

void WispView::set_size(int size_px) {
    if (size_px == impl_->model.width()) return;
    const int32_t old_w = lv_obj_get_width(obj_);
    const int32_t old_h = lv_obj_get_height(obj_);
    lv_obj_invalidate(obj_);
    if (!build(size_px)) return;
    // A centred object stays centred by itself; any other keeps its centre
    // where it was.
    if (lv_obj_get_style_align(obj_, LV_PART_MAIN) != LV_ALIGN_CENTER) {
        lv_obj_set_pos(obj_, lv_obj_get_x_aligned(obj_) + (old_w - impl_->model.width()) / 2,
                       lv_obj_get_y_aligned(obj_) + (old_h - impl_->model.height()) / 2);
    }
}

void WispView::schedule() {
    const uint32_t period = impl_->model.frame_interval_ms();
    if (paused_ || period == 0) {
        if (timer_ != nullptr) {
            lv_timer_delete(timer_);
            timer_ = nullptr;
        }
        return;
    }
    if (timer_ == nullptr) {
        timer_ = lv_timer_create(on_timer, period, this);
        last_ms_ = os::now_ms();
    } else if (period != period_) {
        lv_timer_set_period(timer_, period);
    }
    period_ = period;
}

// A panel that shows still pictures: the new mood, settled, drawn once.
void WispView::still_frame() {
    if (paused_) {
        stale_ = true;
        return;
    }
    stale_ = false;
    const uint64_t t0 = os::now_us();
    impl_->model.settle();
    impl_->model.build();
    wisp_perf_record(os::now_ms(), static_cast<uint32_t>(os::now_us() - t0));
    ++frames_;
    lv_obj_invalidate(obj_);
}

void WispView::frame() {
    const uint64_t now = os::now_ms();
    const auto dt = static_cast<uint32_t>(std::min<uint64_t>(now - last_ms_, 1000));
    last_ms_ = now;
    // Hidden, or on a screen not shown: nothing to step or draw. The timer
    // keeps its period, so this costs one check per tick.
    if (!lv_obj_is_visible(obj_)) return;

    WispModel& m = impl_->model;
    const uint64_t t0 = os::now_us();
    m.step(dt);
    const hal::Area dirty = m.build();
    wisp_perf_record(now, static_cast<uint32_t>(os::now_us() - t0) + draw_us_);
    draw_us_ = 0;
    ++frames_;

    if (dirty.x2 >= dirty.x1 && dirty.y2 >= dirty.y1) {
        lv_area_t coords;
        lv_obj_get_coords(obj_, &coords);
        lv_area_t a{coords.x1 + dirty.x1, coords.y1 + dirty.y1, coords.x1 + dirty.x2,
                    coords.y1 + dirty.y2};
        lv_obj_invalidate_area(obj_, &a);
    }
    schedule();
}

void WispView::draw(lv_layer_t* layer) {
    const WispShape& s = impl_->model.shape();
    const hal::PanelKind panel = impl_->model.panel();
    lv_area_t coords;
    lv_obj_get_coords(obj_, &coords);
    const int32_t ox = coords.x1, oy = coords.y1;
    const lv_color_t colour = wisp_colour(panel);

    if (!hal::panel_is_colour(panel)) {
        draw_spans(layer, s, ox, oy, colour);
        return;
    }

    const int32_t width = std::max<int32_t>(to_px(s.stroke), 1);
    // Dimmed by mixing towards the background rather than by opacity: each
    // segment is drawn on its own with round caps, and translucent caps
    // would darken where they overlap.
    const lv_color_t stroke_colour = lv_color_mix(colour, wisp_background(panel), s.opacity);

    // The outline, closed. LVGL draws each segment on its own; a round end
    // on each makes every joint round (the last segment ends on the first
    // point).
    for (int i = 0; i < s.outline_n; ++i) {
        const WispPoint& p = s.outline[static_cast<size_t>(i)];
        impl_->points[i] = {ox + to_px(p.x), oy + to_px(p.y)};
    }
    impl_->points[s.outline_n] = impl_->points[0];
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = stroke_colour;
    line.width = width;
    line.round_end = 1;
    line.points = impl_->points;
    line.point_cnt = s.outline_n + 1;
    lv_draw_line(layer, &line);

    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = colour;
    fill.opa = LV_OPA_COVER;
    fill.radius = LV_RADIUS_CIRCLE;

    // The face keeps full brightness.
    if (s.chevrons) {
        lv_draw_line_dsc_t ch;
        lv_draw_line_dsc_init(&ch);
        ch.color = colour;
        ch.width = width;
        ch.round_start = 1;
        ch.round_end = 1;
        for (const auto& c : s.chevron) {
            ch.p1 = {ox + to_px(c[0].x), oy + to_px(c[0].y)};
            ch.p2 = {ox + to_px(c[1].x), oy + to_px(c[1].y)};
            lv_draw_line(layer, &ch);
            ch.p1 = ch.p2;
            ch.p2 = {ox + to_px(c[2].x), oy + to_px(c[2].y)};
            lv_draw_line(layer, &ch);
        }
    } else {
        for (const auto& e : s.eyes) {
            const lv_area_t a = capsule_area(e, ox, oy);
            lv_draw_fill(layer, &fill, &a);
        }
    }
    for (int i = 0; i < s.spark_n; ++i) {
        const lv_area_t a = capsule_area(s.sparks[static_cast<size_t>(i)], ox, oy);
        lv_draw_fill(layer, &fill, &a);
    }
}

void WispView::on_timer(lv_timer_t* t) {
    static_cast<WispView*>(lv_timer_get_user_data(t))->frame();
}

void WispView::on_draw(lv_event_t* e) {
    auto* v = static_cast<WispView*>(lv_event_get_user_data(e));
    const uint64_t t0 = os::now_us();
    v->draw(lv_event_get_layer(e));
    v->draw_us_ += static_cast<uint32_t>(os::now_us() - t0);
}

void WispView::on_delete(lv_event_t* e) {
    auto* v = static_cast<WispView*>(lv_event_get_user_data(e));
    v->obj_ = nullptr;
    v->~WispView();
    os::dealloc(v);
}

}  // namespace wisp::ui
