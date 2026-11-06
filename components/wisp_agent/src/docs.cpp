// docs.read (agent-core spec 4.4).

#include "wisp/agent/docs.hpp"

#include <string>

#include "docs_data.hpp"
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
    if (topic == "widgets") return widgets_overview();
    if (topic.substr(0, kWidgetPrefix.size()) == kWidgetPrefix) {
        std::string text = scene::widget_markdown(topic.substr(kWidgetPrefix.size()));
        if (text.empty()) return std::nullopt;
        return text;
    }
    for (const DocEntry& e : doc_entries()) {
        if (e.topic == topic) return std::string(e.text);
    }
    return std::nullopt;
}

Result<void> add_docs_tools(ToolRegistry& reg) {
    return reg.add({
        .name = "docs.read",
        .description = "The scene API reference by topic: widgets, widget/<name>, lua, "
                       "lua/<module> (store, http, timer, device, scene, log, notify, power, "
                       "sensor, files), screen, examples, example/<id>. An unknown topic "
                       "lists them all.",
        .schema_json = R"({"type":"object","properties":{"topic":{"type":"string",)"
                       R"("maxLength":64}},"required":["topic"]})",
        .thread = ToolThread::Agent,
        .run =
            [](const Value& a, ToolCtx&) {
                const Value* t = a.find("topic");
                const std::string topic =
                    t != nullptr && t->type() == Value::Type::String ? t->as_string() : "";
                std::optional<std::string> text = read_doc(topic);
                if (!text) {
                    List topics;
                    for (std::string& s : doc_topics()) topics.emplace_back(std::move(s));
                    return tool_error("not_found", "no topic " + topic,
                                      {{"topics", Value(std::move(topics))}});
                }
                ToolOutput out;
                out.result = Value(Map{{"ok", Value(true)},
                                       {"topic", Value(topic)},
                                       {"text", Value(std::move(*text))}});
                return out;
            },
    });
}

}  // namespace wisp::agent
