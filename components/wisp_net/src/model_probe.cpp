#include "wisp/net/model_probe.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/os/large.hpp"

namespace wisp::net {

namespace {

constexpr const char* kTag = "model_probe";

// Members with a longer name are never one this file reads.
constexpr size_t kMaxKeyBytes = 24;
// Nesting a skipped value may have (a bit per level in a uint64_t).
constexpr size_t kMaxSkipDepth = 64;
constexpr std::string_view kEllipsis = "\xE2\x80\xA6";

// A JSON reader that walks the body in place, without building a tree:
// the body and everything kept from it stay in Mem::Large, and only the
// few levels this file reads recurse (skip() is iterative). Lenient about
// what it skips (it checks strings and bracket pairs there, not the
// grammar), strict about what it reads.
class Reader {
public:
    explicit Reader(std::string_view s) : s_(s) {}

    // The first byte of the next value, or 0 at the end.
    char peek() {
        ws();
        return pos_ < s_.size() ? s_[pos_] : '\0';
    }

    bool eat(char c) {
        if (peek() != c) return false;
        ++pos_;
        return true;
    }

    bool at_end() { return peek() == '\0'; }

    // A string into `out` (nullptr: read and dropped). At most `max`
    // bytes are kept, cut before a character that would not fit;
    // truncated() then says so.
    bool string(os::LargeString* out, size_t max) {
        truncated_ = false;
        if (!eat('"')) return false;
        while (pos_ < s_.size()) {
            const auto c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c < 0x20) return false;
            if (c == '\\') {
                ++pos_;
                if (!escape(out, max)) return false;
                continue;
            }
            // A whole UTF-8 sequence at a time, so a cut never splits one;
            // only continuation bytes join it, so a stray lead byte cannot
            // swallow the closing quote.
            size_t len = 1;
            const size_t want = c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            while (len < want && pos_ + len < s_.size() &&
                   (static_cast<unsigned char>(s_[pos_ + len]) & 0xC0) == 0x80) {
                ++len;
            }
            put(out, max, s_.substr(pos_, len));
            pos_ += len;
        }
        return false;
    }

    bool truncated() const { return truncated_; }

    // A number; nullopt when the next value is not one.
    std::optional<double> number() {
        ws();
        const size_t start = pos_;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        if (!digits()) return std::nullopt;
        if (pos_ < s_.size() && s_[pos_] == '.') {
            ++pos_;
            if (!digits()) return std::nullopt;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) ++pos_;
            if (!digits()) return std::nullopt;
        }
        double v = 0;
        const auto [end, ec] = std::from_chars(s_.data() + start, s_.data() + pos_, v);
        // Out of range parses as a number that is never used.
        if (end != s_.data() + pos_) return 0.0;
        return ec == std::errc() ? v : 0.0;
    }

    // Any value.
    bool skip() {
        const char c = peek();
        if (c == '"') return string(nullptr, 0);
        if (c == 't') return literal("true");
        if (c == 'f') return literal("false");
        if (c == 'n') return literal("null");
        if (c != '{' && c != '[') return number().has_value();
        uint64_t objects = 0;  // bit n: level n is an object
        size_t depth = 0;
        while (pos_ < s_.size()) {
            const char b = s_[pos_];
            if (b == '"') {
                if (!string(nullptr, 0)) return false;
                continue;
            }
            ++pos_;
            if (b == '{' || b == '[') {
                if (depth == kMaxSkipDepth) return false;
                if (b == '{') {
                    objects |= uint64_t{1} << depth;
                } else {
                    objects &= ~(uint64_t{1} << depth);
                }
                ++depth;
            } else if (b == '}' || b == ']') {
                if (depth == 0) return false;
                --depth;
                if (((objects >> depth) & 1) != (b == '}' ? 1u : 0u)) return false;
                if (depth == 0) return true;
            }
        }
        return false;
    }

    // Calls fn(key) at each member, which must read or skip its value.
    template <class F>
    bool object(F&& fn) {
        if (!eat('{')) return false;
        if (eat('}')) return true;
        os::LargeString key;
        do {
            if (!string(&key, kMaxKeyBytes) || !eat(':')) return false;
            const std::string_view k = truncated() ? std::string_view() : std::string_view(key);
            if (!fn(k)) return false;
            key.clear();
        } while (eat(','));
        return eat('}');
    }

    // Calls fn() at each element, which must read or skip it.
    template <class F>
    bool array(F&& fn) {
        if (!eat('[')) return false;
        if (eat(']')) return true;
        do {
            if (!fn()) return false;
        } while (eat(','));
        return eat(']');
    }

private:
    void ws() {
        while (pos_ < s_.size() &&
               (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
            ++pos_;
        }
    }

    bool digits() {
        const size_t start = pos_;
        while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') ++pos_;
        return pos_ > start;
    }

    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    void put(os::LargeString* out, size_t max, std::string_view bytes) {
        if (out == nullptr || truncated_) return;
        if (out->size() + bytes.size() > max) {
            truncated_ = true;
            return;
        }
        out->append(bytes.data(), bytes.size());
    }

    bool hex4(uint32_t& cp) {
        if (pos_ + 4 > s_.size()) return false;
        cp = 0;
        for (size_t i = 0; i < 4; ++i) {
            const char h = s_[pos_ + i];
            cp <<= 4;
            if (h >= '0' && h <= '9') {
                cp |= static_cast<uint32_t>(h - '0');
            } else if (h >= 'a' && h <= 'f') {
                cp |= static_cast<uint32_t>(h - 'a' + 10);
            } else if (h >= 'A' && h <= 'F') {
                cp |= static_cast<uint32_t>(h - 'A' + 10);
            } else {
                return false;
            }
        }
        pos_ += 4;
        return true;
    }

    // After the backslash.
    bool escape(os::LargeString* out, size_t max) {
        if (pos_ >= s_.size()) return false;
        const char e = s_[pos_++];
        const char* plain = nullptr;
        switch (e) {
            case '"':
                plain = "\"";
                break;
            case '\\':
                plain = "\\";
                break;
            case '/':
                plain = "/";
                break;
            case 'b':
                plain = "\b";
                break;
            case 'f':
                plain = "\f";
                break;
            case 'n':
                plain = "\n";
                break;
            case 'r':
                plain = "\r";
                break;
            case 't':
                plain = "\t";
                break;
            case 'u':
                break;
            default:
                return false;
        }
        if (plain != nullptr) {
            put(out, max, plain);
            return true;
        }
        uint32_t cp = 0;
        if (!hex4(cp)) return false;
        if (cp >= 0xD800 && cp <= 0xDBFF && s_.substr(pos_, 2) == "\\u") {
            const size_t mark = pos_;
            pos_ += 2;
            uint32_t low = 0;
            if (!hex4(low)) return false;
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            } else {
                pos_ = mark;  // read again on its own
            }
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;  // a lone surrogate
        char buf[4];
        size_t n = 0;
        if (cp < 0x80) {
            buf[n++] = static_cast<char>(cp);
        } else if (cp < 0x800) {
            buf[n++] = static_cast<char>(0xC0 | (cp >> 6));
            buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            buf[n++] = static_cast<char>(0xE0 | (cp >> 12));
            buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            buf[n++] = static_cast<char>(0xF0 | (cp >> 18));
            buf[n++] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            buf[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            buf[n++] = static_cast<char>(0x80 | (cp & 0x3F));
        }
        put(out, max, std::string_view(buf, n));
        return true;
    }

    std::string_view s_;
    size_t pos_ = 0;
    bool truncated_ = false;
};

bool leaks(std::string_view s, std::string_view key) {
    return !key.empty() && s.find(key) != std::string_view::npos;
}

// What one /models entry yields, and the shared voice budget.
struct ListParse {
    Reader& r;
    std::string_view key;
    size_t voices_left = kMaxListedVoices;

    // Set by text() when a string it read held the key.
    bool leaked = false;

    // A string member of at most `max` bytes into `out`; anything else, a
    // longer one or one that holds the key leaves `out` empty. With `cut`
    // a longer one is shortened on a character boundary and ends in an
    // ellipsis instead.
    bool text(os::LargeString& out, size_t max, bool cut = false) {
        out.clear();
        if (r.peek() != '"') return r.skip();
        if (!r.string(&out, max)) return false;
        if (r.truncated()) {
            if (cut) {
                size_t n = max - kEllipsis.size();
                while (n > 0 && (static_cast<unsigned char>(out[n]) & 0xC0) == 0x80) --n;
                out.resize(n);
                out.append(kEllipsis.data(), kEllipsis.size());
            } else {
                out.clear();
            }
        }
        if (leaks(out, key)) {
            leaked = true;
            out.clear();
        }
        return true;
    }

    // A whole number in [1, UINT32_MAX] into `out`; anything else leaves it.
    bool count(uint32_t& out) {
        if (r.peek() == '"' || r.peek() == '{' || r.peek() == '[' || r.peek() == 't' ||
            r.peek() == 'f' || r.peek() == 'n') {
            return r.skip();
        }
        const std::optional<double> v = r.number();
        if (!v) return false;
        if (*v >= 1 && *v <= 4294967295.0 && *v == static_cast<double>(static_cast<uint64_t>(*v)))
            out = static_cast<uint32_t>(*v);
        return true;
    }

    // A list of strings into `out`, up to `limit` of them.
    bool strings(os::LargeVector<os::LargeString>& out, size_t limit, size_t max) {
        out.clear();
        if (r.peek() != '[') return r.skip();
        return r.array([&] {
            os::LargeString s;
            if (!text(s, max)) return false;
            if (!s.empty() && out.size() < limit) out.push_back(std::move(s));
            return true;
        });
    }

    bool voice(core::ModelInfo& info) {
        if (r.peek() != '{') return r.skip();
        core::ModelVoiceInfo v;
        leaked = false;
        const bool ok = r.object([&](std::string_view k) {
            if (k == "name") return text(v.name, kMaxVoiceNameBytes);
            if (k == "language") return text(v.language, kMaxLanguageBytes);
            if (k == "gender") return text(v.gender, kMaxGenderBytes);
            if (k == "description") return text(v.description, kMaxVoiceDescriptionBytes, true);
            return r.skip();
        });
        if (!ok) return false;
        // A voice with any field that holds the key goes as a whole.
        if (!leaked && !v.name.empty() && info.voices.size() < kMaxModelVoices && voices_left > 0) {
            info.voices.push_back(std::move(v));
            --voices_left;
        }
        return true;
    }

    bool speech(core::ModelInfo& info) {
        if (r.peek() != '{') return r.skip();
        return r.object([&](std::string_view k) {
            if (k == "task") {
                os::LargeString task;
                if (!text(task, 16)) return false;
                if (task == "synthesis") {
                    info.kind = core::ModelKind::Tts;
                } else if (task == "recognition") {
                    info.kind = core::ModelKind::Asr;
                }
                return true;
            }
            if (k == "sample_rate") return count(info.sample_rate);
            if (k == "languages")
                return strings(info.languages, kMaxModelLanguages, kMaxLanguageBytes);
            if (k == "voices") {
                info.voices.clear();
                if (r.peek() != '[') return r.skip();
                return r.array([&] { return voice(info); });
            }
            return r.skip();
        });
    }

    // OpenRouter: input_modalities ["text","image"], or the older
    // modality "text+image->text".
    bool architecture(core::ModelInfo& info, bool& from_list) {
        if (r.peek() != '{') return r.skip();
        return r.object([&](std::string_view k) {
            if (k == "input_modalities") {
                os::LargeVector<os::LargeString> inputs;
                if (r.peek() != '[') return r.skip();
                if (!strings(inputs, 16, 32)) return false;
                bool image = false;
                for (const auto& m : inputs) image = image || m == "image";
                info.vision = image;
                from_list = true;
                return true;
            }
            if (k == "modality" && !from_list) {
                os::LargeString m;
                if (!text(m, 64)) return false;
                if (!m.empty()) {
                    const std::string_view in = std::string_view(m).substr(0, m.find("->"));
                    info.vision = in.find("image") != std::string_view::npos;
                }
                return true;
            }
            return r.skip();
        });
    }

    // One element of "data"; `id` empty when it has no usable one.
    bool entry(core::ModelInfo& info) {
        if (r.peek() != '{') return r.skip();
        bool from_list = false;
        return r.object([&](std::string_view k) {
            if (k == "id") return text(info.id, kMaxModelIdBytes);
            if (k == "speech") return speech(info);
            if (k == "context_length" || k == "context_window") return count(info.context_window);
            if (k == "architecture") return architecture(info, from_list);
            return r.skip();
        });
    }
};

bool says_more(const core::ModelInfo& m) {
    return m.kind || m.sample_rate != 0 || m.context_window != 0 || m.vision ||
           !m.languages.empty() || !m.voices.empty();
}

}  // namespace

bool parse_model_list(std::string_view body, std::string_view key, core::ModelProbe& probe) {
    probe.models.clear();
    probe.details.clear();
    Reader r(body);
    ListParse lp{r, key};
    bool listed = false;
    const bool ok = r.object([&](std::string_view k) {
        if (k != "data") return r.skip();
        if (r.peek() != '[') return false;
        listed = true;
        probe.models.clear();
        probe.details.clear();
        return r.array([&] {
            if (probe.models.size() >= kMaxListedModels) return r.skip();
            core::ModelInfo info;
            if (!lp.entry(info)) return false;
            if (info.id.empty()) return true;
            probe.models.push_back(info.id);
            if (says_more(info)) probe.details.push_back(std::move(info));
            return true;
        });
    });
    if (!ok || !listed || !r.at_end()) {
        probe.models.clear();
        probe.details.clear();
        return false;
    }
    return true;
}

core::ModelProbe probe_models(const core::ResolvedProfile& target, HttpClient& client,
                              const std::function<uint64_t()>& now_ms) {
    core::ModelProbe probe;
    if (target.base_url.empty()) {
        probe.error = to_string(TransportError::Other);
        return probe;
    }

    HttpClientRequest req;
    req.method = Method::Get;
    // ModelProfileService strips trailing '/' from base_url.
    req.url = target.base_url + "/models";
    req.timeout_ms = kModelProbeTimeoutMs;
    req.headers.emplace_back("Accept", "application/json");
    if (!target.api_key.empty())
        req.headers.emplace_back("Authorization", "Bearer " + target.api_key);
    // Straight into Mem::Large: a list with voices and descriptions can be
    // tens of KB. A failed request's body is dropped unread (it may quote
    // the key back).
    req.max_response_bytes = kModelProbeMaxBytes;
    int status = 0;
    os::LargeString body;
    req.on_status = [&status](int s) { status = s; };
    req.on_body = [&](std::span<const uint8_t> bytes) {
        if (status != 200) return true;
        if (bytes.size() > kModelProbeMaxBytes - body.size()) return false;
        body.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    };

    const uint64_t start = now_ms();
    HttpClientResponse res = client.request(req);
    const uint64_t end = now_ms();
    probe.latency_ms = static_cast<uint32_t>(end >= start ? end - start : 0);
    if (res.transport != TransportError::None) {
        // The models page (web/app/pages/models.js) knows dns, connect,
        // tls, timeout and other: an oversized model list is "other".
        const bool listed =
            res.transport != TransportError::TooLarge && res.transport != TransportError::Cancelled;
        probe.error = to_string(listed ? res.transport : TransportError::Other);
        WLOG_I(kTag, "probe of %s failed: transport error %s", target.id.c_str(),
               probe.error.c_str());
        return probe;
    }
    probe.http_status = res.status;
    if (res.status != 200) {
        probe.error = res.status == 404 ? "no_model_list" : "http";
        WLOG_I(kTag, "probe of %s failed: HTTP %d", target.id.c_str(), res.status);
        return probe;
    }

    if (!parse_model_list(body, target.api_key, probe)) {
        probe.error = "http";
        WLOG_I(kTag, "probe of %s failed: HTTP 200 without a model list", target.id.c_str());
        return probe;
    }
    probe.ok = true;
    WLOG_I(kTag, "probe of %s ok: %u ms, %u models, %u with details", target.id.c_str(),
           static_cast<unsigned>(probe.latency_ms), static_cast<unsigned>(probe.models.size()),
           static_cast<unsigned>(probe.details.size()));
    return probe;
}

core::ModelProber make_model_prober(HttpClient& client, std::function<uint64_t()> now_ms) {
    return [&client, now_ms = std::move(now_ms)](const core::ResolvedProfile& target) {
        return probe_models(target, client, now_ms);
    };
}

}  // namespace wisp::net
