#pragma once

// Test-only helpers for inspecting a live LVGL UI owned by a UiRuntime:
// running probes on the UI thread and searching widget trees by type or
// text. Shared by the self-check screen, network UI and factory-reset
// tests, which treat the screens themselves as black boxes.

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "lvgl.h"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::test {

// Runs `probe` (which may call lv_*: it runs on the UI task) and blocks
// until it has returned. Empty if the post queue was full.
inline std::string run_on_ui_thread(wisp::ui::UiRuntime* ui,
                                    const std::function<std::string()>& probe) {
    std::string result;
    wisp::os::Semaphore done(1, 0);
    if (!ui->post([&] {
            result = probe();
            done.give();
        })) {
        return {};
    }
    done.take(wisp::os::kForever);
    return result;
}

// Like run_on_ui_thread() for a void `fn` -- e.g. PowerManager's
// enter_standby()/exit_standby(), which call Display::set_power() directly
// and so must run on the UI thread (spec 5.3).
inline void run_on_ui_blocking(wisp::ui::UiRuntime* ui, const std::function<void()>& fn) {
    wisp::os::Semaphore done(1, 0);
    if (!ui->post([&] {
            fn();
            done.give();
        })) {
        return;
    }
    done.take(wisp::os::kForever);
}

// The first object of class `cls` in `obj`'s subtree (depth first), or
// nullptr.
inline lv_obj_t* find_obj_of_class(lv_obj_t* obj, const lv_obj_class_t* cls) {
    if (lv_obj_check_type(obj, cls)) {
        return obj;
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        if (lv_obj_t* found = find_obj_of_class(lv_obj_get_child(obj, i), cls)) {
            return found;
        }
    }
    return nullptr;
}

// How many objects of class `cls` are in `obj`'s subtree.
inline int count_obj_of_class(lv_obj_t* obj, const lv_obj_class_t* cls) {
    int n = lv_obj_check_type(obj, cls) ? 1 : 0;
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        n += count_obj_of_class(lv_obj_get_child(obj, i), cls);
    }
    return n;
}

// Width of the first lv_msgbox in `obj`'s subtree, -1 if none is open.
// lv_msgbox_create(nullptr) parents the box under a full-screen backdrop
// on lv_layer_top(), not under the active screen.
inline int32_t find_msgbox_width(lv_obj_t* obj) {
    lv_obj_t* box = find_obj_of_class(obj, &lv_msgbox_class);
    return box != nullptr ? lv_obj_get_width(box) : -1;
}

// True if every label in `obj`'s subtree can render every codepoint of
// its text with the font it actually resolves to (fallbacks included).
// Catches widgets created outside the active screen (e.g. on
// lv_layer_top()) that do not inherit the CJK fallback.
inline bool all_label_glyphs_resolvable(lv_obj_t* obj) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const lv_font_t* font = lv_obj_get_style_text_font(obj, LV_PART_MAIN);
        const char* text = lv_label_get_text(obj);
        const auto* p = reinterpret_cast<const unsigned char*>(text != nullptr ? text : "");
        while (*p != 0) {
            uint32_t letter = *p;
            int extra = letter >= 0xF0 ? 3 : letter >= 0xE0 ? 2 : letter >= 0xC0 ? 1 : 0;
            letter &= extra == 0 ? 0x7F : (0x3F >> extra);
            ++p;
            for (int k = 0; k < extra && *p != 0; ++k, ++p) {
                letter = (letter << 6) | (*p & 0x3F);
            }
            if (letter == ' ' || letter == '\n') {
                continue;
            }
            lv_font_glyph_dsc_t dsc;
            if (!lv_font_get_glyph_dsc(font, &dsc, letter, 0)) {
                return false;
            }
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        if (!all_label_glyphs_resolvable(lv_obj_get_child(obj, i))) {
            return false;
        }
    }
    return true;
}

// The first label in `obj`'s subtree whose text contains `needle`.
inline lv_obj_t* find_label_containing(lv_obj_t* obj, const char* needle) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char* text = lv_label_get_text(obj);
        if (text != nullptr && std::string(text).find(needle) != std::string::npos) {
            return obj;
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        if (lv_obj_t* found = find_label_containing(lv_obj_get_child(obj, i), needle)) {
            return found;
        }
    }
    return nullptr;
}

// That label's text, or empty if there is none.
inline std::string find_label_text_containing(lv_obj_t* obj, const char* needle) {
    lv_obj_t* label = find_label_containing(obj, needle);
    return label != nullptr ? lv_label_get_text(label) : std::string();
}

// The object (e.g. a button) one of whose direct child labels reads
// exactly `text`, searching `obj`'s subtree; nullptr if none. Lets a test
// lv_obj_send_event() a click to it.
inline lv_obj_t* find_button_with_label(lv_obj_t* obj, const char* text) {
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* child = lv_obj_get_child(obj, i);
        if (lv_obj_check_type(child, &lv_label_class)) {
            const char* label_text = lv_label_get_text(child);
            if (label_text != nullptr && std::string(label_text) == text) {
                return obj;
            }
        }
    }
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t* found = find_button_with_label(lv_obj_get_child(obj, i), text);
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

// Every label in `obj`'s subtree, as text -> screen coordinates.
inline void collect_labels(lv_obj_t* obj, std::vector<std::pair<std::string, lv_area_t>>& out) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        const char* text = lv_label_get_text(obj);
        out.emplace_back(text != nullptr ? text : "", a);
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        collect_labels(lv_obj_get_child(obj, i), out);
    }
}

}  // namespace wisp::test
