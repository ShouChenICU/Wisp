#pragma once

// Internal (not under include/) helper for the self-check screen's
// "录音回放" button (spec 8): records 3s of 16kHz mono audio into a
// os::alloc(Mem::Large) buffer on a dedicated "wisp.loop" task, then
// immediately plays it back. See self_check_screen.cpp for how it's
// wired to the button; see app.cpp's start()/stop() for how its
// lifetime is tied to the owning App (reset_loopback()/join_loopback()).

#include <functional>
#include <string>

#include "wisp/base/error.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

class App;

// One step of a loopback run. Carries the string id rather than text so
// the text is only made where it is shown, in the language shown there;
// the screen keeps the last one and re-renders it after a language
// switch.
struct LoopbackStatus {
    ui::StrId id = ui::StrId::LoopReady;
    // Set only with LoopFailed: what failed.
    ErrorCode error = ErrorCode::Ok;
};

// The status as shown on screen in `lang`, e.g. "录音中" or
// "失败: not_found".
std::string loopback_status_text(const LoopbackStatus& status, ui::Lang lang);

// Must be called from any thread other than the "wisp.loop" task itself.
// Spawns (and this call returns without waiting for) a "wisp.loop" task
// that:
//   1. opens "audio_in.main", records 3s (16kHz mono, a 96000-byte
//      os::alloc(Mem::Large) buffer) and reports LoopRecording right
//      before recording starts;
//   2. closes audio_in, opens "audio_out.main", reports LoopPlaying right
//      before playback starts, writes the recorded buffer, closes
//      audio_out;
//   3. reports LoopDone.
// On any failure (device missing, open()/read()/write() erroring, or
// os::Task::spawn() itself failing), reports LoopFailed with the error
// code and stops early (skipping whichever steps hadn't started yet).
// `on_status` runs on the "wisp.loop" task, not the caller's thread; the
// caller (self_check_screen.cpp) marshals it to the UI thread itself via
// ui::UiRuntime::post().
//
// This call is ignored outright (no task spawned, no on_status call at
// all) if a previous run_loopback() task for this `App` is still running
// (see spec 8's "已在运行时再次触发直接忽略") -- or if join_loopback()
// has already been called for this App and reset_loopback() hasn't run
// since -- this is what makes it safe to trigger
// "录音回放" concurrently with App::stop(), without risking a "wisp.loop"
// task that starts after join_loopback() has already returned and so
// never gets joined). The self-check screen may go while the task runs
// (the user goes back mid-test): its on_status posts check that the screen
// is still up before touching it.
void run_loopback(App& app, std::function<void(const LoopbackStatus&)> on_status);

// Marks `app` as stopping (so every subsequent run_loopback() call on it
// is ignored, per the note above) and blocks until any in-flight
// run_loopback() task for it has finished, then releases its handle.
// Safe to call even if no loopback ever ran, or one already finished on
// its own. Not safe to call from within on_status() itself (the task
// can't join itself). Call this (from App::stop()) before tearing down
// anything a running loopback task's on_status callback might touch.
void join_loopback(App& app);

// Clears the "stopping" state join_loopback() set, so run_loopback()
// works again. Call this from App::start(), before anything else, so a
// start()/stop()/start() cycle on the same App doesn't leave "录音回放"
// permanently disabled after the first stop().
void reset_loopback(App& app);

}  // namespace wisp::app
