// docs.read (agent-core spec 4.4).

#include "wisp/agent/docs.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "docs_data.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/scene/schema.hpp"

namespace wisp::agent {

namespace {

constexpr std::string_view kWidgetPrefix = "widget/";

// The value types, properties, font sizes and the widget list of the
// generated reference, from "## Value types" up to "## Screen shape".
std::string widgets_overview() {
    const std::string all = scene::schema_markdown();
    const size_t from = all.find("## Value types");
    const size_t to = all.find("## Screen shape");
    if (from == std::string::npos) return all;
    return all.substr(from, to == std::string::npos ? std::string::npos : to - from);
}

// A topic's text: built at run time ("widgets", "widget/<name>") and kept
// here, or compiled in (DocEntry::text) and only pointed at, so that a
// page of a long static topic copies that page and not the whole topic.
class DocText {
public:
    explicit DocText(std::string built) : built_(std::move(built)), is_built_(true) {}
    explicit DocText(std::string_view fixed) : fixed_(fixed) {}
    std::string_view view() const { return is_built_ ? std::string_view(built_) : fixed_; }

private:
    std::string built_;
    std::string_view fixed_;
    bool is_built_ = false;
};

std::optional<DocText> find_doc(std::string_view topic) {
    if (topic == "widgets") return DocText(widgets_overview());
    if (topic.substr(0, kWidgetPrefix.size()) == kWidgetPrefix) {
        std::string text = scene::widget_markdown(topic.substr(kWidgetPrefix.size()));
        if (text.empty()) return std::nullopt;
        return DocText(std::move(text));
    }
    for (const DocEntry& e : doc_entries()) {
        if (e.topic == topic) return DocText(e.text);
    }
    return std::nullopt;
}

}  // namespace

std::vector<std::string> doc_topics() {
    std::vector<std::string> out{"widgets"};
    for (size_t i = 0; i < scene::kWidgetCount; ++i) {
        out.push_back(std::string(kWidgetPrefix) +
                      std::string(scene::widget_spec(static_cast<scene::Widget>(i)).name));
    }
    for (const DocEntry& e : doc_entries()) out.emplace_back(e.topic);
    return out;
}

std::optional<std::string> read_doc(std::string_view topic) {
    std::optional<DocText> doc = find_doc(topic);
    if (!doc) return std::nullopt;
    return std::string(doc->view());
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
                const std::optional<DocText> doc = find_doc(topic);
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
