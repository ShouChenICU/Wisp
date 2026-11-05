#include "loopback.hpp"

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

#include "app_impl.hpp"
#include "wisp/app/app.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

constexpr uint32_t kSampleRateHz = 16000;
constexpr uint32_t kDurationS = 3;
constexpr size_t kFrameCount = kSampleRateHz * kDurationS;      // 48000 frames
constexpr size_t kBufferBytes = kFrameCount * sizeof(int16_t);  // 96000 bytes

// Default volume (percent) applied when Settings has no "audio"/"volume"
// entry yet (e.g. first boot, or a settings backend that failed to open).
constexpr int64_t kDefaultVolumePercent = 60;

void record_and_play(App& app, core::SettingsBackend* settings_backend,
                     const std::function<void(const LoopbackStatus&)>& on_status) {
    auto fail = [&on_status](ErrorCode code) {
        on_status(LoopbackStatus{ui::StrId::LoopFailed, code});
    };

    auto audio_in = app.registry().get<hal::AudioIn>("audio_in.main");
    auto audio_out = app.registry().get<hal::AudioOut>("audio_out.main");
    if (!audio_in || !audio_out) {
        fail(ErrorCode::NotFound);
        return;
    }

    auto* samples = static_cast<int16_t*>(os::alloc(kBufferBytes, os::Mem::Large));
    if (samples == nullptr) {
        fail(ErrorCode::NoMemory);
        return;
    }

    hal::AudioFormat format;
    format.sample_rate = kSampleRateHz;
    format.channels = 1;

    bool ok = [&] {
        auto opened = audio_in->open(format);
        if (!opened) {
            fail(opened.error().code);
            return false;
        }
        on_status(LoopbackStatus{ui::StrId::LoopRecording});
        size_t total = 0;
        while (total < kFrameCount) {
            auto read = audio_in->read(samples + total, kFrameCount - total);
            if (!read) {
                audio_in->close();
                fail(read.error().code);
                return false;
            }
            if (read.value() == 0) {
                break;  // nothing more to read; play back what we have.
            }
            total += read.value();
        }
        audio_in->close();
        return true;
    }();
    if (!ok) {
        os::dealloc(samples);
        return;
    }

    ok = [&] {
        auto opened = audio_out->open(format);
        if (!opened) {
            fail(opened.error().code);
            return false;
        }
        // spec 6.6: volume is one of sub-project 1's three Settings-backed
        // items (brightness, volume, screen_off_s). Applied here, once per
        // open(), rather than once at startup, so a change takes effect on
        // the very next playback -- there is no separate "apply settings"
        // hook for audio the way there is for display at App::start().
        if (settings_backend != nullptr) {
            core::Settings audio_settings(*settings_backend, "audio");
            int64_t volume = std::clamp<int64_t>(
                audio_settings.get_int("volume", kDefaultVolumePercent), 0, 100);
            audio_out->set_volume(static_cast<uint8_t>(volume));
        }
        on_status(LoopbackStatus{ui::StrId::LoopPlaying});
        size_t total = 0;
        while (total < kFrameCount) {
            auto written = audio_out->write(samples + total, kFrameCount - total);
            if (!written) {
                audio_out->close();
                fail(written.error().code);
                return false;
            }
            if (written.value() == 0) {
                break;
            }
            total += written.value();
        }
        audio_out->close();
        return true;
    }();
    os::dealloc(samples);
    if (!ok) {
        return;
    }

    on_status(LoopbackStatus{ui::StrId::LoopDone});
}

}  // namespace

std::string loopback_status_text(const LoopbackStatus& status, ui::Lang lang) {
    std::string text = ui::tr(status.id, lang);
    if (status.id == ui::StrId::LoopFailed) {
        text += ": ";
        text += to_string(status.error);
    }
    return text;
}

void run_loopback(App& app, std::function<void(const LoopbackStatus&)> on_status) {
    App::Impl* impl = app.impl_.get();

    bool spawn_failed = false;
    {
        std::lock_guard<os::Mutex> lock(impl->loop_mutex);
        if (impl->loop_stopping || impl->loop_running) {
            // Either App::stop() has already begun (join_loopback() sets
            // loop_stopping as its first action, before anything else)
            // or a previous run is still in flight (spec 8's "已在运行时
            // 再次触发直接忽略"). Either way, ignore this trigger
            // entirely -- no task spawned, no on_status call.
            return;
        }
        if (impl->loop_task) {
            // A previous run finished but hasn't been joined yet; join it
            // now (it's guaranteed not running, since loop_running was
            // false) before replacing the handle.
            impl->loop_task->join();
            impl->loop_task.reset();
        }

        impl->loop_running = true;
        os::TaskConfig config;
        config.name = "wisp.loop";
        // Copied (not moved) so the original `on_status` is still usable
        // below if os::Task::spawn() itself fails.
        auto on_status_copy = on_status;
        core::SettingsBackend* settings_backend = impl->platform.settings.get();
        impl->loop_task =
            os::Task::spawn(config, [&app, impl, settings_backend, on_status_copy]() mutable {
                record_and_play(app, settings_backend, on_status_copy);
                std::lock_guard<os::Mutex> lock(impl->loop_mutex);
                impl->loop_running = false;
            });
        if (impl->loop_task == nullptr) {
            impl->loop_running = false;
            spawn_failed = true;
        }
    }

    if (spawn_failed) {
        on_status(LoopbackStatus{ui::StrId::LoopFailed, ErrorCode::NoMemory});
    }
}

void join_loopback(App& app) {
    App::Impl* impl = app.impl_.get();
    std::unique_ptr<os::Task> task_to_join;
    {
        std::lock_guard<os::Mutex> lock(impl->loop_mutex);
        // Set first, before anything else below (including inspecting
        // loop_task) -- see loopback.hpp/app_impl.hpp: this is what
        // serializes against a concurrent run_loopback() call so no new
        // "wisp.loop" task can ever be spawned after this point without
        // also being the exact task we go on to join just below.
        impl->loop_stopping = true;
        task_to_join = std::move(impl->loop_task);
    }
    if (task_to_join) {
        // Joined outside the lock: the task's own completion lambda
        // above takes loop_mutex too, so holding it here while joining
        // would deadlock against that.
        task_to_join->join();
    }
}

void reset_loopback(App& app) {
    App::Impl* impl = app.impl_.get();
    std::lock_guard<os::Mutex> lock(impl->loop_mutex);
    impl->loop_stopping = false;
}

}  // namespace wisp::app
