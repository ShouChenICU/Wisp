#pragma once

// The font service's hooks into UiRuntime's lifecycle (fonts.cpp). Only
// UiRuntime calls these.

#include "wisp/os/os.hpp"
#include "wisp/ui/font_image.hpp"

namespace wisp::ui::detail {

// Hands font() the image this start() checked; nullptr when there is none
// or it failed open_font_image(). Called by UiRuntime::start() before the
// UI task runs; the image's bytes must outlive the runtime.
void install_font_image(const FontImage* image);

// The UI task font() must run on, checked in debug builds; nullptr while
// no runtime runs, when any thread may call it. Set by UiRuntime::start()
// once the task exists and cleared by stop() after it has exited.
void set_font_owner(const os::Task* task);

// Destroys every vector font font() created and forgets the image.
// Called by UiRuntime::stop() after the UI task has exited and before
// lv_deinit(): the fonts and their glyph caches are LVGL heap and
// lv_cache objects, which must not outlive the LVGL instance.
void release_vector_fonts();

}  // namespace wisp::ui::detail
