#pragma once

// ESP-IDF wisp::hal::AudioOut/AudioIn backed by esp_codec_dev handles that
// esp_board_manager (bmgr) already constructed for the board's audio_dac
// (ES8311) and audio_adc (ES7210) devices (see
// boards/waveshare_amoled_216/board.cpp, which fetches
// dev_audio_codec_handles_t::codec_dev for each and passes it to
// create()). The PA (GPIO46, board_peripherals.yaml's gpio_pa_control) is
// not touched here at all: it is wired into audio_dac's "pa_name"
// peripheral binding (board_devices.yaml), so esp_codec_dev's ES8311
// driver itself drives PA_EN low/high in step with
// esp_codec_dev_open()/close() and each dac_enable() transition --
// task-18-brief.md's "若 bmgr 让 PA 跟随 codec open/close，则优先选用该
// 方式". Chip models, pins and the PA GPIO are board-level facts
// (AGENTS.md's "板子无关" rule) and never appear here.
//
// esp_codec_dev's own output volume/mic gain both start at 0 (muted) on a
// freshly opened device (esp_codec_dev_new() calloc's its internal state,
// and codec_dev_apply_vol_mute() in esp_codec_dev_open() applies that
// zero) -- open() below immediately re-applies a sane default so
// components/wisp_app/src/loopback.cpp's "录音回放" self-check (which
// never calls set_volume()/set_gain() itself) is actually audible. These
// defaults (kDefaultVolumePercent/kDefaultMicGainDb) are a starting point
// for real-hardware tuning, not calibrated values -- see task-18-report.md.
//
// EspAudioIn always opens the ES7210 codec with 2 physical channels
// (channel_mask 0x3 -- the board wires two analog mics, migrated from
// board_devices.yaml's legacy `adc_channel_mask: "0011"`) and downmixes
// every read() to mono with wisp/port/esp/audio_downmix.hpp before
// handing samples back: wisp::hal::AudioIn's contract (and loopback.cpp's
// only caller) is mono (AudioFormat::channels == 1) -- see that header's
// comment. Both open()s below validate that the *caller's* requested
// format.channels is 1 and fail with ErrorCode::InvalidArgument
// otherwise (fix round 1) -- the 2-physical-channel ES7210 open above is
// this class's own internal implementation detail, never something a
// caller can ask to change.
//
// EspAudioOut::close() drains before calling esp_codec_dev_close() (fix
// round 1): esp_codec_dev_write()/i2s_channel_write() returns as soon as
// data is handed to the I2S DMA ring, not once it's actually been
// clocked out -- closing (which also cuts PA power, via the "pa_name"
// binding above) immediately after a write would truncate the audible
// tail. See dev/esp_audio.cpp's kDrainFrames comment for how the drain
// amount is derived and wisp::hal::AudioOut's close() contract (matched
// by wisp::port::sim::SimAudioOut::close(), which also drains).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "esp_codec_dev.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/audio.hpp"

namespace wisp::port::esp {

class EspAudioOut : public wisp::hal::AudioOut {
public:
    // Wraps an already-constructed esp_codec_dev_handle_t (owned by bmgr;
    // this class never deletes it). Fails with ErrorCode::InvalidArgument
    // if `codec_dev` is null.
    static Result<std::shared_ptr<EspAudioOut>> create(esp_codec_dev_handle_t codec_dev,
                                                       std::string model);

    ~EspAudioOut() override;

    EspAudioOut(const EspAudioOut&) = delete;
    EspAudioOut& operator=(const EspAudioOut&) = delete;

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // Fails with ErrorCode::InvalidArgument if format.channels != 1 (see
    // file comment). Otherwise, esp_codec_dev_open() with
    // {bits_per_sample=16, channel=1, channel_mask=0x1,
    // sample_rate=format.sample_rate}. Fails with
    // ErrorCode::AlreadyExists if already open (mirrors
    // wisp::port::sim::SimAudioOut), or ErrorCode::DeviceFailure if
    // esp_codec_dev_open() itself fails. Re-applies the current volume
    // (see set_volume()) immediately after a successful open, since
    // esp_codec_dev starts every freshly opened device muted.
    Result<void> open(const wisp::hal::AudioFormat& format) override;
    // esp_codec_dev_write(). Blocking: either writes all `count` frames
    // (returned as ok(count)) or fails -- esp_codec_dev's I2S data path
    // has no concept of a partial write.
    Result<size_t> write(const int16_t* frames, size_t count) override;
    // Drains (see file comment), then esp_codec_dev_close(). Safe to
    // call when not open (no-op), mirrors every other AudioOut.
    void close() override;
    // esp_codec_dev_set_out_vol(). Stored even while closed, and
    // re-applied by the next open().
    Result<void> set_volume(uint8_t percent) override;

private:
    EspAudioOut(esp_codec_dev_handle_t codec_dev, std::string model);

    // Writes enough silence to flush any real audio out of the I2S DMA
    // ring before close() actually closes the codec device -- see file
    // comment and dev/esp_audio.cpp's kDrainFrames.
    void drain();

    wisp::hal::DeviceInfo info_;
    esp_codec_dev_handle_t codec_dev_;
    std::atomic<bool> open_{false};
    std::atomic<uint8_t> volume_percent_;
};

class EspAudioIn : public wisp::hal::AudioIn {
public:
    // Wraps an already-constructed esp_codec_dev_handle_t (owned by bmgr;
    // this class never deletes it). Fails with ErrorCode::InvalidArgument
    // if `codec_dev` is null.
    static Result<std::shared_ptr<EspAudioIn>> create(esp_codec_dev_handle_t codec_dev,
                                                      std::string model);

    ~EspAudioIn() override;

    EspAudioIn(const EspAudioIn&) = delete;
    EspAudioIn& operator=(const EspAudioIn&) = delete;

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // Fails with ErrorCode::InvalidArgument if format.channels != 1 (see
    // file comment -- the caller-facing contract is still mono even
    // though the codec itself is always opened with 2 physical
    // channels). Otherwise, esp_codec_dev_open() with
    // {bits_per_sample=16, channel=2, channel_mask=0x3,
    // sample_rate=format.sample_rate}. Fails with
    // ErrorCode::AlreadyExists if already open, or
    // ErrorCode::DeviceFailure if esp_codec_dev_open() fails. Re-applies
    // the current mic gain (see set_gain()) immediately after a
    // successful open, for the same reason as EspAudioOut::open().
    Result<void> open(const wisp::hal::AudioFormat& format) override;
    // Reads in fixed-size internal chunks (kScratchFrames stereo frames
    // at a time, downmixed into `frames` as each chunk completes) rather
    // than one `count`-sized stereo allocation, so a large `count` (e.g.
    // loopback.cpp's 48000-frame/3s capture) never needs a correspondingly
    // large scratch buffer. Blocking: either reads all `count` frames
    // (returned as ok(count)) or fails -- esp_codec_dev_read() has no
    // concept of a partial read either.
    Result<size_t> read(int16_t* frames, size_t count) override;
    // esp_codec_dev_close(). Safe to call when not open (no-op).
    void close() override;
    // esp_codec_dev_set_in_gain(). Stored even while closed, and
    // re-applied by the next open().
    Result<void> set_gain(float db) override;

private:
    // 30ms at 16kHz; small enough to keep as a fixed member array (no
    // heap allocation on the read() hot path), large enough that a 3s
    // capture only needs ~100 esp_codec_dev_read() calls.
    static constexpr size_t kScratchFrames = 480;

    EspAudioIn(esp_codec_dev_handle_t codec_dev, std::string model);

    wisp::hal::DeviceInfo info_;
    esp_codec_dev_handle_t codec_dev_;
    std::atomic<bool> open_{false};
    std::atomic<float> mic_gain_db_;
    int16_t scratch_[kScratchFrames * 2];  // interleaved stereo
};

}  // namespace wisp::port::esp
