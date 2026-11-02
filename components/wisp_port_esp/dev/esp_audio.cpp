#include "wisp/port/esp/esp_audio.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "esp_codec_dev_defaults.h"
#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"
#include "wisp/port/esp/audio_downmix.hpp"

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "esp_audio";

// Starting points for real-hardware tuning (see esp_audio.hpp's file
// comment) -- not calibrated against the actual ES8311/ES7210/speaker/mic
// on this board yet.
constexpr uint8_t kDefaultVolumePercent = 80;
constexpr float kDefaultMicGainDb = 30.0f;

constexpr uint8_t kBitsPerSample = 16;

}  // namespace

// ---------------------------------------------------------------------
// EspAudioOut
// ---------------------------------------------------------------------

Result<std::shared_ptr<EspAudioOut>> EspAudioOut::create(esp_codec_dev_handle_t codec_dev,
                                                         std::string model) {
    using R = Result<std::shared_ptr<EspAudioOut>>;
    if (codec_dev == nullptr) {
        return R::err(ErrorCode::InvalidArgument, "null codec_dev handle");
    }
    return R::ok(std::shared_ptr<EspAudioOut>(new EspAudioOut(codec_dev, std::move(model))));
}

EspAudioOut::EspAudioOut(esp_codec_dev_handle_t codec_dev, std::string model)
    : codec_dev_(codec_dev), volume_percent_(kDefaultVolumePercent) {
    info_.id = "audio_out.main";
    info_.cls = kClass;
    info_.model = std::move(model);
}

// No esp_codec_dev_delete(): the handle is owned by bmgr (it lives for
// the process's lifetime, like every other ESP HAL device Wisp wraps).
EspAudioOut::~EspAudioOut() { close(); }

Result<void> EspAudioOut::open(const wisp::hal::AudioFormat& format) {
    if (open_) {
        return Result<void>::err(ErrorCode::AlreadyExists, "audio_out already open");
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = kBitsPerSample;
    fs.channel = 1;
    fs.channel_mask = 0x1;
    fs.sample_rate = format.sample_rate;

    int ret = esp_codec_dev_open(codec_dev_, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 "esp_codec_dev_open: " + std::to_string(ret));
    }
    open_ = true;

    ret = esp_codec_dev_set_out_vol(codec_dev_, volume_percent_.load());
    if (ret != ESP_CODEC_DEV_OK) {
        // Non-fatal: the device is open and usable, just not necessarily
        // at the requested volume -- log and continue.
        WLOG_W(kTag, "esp_codec_dev_set_out_vol failed on open: %d", ret);
    }
    return Result<void>::ok();
}

Result<size_t> EspAudioOut::write(const int16_t* frames, size_t count) {
    if (!open_) {
        return Result<size_t>::err(ErrorCode::InvalidArgument, "audio_out not open");
    }
    int len = static_cast<int>(count * sizeof(int16_t));
    int ret = esp_codec_dev_write(codec_dev_, const_cast<int16_t*>(frames), len);
    if (ret != ESP_CODEC_DEV_OK) {
        return Result<size_t>::err(ErrorCode::IoError,
                                   "esp_codec_dev_write: " + std::to_string(ret));
    }
    return Result<size_t>::ok(count);
}

void EspAudioOut::close() {
    if (!open_) {
        return;
    }
    esp_codec_dev_close(codec_dev_);
    open_ = false;
}

Result<void> EspAudioOut::set_volume(uint8_t percent) {
    uint8_t clamped = std::min<uint8_t>(percent, 100);
    volume_percent_.store(clamped);
    if (!open_) {
        return Result<void>::ok();
    }
    int ret = esp_codec_dev_set_out_vol(codec_dev_, clamped);
    if (ret != ESP_CODEC_DEV_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 "esp_codec_dev_set_out_vol: " + std::to_string(ret));
    }
    return Result<void>::ok();
}

// ---------------------------------------------------------------------
// EspAudioIn
// ---------------------------------------------------------------------

Result<std::shared_ptr<EspAudioIn>> EspAudioIn::create(esp_codec_dev_handle_t codec_dev,
                                                       std::string model) {
    using R = Result<std::shared_ptr<EspAudioIn>>;
    if (codec_dev == nullptr) {
        return R::err(ErrorCode::InvalidArgument, "null codec_dev handle");
    }
    return R::ok(std::shared_ptr<EspAudioIn>(new EspAudioIn(codec_dev, std::move(model))));
}

EspAudioIn::EspAudioIn(esp_codec_dev_handle_t codec_dev, std::string model)
    : codec_dev_(codec_dev), mic_gain_db_(kDefaultMicGainDb) {
    info_.id = "audio_in.main";
    info_.cls = kClass;
    info_.model = std::move(model);
    std::memset(scratch_, 0, sizeof(scratch_));
}

EspAudioIn::~EspAudioIn() { close(); }

Result<void> EspAudioIn::open(const wisp::hal::AudioFormat& format) {
    if (open_) {
        return Result<void>::err(ErrorCode::AlreadyExists, "audio_in already open");
    }

    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = kBitsPerSample;
    fs.channel = 2;
    fs.channel_mask = 0x3;
    fs.sample_rate = format.sample_rate;

    int ret = esp_codec_dev_open(codec_dev_, &fs);
    if (ret != ESP_CODEC_DEV_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 "esp_codec_dev_open: " + std::to_string(ret));
    }
    open_ = true;

    ret = esp_codec_dev_set_in_gain(codec_dev_, mic_gain_db_.load());
    if (ret != ESP_CODEC_DEV_OK) {
        WLOG_W(kTag, "esp_codec_dev_set_in_gain failed on open: %d", ret);
    }
    return Result<void>::ok();
}

Result<size_t> EspAudioIn::read(int16_t* frames, size_t count) {
    if (!open_) {
        return Result<size_t>::err(ErrorCode::InvalidArgument, "audio_in not open");
    }

    size_t done = 0;
    while (done < count) {
        size_t chunk = std::min(count - done, kScratchFrames);
        int len = static_cast<int>(chunk * 2 * sizeof(int16_t));
        int ret = esp_codec_dev_read(codec_dev_, scratch_, len);
        if (ret != ESP_CODEC_DEV_OK) {
            return Result<size_t>::err(ErrorCode::IoError,
                                       "esp_codec_dev_read: " + std::to_string(ret));
        }
        downmix_stereo_to_mono(scratch_, frames + done, chunk);
        done += chunk;
    }
    return Result<size_t>::ok(done);
}

void EspAudioIn::close() {
    if (!open_) {
        return;
    }
    esp_codec_dev_close(codec_dev_);
    open_ = false;
}

Result<void> EspAudioIn::set_gain(float db) {
    mic_gain_db_.store(db);
    if (!open_) {
        return Result<void>::ok();
    }
    int ret = esp_codec_dev_set_in_gain(codec_dev_, db);
    if (ret != ESP_CODEC_DEV_OK) {
        return Result<void>::err(ErrorCode::DeviceFailure,
                                 "esp_codec_dev_set_in_gain: " + std::to_string(ret));
    }
    return Result<void>::ok();
}

}  // namespace wisp::port::esp
