// WispView (wisp/ui/wisp_view.hpp).

#include "wisp/ui/wisp_view.hpp"

#include <algorithm>
#include <new>
#include <utility>

#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::ui {

// The model and its two buffers, all in Mem::Large: at 45% of a 480 px
// screen that is 216 x 270, 58 KB of luminance and 117 KB of frame.
struct WispView::Impl {
    WispModel model;
    os::LargeBuffer lum;
    os::LargeBuffer out;
    lv_image_dsc_t dsc{};

    Impl(int size_px, uint32_t seed) : model(size_px, seed) {}
};

int wisp_size_px(const hal::Area& safe, int percent) {
    const int w = safe.x2 - safe.x1 + 1;
    const int h = safe.y2 - safe.y1 + 1;
    return std::max(std::min(w, h), 0) * percent / 100;
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
    Impl* impl = os::make_large<Impl>(size_px, seed).release();
    if (impl == nullptr) return false;
    const int w = impl->model.width();
    const int h = impl->model.height();
    const size_t n = static_cast<size_t>(w) * static_cast<size_t>(h);
    impl->lum = os::LargeBuffer::allocate(n);
    impl->out = os::LargeBuffer::allocate(n * 2);
    if (!impl->lum.ok() || !impl->out.ok()) {
        impl->~Impl();
        os::dealloc(impl);
        return false;
    }

    // Render straight in the display's byte order, so drawing the frame
    // is a plain copy.
    lv_display_t* disp = lv_obj_get_display(obj_);
    const lv_color_format_t cf =
        disp != nullptr ? lv_display_get_color_format(disp) : LV_COLOR_FORMAT_RGB565;
    const bool swapped = cf == LV_COLOR_FORMAT_RGB565_SWAPPED;
    impl->model.set_swapped_palette(swapped);

    if (impl_ != nullptr) {
        const WispModel& old = impl_->model;
        impl->model.set_mood(old.mood());
        impl->model.set_busy(busy_);
        impl->model.set_level(level_);
        impl_->~Impl();
        os::dealloc(impl_);
    }
    impl_ = impl;
    // Draw the first frame now, so the object never shows stale memory.
    impl_->model.render({impl_->lum.data(), n},
                        {reinterpret_cast<uint16_t*>(impl_->out.data()), n});

    lv_image_dsc_t& d = impl_->dsc;
    d = lv_image_dsc_t{};
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
    d.header.w = static_cast<uint32_t>(w);
    d.header.h = static_cast<uint32_t>(h);
    d.header.stride = static_cast<uint32_t>(w * 2);
    d.data = impl_->out.data();
    d.data_size = static_cast<uint32_t>(n * 2);

    lv_obj_set_size(obj_, w, h);
    lv_obj_invalidate(obj_);
    return true;
}

const WispModel& WispView::model() const { return impl_->model; }

void WispView::set_mood(WispMood mood) {
    impl_->model.set_mood(mood);
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
    if (paused_) return;
    const uint32_t period = impl_->model.frame_interval_ms();
    if (timer_ == nullptr) {
        timer_ = lv_timer_create(on_timer, period, this);
        last_ms_ = os::now_ms();
    } else if (period != period_) {
        lv_timer_set_period(timer_, period);
    }
    period_ = period;
}

void WispView::frame() {
    const uint64_t now = os::now_ms();
    const auto dt = static_cast<uint32_t>(std::min<uint64_t>(now - last_ms_, 1000));
    last_ms_ = now;

    WispModel& m = impl_->model;
    const size_t n = static_cast<size_t>(m.width()) * static_cast<size_t>(m.height());
    const uint64_t t0 = os::now_us();
    m.step(dt);
    const hal::Area dirty =
        m.render({impl_->lum.data(), n}, {reinterpret_cast<uint16_t*>(impl_->out.data()), n});
    wisp_perf_record(now, static_cast<uint32_t>(os::now_us() - t0));
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

void WispView::on_timer(lv_timer_t* t) {
    static_cast<WispView*>(lv_timer_get_user_data(t))->frame();
}

void WispView::on_draw(lv_event_t* e) {
    auto* v = static_cast<WispView*>(lv_event_get_user_data(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t coords;
    lv_obj_get_coords(v->obj_, &coords);
    lv_draw_image_dsc_t d;
    lv_draw_image_dsc_init(&d);
    d.src = &v->impl_->dsc;
    lv_draw_image(layer, &d, &coords);
}

void WispView::on_delete(lv_event_t* e) {
    auto* v = static_cast<WispView*>(lv_event_get_user_data(e));
    v->obj_ = nullptr;
    v->~WispView();
    os::dealloc(v);
}

}  // namespace wisp::ui
