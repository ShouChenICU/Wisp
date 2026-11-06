#include "wisp/core/sensor_service.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace wisp::core {

namespace {

constexpr float kStandardGravity = 9.80665f;

bool cap(const hal::Imu& imu, const char* key) {
    const auto& caps = imu.info().caps;
    auto it = caps.find(key);
    return it != caps.end() && it->second.type() == Value::Type::Bool && it->second.as_bool();
}

Result<uint32_t> unavailable() {
    return Result<uint32_t>::err(ErrorCode::Unsupported, "unavailable");
}

}  // namespace

std::optional<SensorKind> parse_sensor_kind(std::string_view s) {
    if (s == "accel") return SensorKind::Accel;
    if (s == "gyro") return SensorKind::Gyro;
    return std::nullopt;
}

std::optional<Gesture> parse_gesture(std::string_view s) {
    if (s == "shake") return Gesture::Shake;
    if (s == "flip") return Gesture::Flip;
    return std::nullopt;
}

const char* to_string(Gesture g) { return g == Gesture::Flip ? "flip" : "shake"; }

struct SensorService::Sub {
    uint32_t handle = 0;
    SensorKind kind = SensorKind::Accel;
    uint32_t rate = 0;
    std::string holder;
    std::function<void(const SensorBatch&)> on_batch;
    // A gesture subscription: `gesture` is set and on_gesture is called.
    std::optional<Gesture> gesture;
    std::function<void()> on_gesture;
    ShakeDetector shake;
    FlipDetector flip;
    bool paused = false;
    // When its next sample is due; unset until it has taken one.
    std::optional<uint64_t> next_due;
    // Samples kept since the last delivery; reserved up front, so the
    // polls only fill it.
    SensorBatch batch;
};

SensorService::SensorService(std::shared_ptr<hal::Imu> imu, OccupancyRegistry& occupancy,
                             SensorServiceConfig c)
    : imu_(std::move(imu)), occupancy_(occupancy), config_(c) {
    if (imu_) {
        has_accel_ = cap(*imu_, "accel");
        has_gyro_ = cap(*imu_, "gyro");
    }
}

SensorService::~SensorService() { stop(); }

os::TaskConfig SensorService::task_config(const SensorServiceConfig& c) {
    os::TaskConfig t;
    t.name = "wisp.imu";
    t.stack_bytes = c.stack_bytes;
    t.priority = c.priority;
    // poll() reads the IMU over its bus and runs the subscribers'
    // callbacks, which only copy samples out (file header).
    t.flash_access = false;
    return t;
}

Result<void> SensorService::start() {
    if (task_) return Result<void>::err(ErrorCode::Busy, "sensor service already started");
    if (!imu_) return Result<void>::ok();
    quit_.store(false);
    task_ = os::Task::spawn(task_config(config_), [this] {
        uint64_t next = 0;
        while (!quit_.load()) {
            const uint32_t rate = rate_.load();
            if (rate == 0) {
                wake_.take(os::kForever);
                next = 0;
                continue;
            }
            const uint32_t period = 1000 / rate;
            const uint64_t now = os::now_ms();
            // Starting, or too far behind to catch up: from now on.
            if (next == 0 || now > next + period) next = now;
            if (now < next) {
                // A rate change gives wake_ and gets looked at at once.
                wake_.take(static_cast<uint32_t>(next - now));
                continue;
            }
            poll(now);
            next += period;
        }
    });
    if (!task_) return Result<void>::err(ErrorCode::NoMemory, "cannot start wisp.imu");
    return Result<void>::ok();
}

void SensorService::stop() {
    if (!task_) return;
    quit_.store(true);
    wake_.give();
    task_->join();
    task_.reset();
}

bool SensorService::available(SensorKind kind) const {
    return kind == SensorKind::Accel ? has_accel_ : has_gyro_;
}

Result<uint32_t> SensorService::subscribe(SensorKind kind, uint32_t rate_hz, std::string holder,
                                          std::function<void(const SensorBatch&)> cb) {
    if (!available(kind)) return unavailable();
    if (std::find(kRates.begin(), kRates.end(), rate_hz) == kRates.end())
        return Result<uint32_t>::err(ErrorCode::InvalidArgument,
                                     "rate must be 10, 25, 50 or 100 Hz");
    auto s = os::make_large<Sub>();
    if (!s) return Result<uint32_t>::err(ErrorCode::NoMemory, "no memory for a subscription");
    s->kind = kind;
    s->rate = rate_hz;
    s->holder = std::move(holder);
    s->on_batch = std::move(cb);
    // At most this many samples between two deliveries, plus one for a
    // late poll.
    s->batch.reserve(rate_hz * config_.batch_ms / 1000 + 2);
    return add(std::move(s));
}

Result<uint32_t> SensorService::on_gesture(Gesture g, std::string holder,
                                           std::function<void()> cb) {
    if (!available(SensorKind::Accel)) return unavailable();
    auto s = os::make_large<Sub>();
    if (!s) return Result<uint32_t>::err(ErrorCode::NoMemory, "no memory for a subscription");
    s->kind = SensorKind::Accel;
    s->rate = kGestureRate;
    s->holder = std::move(holder);
    s->gesture = g;
    s->on_gesture = std::move(cb);
    return add(std::move(s));
}

Result<uint32_t> SensorService::add(os::LargeUnique<Sub> s) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (auto r = occupancy_.acquire(kDevice, s->holder, OccupancyMode::Shared); !r)
        return Result<uint32_t>::err(r.error().code, r.error().message);
    s->handle = next_handle_++;
    if (next_handle_ == 0) next_handle_ = 1;
    const uint32_t handle = s->handle;
    subs_.push_back(std::move(s));
    apply_locked();
    return Result<uint32_t>::ok(handle);
}

void SensorService::unsubscribe(uint32_t handle) {
    std::lock_guard<os::Mutex> lock(mutex_);
    auto it = std::find_if(subs_.begin(), subs_.end(),
                           [&](const os::LargeUnique<Sub>& s) { return s->handle == handle; });
    if (it == subs_.end()) return;
    const std::string holder = std::move((*it)->holder);
    subs_.erase(it);
    const bool still_held =
        std::any_of(subs_.begin(), subs_.end(),
                    [&](const os::LargeUnique<Sub>& s) { return s->holder == holder; });
    if (!still_held) occupancy_.release(kDevice, holder);
    apply_locked();
}

void SensorService::set_paused(uint32_t handle, bool paused) {
    std::lock_guard<os::Mutex> lock(mutex_);
    Sub* s = find_locked(handle);
    if (s == nullptr || s->paused == paused) return;
    s->paused = paused;
    restart_locked(*s);
    apply_locked();
}

void SensorService::set_standby(bool standby) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (standby_ == standby) return;
    standby_ = standby;
    for (auto& s : subs_) restart_locked(*s);
    if (!standby) read_ok_.store(true);
    apply_locked();
    // PowerManager powers every IMU up as the device wakes (its caps say
    // "standby": "off"); with nothing to read it goes back down.
    if (!standby && rate_.load() == 0 && imu_) imu_->set_power(hal::PowerLevel::Standby);
}

uint32_t SensorService::imu_rate() const { return rate_.load(); }

SensorService::Sub* SensorService::find_locked(uint32_t handle) {
    for (auto& s : subs_)
        if (s->handle == handle) return s.get();
    return nullptr;
}

void SensorService::restart_locked(Sub& s) {
    // What it saw before the pause is not continued: no stale samples, no
    // gesture half-seen then completed after it.
    s.batch.clear();
    s.next_due.reset();
    s.shake = ShakeDetector();
    s.flip = FlipDetector();
}

void SensorService::apply_locked() {
    uint32_t rate = 0;
    if (!standby_)
        for (const auto& s : subs_)
            if (!s->paused) rate = std::max(rate, s->rate);
    const uint32_t was = rate_.load();
    if (rate == was) return;
    if (imu_) {
        // A failure here shows as failed reads (read_ok()); the driver
        // logs its own errors.
        if (rate == 0) {
            imu_->set_power(hal::PowerLevel::Standby);
        } else {
            if (was == 0) imu_->set_power(hal::PowerLevel::Active);
            imu_->set_rate(rate);
        }
    }
    if (rate == 0 || was == 0) last_batch_ms_.reset();
    rate_.store(rate);
    wake_.give();
}

void SensorService::deliver_locked() {
    for (auto& s : subs_) {
        if (s->gesture || s->batch.empty()) continue;
        s->on_batch(s->batch);
        s->batch.clear();
    }
}

void SensorService::poll(uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    const uint32_t rate = rate_.load();
    if (rate == 0 || !imu_) return;
    if (!last_batch_ms_) {
        last_batch_ms_ = now_ms;
    } else if (now_ms - *last_batch_ms_ >= config_.batch_ms) {
        deliver_locked();
        // Kept on its grid unless the polls fell behind it.
        *last_batch_ms_ += config_.batch_ms;
        if (now_ms - *last_batch_ms_ >= config_.batch_ms) last_batch_ms_ = now_ms;
    }

    auto r = imu_->read();
    read_ok_.store(static_cast<bool>(r));
    if (!r) return;
    const hal::ImuSample& in = r.value();
    const SensorSample accel{in.ax * kStandardGravity, in.ay * kStandardGravity,
                             in.az * kStandardGravity, now_ms};
    const SensorSample gyro{in.gx, in.gy, in.gz, now_ms};
    // A sample within half an IMU period of a subscription's due time is
    // the nearest one it will get.
    const uint64_t half = 1000 / rate / 2;
    for (auto& s : subs_) {
        if (s->paused) continue;
        const uint64_t period = 1000 / s->rate;
        if (s->next_due && now_ms + half < *s->next_due) continue;
        // Kept on its grid unless it fell a whole period behind.
        s->next_due = !s->next_due || now_ms >= *s->next_due + period ? now_ms + period
                                                                      : *s->next_due + period;
        const SensorSample& v = s->kind == SensorKind::Accel ? accel : gyro;
        if (!s->gesture) {
            // Never past what was reserved: no allocation on this task.
            if (s->batch.size() < s->batch.capacity()) s->batch.push_back(v);
            continue;
        }
        const bool fired = *s->gesture == Gesture::Shake ? s->shake.feed(v.x, v.y, v.z, now_ms)
                                                         : s->flip.feed(v.z, now_ms);
        if (fired) s->on_gesture();
    }
}

}  // namespace wisp::core
