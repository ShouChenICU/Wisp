// docs.read (agent-core spec 4.4).

#include "wisp/agent/docs.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "docs_data.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/inflate.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/schema.hpp"

namespace wisp::agent {

namespace {

constexpr std::string_view kWidgetPrefix = "widget/";

// Compiled-in text, inflated for the one read that needs it and dropped
// after: the docs are read rarely, and 100 KB held for good is not worth
// a few milliseconds. Empty (!ok()) when it does not fit in memory or the
// stream is corrupt.
os::LargeBuffer unpack(const PackedText& p) {
    os::LargeBuffer out = os::LargeBuffer::allocate(p.size);
    if (!out.ok()) return out;
    const Result<size_t> n = inflate_raw(p.deflated, out.span());
    if (!n || n.value() != p.size) return {};
    return out;
}

std::string_view view_of(const os::LargeBuffer& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// The value types, properties, font sizes and the widget list of
// docs/scene-api.md, from "## Value types" up to "## Screen shape".
std::string_view widgets_overview(std::string_view md) {
    const size_t from = md.find("## Value types");
    const size_t to = md.find("## Screen shape");
    if (from == std::string_view::npos) return md;
    return md.substr(from, to == std::string_view::npos ? std::string_view::npos : to - from);
}

// lua, lua/<module> and screen are docs/scene-api.md cut at its "## "
// headings: "## wisp.<module>..." goes to lua/<module>, "## Screen shape"
// to screen, everything else (the intro included) to lua; a "### " heading
// stays with its section and HTML comments, with the newlines after them,
// are left out.
constexpr std::string_view kBlank = " \t\n\r\f\v";

std::string_view trim(std::string_view s) {
    const size_t b = s.find_first_not_of(kBlank);
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(kBlank) - b + 1);
}

// The topic a "## " heading's title starts.
std::string section_topic(std::string_view title) {
    title = trim(title);
    constexpr std::string_view kModule = "wisp.";
    if (title.substr(0, kModule.size()) == kModule) {
        size_t end = kModule.size();
        while (end < title.size() &&
               ((title[end] >= 'a' && title[end] <= 'z') || title[end] == '_')) {
            ++end;
        }
        if (end > kModule.size()) {
            return "lua/" + std::string(title.substr(kModule.size(), end - kModule.size()));
        }
    }
    return title == "Screen shape" ? "screen" : "lua";
}

// Calls fn(piece) for the pieces of `md` between its HTML comments.
template <class Fn>
void for_each_uncommented(std::string_view md, Fn&& fn) {
    while (!md.empty()) {
        const size_t at = md.find("<!--");
        const size_t close = at == std::string_view::npos ? at : md.find("-->", at);
        if (close == std::string_view::npos) {
            fn(md);
            return;
        }
        fn(md.substr(0, at));
        size_t end = close + 3;
        while (end < md.size() && md[end] == '\n') ++end;
        md.remove_prefix(end);
    }
}

// Calls fn(topic, line) for every line of `md` outside its HTML comments,
// its newline included. A line split by a comment comes as two pieces.
template <class Fn>
void for_each_line(std::string_view md, Fn&& fn) {
    std::string topic = "lua";
    bool line_start = true;
    for_each_uncommented(md, [&](std::string_view piece) {
        while (!piece.empty()) {
            const size_t nl = piece.find('\n');
            const size_t len = nl == std::string_view::npos ? piece.size() : nl + 1;
            const std::string_view line = piece.substr(0, len);
            piece.remove_prefix(len);
            if (line_start && line.substr(0, 3) == "## ") topic = section_topic(line.substr(3));
            line_start = nl != std::string_view::npos;
            fn(std::string_view(topic), line);
        }
    });
}

std::optional<os::LargeString> scene_api_topic(std::string_view md, std::string_view topic) {
    size_t size = 0;
    bool found = topic == "lua";
    for_each_line(md, [&](std::string_view t, std::string_view line) {
        if (t == topic) size += line.size();
        found = found || t == topic;
    });
    if (!found) return std::nullopt;
    os::LargeString out;
    out.reserve(size + 1);
    for_each_line(md, [&](std::string_view t, std::string_view line) {
        if (t == topic) out.append(line);
    });
    const size_t b = out.find_first_not_of(kBlank);
    if (b == os::LargeString::npos) {
        out.clear();
    } else {
        out.erase(out.find_last_not_of(kBlank) + 1);
        out.erase(0, b);
    }
    out.push_back('\n');
    return out;
}

// lua, then lua/<module> by name, then screen.
std::vector<std::string> scene_api_topics(std::string_view md) {
    std::vector<std::string> modules;
    bool screen = false;
    for_each_line(md, [&](std::string_view t, std::string_view) {
        if (t == "screen") screen = true;
        if (t.substr(0, 4) == "lua/" &&
            std::find(modules.begin(), modules.end(), t) == modules.end())
            modules.emplace_back(t);
    });
    std::sort(modules.begin(), modules.end());
    std::vector<std::string> out{"lua"};
    for (std::string& m : modules) out.push_back(std::move(m));
    if (screen) out.emplace_back("screen");
    return out;
}

bool is_scene_api_topic(std::string_view topic) {
    return topic == "lua" || topic == "screen" || topic.substr(0, 4) == "lua/";
}

// A topic's text and what holds it: the inflated docs it is a part of, or
// text built for it.
class DocText {
public:
    DocText(os::LargeBuffer packed, std::string_view part)
        : packed_(std::move(packed)), part_(part) {}
    explicit DocText(os::LargeString built) : built_(std::move(built)), is_built_(true) {}
    std::string_view view() const { return is_built_ ? std::string_view(built_) : part_; }

private:
    os::LargeBuffer packed_;
    std::string_view part_;  // into packed_, whose bytes stay put when it moves
    os::LargeString built_;
    bool is_built_ = false;
};

enum class Find { Found, NotFound, NoMemory };

struct Found {
    Find status = Find::NotFound;
    std::optional<DocText> doc;
};

Found find_doc(std::string_view topic) {
    if (topic.substr(0, kWidgetPrefix.size()) == kWidgetPrefix) {
        const std::string text = scene::widget_markdown(topic.substr(kWidgetPrefix.size()));
        if (text.empty()) return {};
        return {Find::Found, DocText(os::LargeString(text.begin(), text.end()))};
    }
    if (topic == "widgets" || is_scene_api_topic(topic)) {
        os::LargeBuffer md = unpack(scene_api_text());
        if (!md.ok()) return {Find::NoMemory, std::nullopt};
        if (topic == "widgets") {
            const std::string_view part = widgets_overview(view_of(md));
            return {Find::Found, DocText(std::move(md), part)};
        }
        std::optional<os::LargeString> text = scene_api_topic(view_of(md), topic);
        if (!text) return {};
        return {Find::Found, DocText(std::move(*text))};
    }
    for (const DocEntry& e : doc_entries()) {
        if (e.topic != topic) continue;
        os::LargeBuffer all = unpack(examples_text());
        if (!all.ok()) return {Find::NoMemory, std::nullopt};
        const std::string_view part = view_of(all).substr(e.offset, e.size);
        return {Find::Found, DocText(std::move(all), part)};
    }
    return {};
}

}  // namespace

std::vector<std::string> doc_topics() {
    std::vector<std::string> out{"widgets"};
    for (size_t i = 0; i < scene::kWidgetCount; ++i) {
        out.push_back(std::string(kWidgetPrefix) +
                      std::string(scene::widget_spec(static_cast<scene::Widget>(i)).name));
    }
    // Without the memory to inflate the scene API, its topics go unlisted
    // rather than the whole list failing.
    const os::LargeBuffer md = unpack(scene_api_text());
    if (md.ok()) {
        for (std::string& t : scene_api_topics(view_of(md))) out.push_back(std::move(t));
    }
    for (const DocEntry& e : doc_entries()) out.emplace_back(e.topic);
    return out;
}

std::optional<std::string> read_doc(std::string_view topic) {
    const Found f = find_doc(topic);
    if (!f.doc) return std::nullopt;
    return std::string(f.doc->view());
}

Result<void> add_docs_tools(ToolRegistry& reg) {
    return reg.add({
        .name = "docs.read",
        .description = "The scene API reference by topic: widgets, widget/<name>, lua, "
                       "lua/<module> (store, http, timer, device, scene, log, notify, power, "
                       "sensor, files), screen, examples, example/<id>. An unknown topic "
                       "lists them all. Long topics come in pages: pass the result's next_offset "
                       "to read on.",
        .schema_json =
            R"({"type":"object","properties":{"topic":{"type":"string",)"
            R"("maxLength":64},"offset":{"type":"integer","minimum":0}},"required":["topic"]})",
        .thread = ToolThread::Agent,
        .run =
            [](const Value& a, ToolCtx&) {
                const Value* t = a.find("topic");
                const std::string topic =
                    t != nullptr && t->type() == Value::Type::String ? t->as_string() : "";
                const Found found = find_doc(topic);
                if (found.status == Find::NoMemory) {
                    return tool_error("no_memory", "not enough memory to read " + topic);
                }
                const std::optional<DocText>& doc = found.doc;
                if (!doc) {
                    List topics;
                    for (std::string& s : doc_topics()) topics.emplace_back(std::move(s));
                    return tool_error("not_found", "no topic " + topic,
                                      {{"topics", Value(std::move(topics))}});
                }
                const std::string_view text = doc->view();
                const Value* o = a.find("offset");
                // An offset given but not an integer is refused rather than
                // read as 0: the page would not be the one asked for.
                const bool is_int = o == nullptr || o->type() == Value::Type::Int;
                const int64_t want = o != nullptr && is_int ? o->as_int() : 0;
                const size_t total = text.size();
                // A byte offset in the middle of a UTF-8 sequence would split a
                // character between two pages.
                const bool on_boundary = is_int && want >= 0 &&
                                         static_cast<size_t>(want) <= total &&
                                         (static_cast<size_t>(want) == total ||
                                          (static_cast<uint8_t>(text[want]) & 0xC0) != 0x80);
                if (!on_boundary) {
                    return tool_error("bad_offset",
                                      "offset is not an integer, past the end or inside a "
                                      "character",
                                      {{"total_bytes", Value(static_cast<int64_t>(total))}});
                }
                const size_t begin = static_cast<size_t>(want);
                size_t end = total;
                bool more = false;
                if (total - begin > kDocsPageBytes) {
                    more = true;
                    end = begin + kDocsPageBytes;
                    const size_t nl = text.rfind('\n', end - 1);
                    if (nl != std::string::npos && nl >= begin) {
                        end = nl + 1;
                    } else {
                        while (end > begin && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) {
                            --end;
                        }
                    }
                }
                Map result{{"ok", Value(true)},
                           {"topic", Value(topic)},
                           {"text", Value(std::string(text.substr(begin, end - begin)))},
                           {"total_bytes", Value(static_cast<int64_t>(total))}};
                if (more) result.emplace("next_offset", Value(static_cast<int64_t>(end)));
                ToolOutput out;
                out.result = Value(std::move(result));
                return out;
            },
    });
}

}  // namespace wisp::agent
