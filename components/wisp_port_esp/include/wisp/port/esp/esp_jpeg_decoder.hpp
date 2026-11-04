#pragma once

// The firmware's side of ui::UiConfig::install_image_decoders: baseline
// JPEG through Espressif's esp_new_jpeg (scene-runtime-core spec 4.3).
// The simulator's counterpart is wisp::port::sim::install_image_decoders()
// (TJpgDec); both turn an lv_image_dsc_t whose data is a baseline JPEG
// into pixels in the display's colour format, in a buffer from
// os::alloc(Mem::Large), and refuse a progressive or broken JPEG.
//
// The decoder allocates about 10 KB of internal RAM of its own while a
// file is decoded (esp_new_jpeg's README), and runs on the UI task. Its
// code, IRAM routines included, runs from flash (firmware/main/
// esp_new_jpeg.lf).

namespace wisp::port::esp {

// UI task only, once per UiRuntime (pass it as the hook itself).
void install_image_decoders();

}  // namespace wisp::port::esp
