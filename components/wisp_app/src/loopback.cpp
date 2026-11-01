#include "loopback.hpp"

#include <cstdint>
#include <utility>

#include "app_impl.hpp"
#include "wisp/app/app.hpp"
#include "wisp/base/error.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

namespace {

constexpr uint32_t kSampleRateHz = 16000;
constexpr uint32_t kDurationS = 3;
constexpr size_t kFrameCount = kSampleRateHz * kDurationS;      // 48000 frames
constexpr size_t kBufferBytes = kFrameCount * sizeof(int16_t);  // 96000 bytes

void record_and_play(App& app, std::function<void(const char* status)> on_status) {
    auto fail = [&on_status](ErrorCode code) {
        std::string msg = std::string("失败: ") + to_string(code);
        on_status(msg.c_str());
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
        on_status("录音中");
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
        on_status("播放中");
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

    on_status("完成");
}

}  // namespace

void run_loopback(App& app, std::function<void(const char* status)> on_status) {
    App::Impl* impl = app.impl_.get();
    if (impl->loop_running.exchange(true)) {
        // Already running -- ignore this trigger (spec 8).
        return;
    }
    if (impl->loop_task) {
        // A previous run finished but hasn't been joined yet; join it now
        // (it's guaranteed not running, since loop_running was false)
        // before replacing the handle.
        impl->loop_task->join();
        impl->loop_task.reset();
    }

    os::TaskConfig config;
    config.name = "wisp.loop";
    auto on_status_copy = std::move(on_status);
    impl->loop_task = os::Task::spawn(config, [&app, impl, on_status_copy]() mutable {
        record_and_play(app, on_status_copy);
        impl->loop_running.store(false);
    });
}

void join_loopback(App& app) {
    App::Impl* impl = app.impl_.get();
    if (impl->loop_task) {
        impl->loop_task->join();
        impl->loop_task.reset();
    }
}

}  // namespace wisp::app
