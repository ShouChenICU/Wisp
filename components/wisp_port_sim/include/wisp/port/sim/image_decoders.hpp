#pragma once

// The simulator's side of ui::UiConfig::install_image_decoders: baseline
// JPEG through TJPGD, the decoder LVGL bundles (scene-runtime-core spec
// 4.3). The firmware's counterpart is wisp::port::esp's esp_new_jpeg
// decoder; both turn an lv_image_dsc_t whose data starts with the JPEG
// marker FF D8 into pixels in the display's colour format, in a buffer
// from os::alloc(Mem::Large), and refuse a progressive or broken JPEG
// (no pixels, nothing drawn).

namespace wisp::port::sim {

// UI task only, once per UiRuntime (pass it as the hook itself).
void install_image_decoders();

}  // namespace wisp::port::sim
