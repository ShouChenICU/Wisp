#pragma once

// Internal (not under include/) helper for the self-check screen's
// "录音回放" button (spec 8): records 3s of 16kHz mono audio into a
// os::alloc(Mem::Large) buffer on a dedicated "wisp.loop" task, then
// immediately plays it back. See self_check_screen.cpp for how it's
// wired to the button and to SelfCheck's lifetime (the task this spawns
// is joined by whatever owns the run_loopback() call before the App's
// EventBus/UiRuntime are stopped -- see app.hpp's Lifetime note).

#include <functional>

namespace wisp::app {

class App;

// Must be called from any thread other than the "wisp.loop" task itself.
// Spawns (and this call returns without waiting for) a "wisp.loop" task
// that:
//   1. opens "audio_in.main", records 3s (16kHz mono, a 96000-byte
//      os::alloc(Mem::Large) buffer) and calls on_status("录音中") right
//      before recording starts;
//   2. closes audio_in, opens "audio_out.main", calls on_status("播放中")
//      right before playback starts, writes the recorded buffer, closes
//      audio_out;
//   3. calls on_status("完成").
// On any failure (device missing, open()/read()/write() erroring), calls
// on_status("失败: <error code>") and stops early (skipping whichever
// steps hadn't started yet). `on_status` runs on the "wisp.loop" task,
// not the caller's thread -- the caller (self_check_screen.cpp) marshals
// it back to the UI thread itself via ui::UiRuntime::post().
//
// If a previous run_loopback() task for this `App` is still running when
// this is called again, the new call is ignored (no task spawned, no
// on_status call at all) -- see spec 8's "已在运行时再次触发直接忽略".
// Call join_loopback() (e.g. from SelfCheck's destructor) to make sure no
// "wisp.loop" task is left running before the App that owns it is torn
// down further.
void run_loopback(App& app, std::function<void(const char* status)> on_status);

// Blocks until any in-flight run_loopback() task for `app` has finished,
// then releases its handle. Safe to call even if no loopback ever ran, or
// one already finished on its own. Not safe to call from within
// on_status() itself (the task can't join itself).
void join_loopback(App& app);

}  // namespace wisp::app
