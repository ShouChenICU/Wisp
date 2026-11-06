#include "wisp/port/sim/imu_script.hpp"

#include <charconv>
#include <cmath>
#include <string>

namespace wisp::port::sim {

namespace {

constexpr uint64_t kPeakMs = 40;
constexpr uint64_t kPeakSpacingMs = 100;
constexpr int kPeaks = 3;
constexpr float kPeakG = 3.0f;

// The next whitespace-separated word of `line` from `pos`; empty at the end.
std::string_view word(std::string_view line, size_t& pos) {
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
    const size_t start = pos;
    while (pos < line.size() && line[pos] != ' ' && line[pos] != '\t') ++pos;
    return line.substr(start, pos - start);
}

bool to_float(std::string_view s, float& out) {
    if (s.empty()) return false;
    // std::from_chars for float is not in every host libc++; strtof on a
    // NUL-terminated copy is.
    const std::string copy(s);
    char* end = nullptr;
    out = std::strtof(copy.c_str(), &end);
    return end == copy.c_str() + copy.size() && std::isfinite(out);
}

Result<std::vector<ImuScriptStep>> bad(int line, const std::string& why) {
    return Result<std::vector<ImuScriptStep>>::err(ErrorCode::InvalidArgument,
                                                   "line " + std::to_string(line) + ": " + why);
}

}  // namespace

Result<std::vector<ImuScriptStep>> parse_imu_script(std::string_view text) {
    std::vector<ImuScriptStep> steps;
    int n = 0;
    while (!text.empty()) {
        const size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view() : text.substr(eol + 1);
        ++n;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        size_t pos = 0;
        const std::string_view first = word(line, pos);
        if (first.empty() || first[0] == '#') continue;

        ImuScriptStep step{0, ImuScriptStep::Kind::Tilt};
        auto [p, ec] = std::from_chars(first.data(), first.data() + first.size(), step.at_ms);
        if (ec != std::errc() || p != first.data() + first.size())
            return bad(n, "expected a time in ms, got \"" + std::string(first) + "\"");
        const std::string_view kind = word(line, pos);
        if (kind == "tilt") {
            float* axes[] = {&step.x, &step.y, &step.z};
            for (float* a : axes)
                if (!to_float(word(line, pos), *a)) return bad(n, "tilt takes <x> <y> <z> in g");
        } else if (kind == "shake") {
            step.kind = ImuScriptStep::Kind::Shake;
        } else if (kind == "flip") {
            step.kind = ImuScriptStep::Kind::Flip;
        } else {
            return bad(n, "expected tilt, shake or flip, got \"" + std::string(kind) + "\"");
        }
        if (!word(line, pos).empty()) return bad(n, "unexpected text after the step");
        if (!steps.empty() && step.at_ms < steps.back().at_ms)
            return bad(n, "steps must be in time order");
        steps.push_back(step);
    }
    return Result<std::vector<ImuScriptStep>>::ok(std::move(steps));
}

std::optional<std::array<float, 3>> imu_script_accel(std::span<const ImuScriptStep> steps,
                                                     uint64_t t_ms) {
    if (steps.empty() || t_ms < steps.front().at_ms) return std::nullopt;
    std::array<float, 3> base{0, 0, 1};
    std::optional<uint64_t> shake_at;
    for (const ImuScriptStep& s : steps) {
        if (s.at_ms > t_ms) break;
        switch (s.kind) {
            case ImuScriptStep::Kind::Tilt:
                base = {s.x, s.y, s.z};
                break;
            case ImuScriptStep::Kind::Flip:
                base[2] = -base[2];
                break;
            case ImuScriptStep::Kind::Shake:
                shake_at = s.at_ms;
                break;
        }
    }
    if (shake_at) {
        const uint64_t since = t_ms - *shake_at;
        if (since < kPeaks * kPeakSpacingMs && since % kPeakSpacingMs < kPeakMs) {
            const float len = std::sqrt(base[0] * base[0] + base[1] * base[1] + base[2] * base[2]);
            if (len < 0.1f) return std::array<float, 3>{0, 0, kPeakG};
            return std::array<float, 3>{base[0] / len * kPeakG, base[1] / len * kPeakG,
                                        base[2] / len * kPeakG};
        }
    }
    return base;
}

}  // namespace wisp::port::sim
