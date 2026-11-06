#pragma once

// The font service's hooks into UiRuntime's lifecycle (fonts.cpp). Only
// UiRuntime calls these.

#include <cstdint>
#include <span>

#include "wisp/os/os.hpp"
#include "wisp/ui/font_image.hpp"

namespace wisp::ui::detail {

// Hands font() the image this start() checked; nullptr when there is none
// or it failed open_font_image(). Called by UiRuntime::start() before the
// UI task runs; the image's bytes must outlive the runtime.
void install_font_image(const FontImage* image);

// Chooses the body font's CJK fallback (glyph-partition spec 4.3): the
// font built from `raw` if it passes open_glyph_image(); else, when
// `builtin_allowed`, the built-in lv_font_cjk_16 if this build has it;
// else a 16 px vector font from the image install_font_image() was
// given; else none. Logs which, with the reason when it is not `raw`.
// Called by UiRuntime::start() after install_font_image(), before the UI
// task runs; `raw` must outlive the runtime. A font built from `raw` is
// kept for the life of the process and reused by a later call with the
// same bytes.
void install_glyph_font(std::span<const uint8_t> raw, bool builtin_allowed);

// The UI task font() must run on, checked in debug builds; nullptr while
// no runtime runs, when any thread may call it. Set by UiRuntime::start()
// once the task exists and cleared by stop() after it has exited.
void set_font_owner(const os::Task* task);

// Destroys every vector font font() and install_glyph_font() created,
// returns the body font to its CJK fallback from before any runtime
// (the built-in font where there is one) and forgets the image.
// Called by UiRuntime::stop() after the UI task has exited and before
// lv_deinit(): the fonts and their glyph caches are LVGL heap and
// lv_cache objects, which must not outlive the LVGL instance.
void release_vector_fonts();

}  // namespace wisp::ui::detail
