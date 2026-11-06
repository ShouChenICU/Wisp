#include "wisp/ui/strings.hpp"

#include <atomic>

#include "strings_tables.hpp"

namespace wisp::ui {

namespace {

std::atomic<Lang> g_lang{Lang::Zh};

}  // namespace

namespace detail {

const char* table_text(StrId id, Lang lang) {
    if (static_cast<size_t>(id) >= kStrCount) {
        return nullptr;
    }
    switch (lang) {
        case Lang::Zh:
            return strings_zh(id);
        case Lang::Ja:
            return strings_ja(id);
        case Lang::En:
            return strings_en(id);
    }
    return nullptr;
}

}  // namespace detail

const char* tr(StrId id, Lang lang) {
    const char* text = detail::table_text(id, lang);
    if (text == nullptr || *text == '\0') {
        text = detail::table_text(id, Lang::Zh);
    }
    return text != nullptr ? text : "";
}

const char* tr(StrId id) { return tr(id, g_lang.load(std::memory_order_relaxed)); }

std::string tr_fill(StrId id, Lang lang, std::string_view value) {
    std::string text = tr(id, lang);
    size_t pos = text.find("{}");
    if (pos != std::string::npos) {
        text.replace(pos, 2, value);
    }
    return text;
}

std::string tr_fill_n(StrId id, Lang lang, std::initializer_list<std::string_view> values) {
    return tr_fill_n(id, lang, std::span<const std::string_view>(values.begin(), values.size()));
}

std::string tr_fill_n(StrId id, Lang lang, std::span<const std::string_view> values) {
    std::string text = tr(id, lang);
    size_t from = 0;
    for (std::string_view v : values) {
        const size_t pos = text.find("{}", from);
        if (pos == std::string::npos) break;
        text.replace(pos, 2, v);
        from = pos + v.size();
    }
    return text;
}

std::string tr_fill_n(StrId id, std::span<const std::string_view> values) {
    return tr_fill_n(id, g_lang.load(std::memory_order_relaxed), values);
}

std::string tr_fill(StrId id, std::string_view value) {
    return tr_fill(id, g_lang.load(std::memory_order_relaxed), value);
}

void set_lang(Lang lang) { g_lang.store(lang, std::memory_order_relaxed); }

Lang lang() { return g_lang.load(std::memory_order_relaxed); }

std::optional<Lang> parse_lang(std::string_view code) {
    for (Lang lang : kAllLangs) {
        if (code == lang_code(lang)) {
            return lang;
        }
    }
    return std::nullopt;
}

const char* lang_code(Lang lang) {
    switch (lang) {
        case Lang::Zh:
            return "zh";
        case Lang::Ja:
            return "ja";
        case Lang::En:
            return "en";
    }
    return "zh";
}

}  // namespace wisp::ui
