// SceneView: ui.chart, ui.image and ui.icon (see wisp/scene/scene_view.hpp).

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstring>

#include "lvgl_private.h"  // lv_image_decoder_dsc_t
#include "scene_view_internal.hpp"
#include "wisp/core/log.hpp"
#include "wisp/scene/theme.hpp"

namespace wisp::scene {

using namespace view;

namespace {

// The series colours, in series order.
constexpr std::string_view kSeriesColors[kMaxSeries] = {"accent", "ok", "warn", "danger"};

struct IconSymbol {
    std::string_view name;
    const char* symbol;
};

// Every name of the schema's `name` prop (schema.cpp).
constexpr IconSymbol kIconSymbols[] = {
    {"home", LV_SYMBOL_HOME},
    {"settings", LV_SYMBOL_SETTINGS},
    {"wifi", LV_SYMBOL_WIFI},
    {"battery", LV_SYMBOL_BATTERY_FULL},
    {"bell", LV_SYMBOL_BELL},
    {"play", LV_SYMBOL_PLAY},
    {"pause", LV_SYMBOL_PAUSE},
    {"stop", LV_SYMBOL_STOP},
    {"prev", LV_SYMBOL_PREV},
    {"next", LV_SYMBOL_NEXT},
    {"plus", LV_SYMBOL_PLUS},
    {"minus", LV_SYMBOL_MINUS},
    {"ok", LV_SYMBOL_OK},
    {"close", LV_SYMBOL_CLOSE},
    {"trash", LV_SYMBOL_TRASH},
    {"edit", LV_SYMBOL_EDIT},
    {"refresh", LV_SYMBOL_REFRESH},
    {"left", LV_SYMBOL_LEFT},
    {"right", LV_SYMBOL_RIGHT},
    {"up", LV_SYMBOL_UP},
    {"down", LV_SYMBOL_DOWN},
    {"upload", LV_SYMBOL_UPLOAD},
    {"download", LV_SYMBOL_DOWNLOAD},
    {"save", LV_SYMBOL_SAVE},
    {"list", LV_SYMBOL_LIST},
    {"image", LV_SYMBOL_IMAGE},
    {"audio", LV_SYMBOL_AUDIO},
    {"warning", LV_SYMBOL_WARNING},
};

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

// Smaller than any PNG (signature and IHDR chunk) or JPEG. LVGL's PNG
// decoder reads the size at bytes 16..23 before it checks the length.
constexpr size_t kMinImageBytes = 8 + 25;

}  // namespace

void SceneView::warn(Node& n, std::string_view message) {
    WLOG_W(kTag, "node %" PRIu32 ": %.*s", n.id, static_cast<int>(message.size()), message.data());
    if (cb_.on_warning) cb_.on_warning(n.id, message);
}

// --- Chart ---

void SceneView::set_series(Node& n, const PropValue& v) {
    const auto* bytes = as_string(v);
    if (bytes == nullptr) {
        n.series.clear();
        return;
    }
    if (!decode_series(*bytes, n.series))
        WLOG_D(kTag, "node %" PRIu32 ": malformed series dropped", n.id);
}

void SceneView::apply_chart(Node& n) {
    lv_obj_t* chart = n.obj;
    while (lv_chart_series_t* s = lv_chart_get_series_next(chart, nullptr))
        lv_chart_remove_series(chart, s);
    size_t points = 1;
    for (const auto& s : n.series) points = std::max(points, s.size());
    lv_chart_set_point_count(chart, static_cast<uint32_t>(points));
    // Values are placed on 0..kScaledSteps between min and max, so any
    // range (fractions, huge numbers) fits LVGL's int32 axis.
    const double span = n.max - n.min;
    for (size_t k = 0; k < n.series.size() && k < kMaxSeries; ++k) {
        lv_chart_series_t* ser =
            lv_chart_add_series(chart, theme_color(kSeriesColors[k]), LV_CHART_AXIS_PRIMARY_Y);
        if (ser == nullptr) break;
        int32_t* y = lv_chart_get_series_y_array(chart, ser);
        const auto& values = n.series[k];
        for (size_t i = 0; i < points; ++i) {
            if (i >= values.size() || std::isnan(values[i])) {
                y[i] = LV_CHART_POINT_NONE;
                continue;
            }
            double f = span == 0 ? 0.0 : (finite(values[i]) - n.min) / span;
            f = std::isnan(f) ? 0.0 : std::clamp(f, 0.0, 1.0);
            y[i] = static_cast<int32_t>(std::lround(f * kScaledSteps));
        }
    }
    lv_chart_refresh(chart);
}

// --- Image ---

void SceneView::set_image(Node& n, const PropValue& v) {
    // Shown from n.image: the old pixels go only once the image no longer
    // points at them.
    lv_image_set_src(n.obj, nullptr);
    n.image = {};
    n.image_mem = {};
    media_bytes_ -= n.image_bytes;
    n.image_bytes = 0;
    const auto* path = as_string(v);
    if (path == nullptr) return;

    const auto message = [&](const char* what) {
        os::LargeString m("ui.image: ");
        m += what;
        m += " '";
        m += *path;
        m += "'";
        warn(n, m);
    };
    const os::LargeBuffer* file = nullptr;
    if (assets_)
        for (const auto& [name, data] : assets_->files)
            if (name == std::string_view(*path)) file = &data;
    if (file == nullptr) {
        message("no asset");
        return;
    }
    const bool jpeg = ends_with(*path, ".jpg") || ends_with(*path, ".jpeg");
    if (file->size() < kMinImageBytes) {
        message("cannot decode");
        return;
    }

    // The package bytes as an image source LVGL's decoders recognize by
    // their content. An unknown colour format keeps the built-in decoder
    // (LVGL's own image format) from claiming them.
    lv_image_dsc_t src{};
    src.header.magic = LV_IMAGE_HEADER_MAGIC;
    src.header.cf = LV_COLOR_FORMAT_UNKNOWN;
    src.data = file->data();
    src.data_size = static_cast<uint32_t>(file->size());
    // The JPEG decoders take baseline JPEG only (spec 4.3): whatever else
    // they fail on, progressive or damaged, is reported as such.
    const char* undecodable = jpeg ? "unsupported JPEG" : "cannot decode";
    lv_image_header_t header;
    if (lv_image_decoder_get_info(&src, &header) != LV_RESULT_OK) {
        message(undecodable);
        return;
    }
    // Checked on the header, before anything is decoded. The estimate is
    // what the decoder's output format takes (4 bytes a pixel if it does
    // not say).
    const uint64_t pixels = static_cast<uint64_t>(header.w) * header.h;
    const uint32_t px_bytes = lv_color_format_get_size(static_cast<lv_color_format_t>(header.cf));
    const uint64_t estimate = pixels * (px_bytes > 0 ? px_bytes : 4);
    if (header.w > kMaxImageSide || header.h > kMaxImageSide || pixels > kMaxImagePixels ||
        estimate > kMaxMediaBytes - media_bytes_) {
        message("too large");
        return;
    }
    lv_image_decoder_args_t args{};
    args.no_cache = true;
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, &src, &args) != LV_RESULT_OK) {
        message(undecodable);
        return;
    }
    const lv_draw_buf_t* decoded = dsc.decoded;
    LargeMem mem;
    size_t size = 0;
    bool fits = true;
    if (decoded != nullptr && decoded->header.w > 0 && decoded->header.h > 0 &&
        decoded->data_size > 0) {
        size = static_cast<size_t>(decoded->header.stride) * decoded->header.h;
        // The decoder's own stride or format may make it larger than the
        // estimate.
        fits = size <= kMaxMediaBytes - media_bytes_;
        if (fits && size <= decoded->data_size) mem.p = os::alloc(size, os::Mem::Large);
        if (mem.p != nullptr) {
            std::memcpy(mem.p, decoded->data, size);
            n.image.header = decoded->header;
            n.image.header.flags = 0;
            n.image.data = static_cast<const uint8_t*>(mem.p);
            n.image.data_size = static_cast<uint32_t>(size);
        }
    }
    lv_image_decoder_close(&dsc);
    if (!fits) {
        message("too large");
        return;
    }
    if (mem.p == nullptr) {
        message(undecodable);
        return;
    }
    n.image_mem = std::move(mem);
    n.image_bytes = size;
    media_bytes_ += size;
    lv_image_set_src(n.obj, &n.image);
}

// --- Icon ---

void SceneView::set_icon(Node& n, const PropValue& v) {
    const char* symbol = "";
    if (const auto* name = as_string(v))
        for (const IconSymbol& i : kIconSymbols)
            if (i.name == std::string_view(*name)) symbol = i.symbol;
    lv_label_set_text(n.obj, symbol);
}

}  // namespace wisp::scene
