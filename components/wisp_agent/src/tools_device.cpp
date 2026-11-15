// device.status, settings.get, settings.set, notify.list and
// notify.dismiss (agent-core spec 4.4): thin adapters over App's status
// provider, SystemSettings and NotificationService. They run on the
// agent's thread. settings.get reads the settings cell (memory);
// SystemSettings::set() writes NVS, which is flash, so settings.set is a
// job on the storage worker (run_io). The settings tools' keys, ranges and
// description come from the schema table (wisp/core/settings_schema.hpp).

#include <optional>
#include <string>
#include <utility>

#include "tool_io.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/settings_schema.hpp"
#include "wisp/ui/setting_labels.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_notice.hpp"

namespace wisp::agent {

namespace {

using detail::arg_str;
using detail::done;
using detail::fail;
using detail::run_io;

constexpr uint32_t kIoMs = 5000;

bool agent_row(const core::SettingSpec& spec) { return (spec.flags & core::kSettingAgent) != 0; }

// One clause of settings.get's description; the format is pinned by
// DeviceTools.SettingsGetDescriptionIsGenerated.
std::string describe_setting(const core::SettingSpec& spec) {
    std::string out(spec.name);
    out += " (";
    switch (spec.type) {
        case core::SettingType::Int:
            switch (spec.unit) {
                case core::SettingUnit::Seconds:
                    out += "seconds, ";
                    break;
                case core::SettingUnit::Minutes:
                    out += "minutes, ";
                    break;
                case core::SettingUnit::UtcOffsetMinutes:
                    out += "minutes east of UTC, ";
                    break;
                case core::SettingUnit::Percent:
                    out += "percent, ";
                    break;
                case core::SettingUnit::None:
                case core::SettingUnit::Level:
                case core::SettingUnit::Steps:
                    break;
            }
            if ((spec.flags & core::kSettingZeroNever) != 0) out += "0 never, or ";
            out += std::to_string(spec.min) + " to " + std::to_string(spec.max);
            break;
        case core::SettingType::Enum:
            for (size_t i = 0; i < spec.enum_values.size(); ++i) {
                if (i > 0) out += ", ";
                out += spec.enum_values[i];
            }
            break;
        case core::SettingType::Bool:
            if (spec.name == "time_24h") {
                out += "true: 24-hour clock, false: 12-hour";
            } else if (spec.name == "tts_mute") {
                out += "true: replies are shown but not spoken";
            } else {
                out += "true or false";
            }
            break;
        case core::SettingType::HostList:
            out += "comma-separated host names";
            break;
    }
    out += ")";
    return out;
}

os::LargeString get_description() {
    os::LargeString out = "The settings the agent may change: ";
    bool first = true;
    for (const core::SettingSpec& spec : core::kSettingsSchema) {
        if (!agent_row(spec)) continue;
        if (!first) out += ", ";
        first = false;
        out += describe_setting(spec);
    }
    out += ".";
    return out;
}

os::LargeString set_schema() {
    os::LargeString out = R"({"type":"object","properties":{"key":{"type":"string","enum":[)";
    bool first = true;
    for (const core::SettingSpec& spec : core::kSettingsSchema) {
        if (!agent_row(spec)) continue;
        if (!first) out += ",";
        first = false;
        out += "\"";
        out += spec.name;
        out += "\"";
    }
    out += R"(]},"value":{"type":["integer","string","boolean"]}},"required":["key","value"]})";
    return out;
}

// The agent rows under their wire names.
Map agent_view(const core::SettingsSnapshot& v) {
    Map out;
    for (const core::SettingSpec& spec : core::kSettingsSchema) {
        if (agent_row(spec))
            out[std::string(spec.name)] = core::setting_value(v, core::setting_id_of(spec));
    }
    return out;
}

// As the settings page shows the value: tts_mute is the page's "spoken
// replies" switch, so it reads inverted (settings-schema decision 7).
std::string text_of(const std::string& key, const Value& v) {
    if (v.type() == Value::Type::Bool) {
        if (key == "time_24h") return v.as_bool() ? "24h" : "12h";
        if (key == "tts_mute") return v.as_bool() ? "off" : "on";
    }
    if (v.type() == Value::Type::String) return v.as_string();
    if (v.type() == Value::Type::Int) return std::to_string(v.as_int());
    return to_json(v);
}

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

}  // namespace

Result<void> add_device_tools(ToolRegistry& reg, DeviceToolDeps deps) {
    // Lives as long as the tools, so in PSRAM with the registry.
    auto d =
        std::allocate_shared<DeviceToolDeps>(os::LargeAllocator<DeviceToolDeps>(), std::move(deps));

    WISP_TRY(reg.add({
        .name = "device.status",
        .description = "Battery, charging, network, storage use, power level, peripherals in "
                       "use, the screen's shape and its safe area.",
        .thread = ToolThread::Agent,
        .run =
            [d](const Value&, ToolCtx&) {
                const Value s = d->status ? d->status() : Value();
                Map out;
                for (const char* k : {"battery", "charging", "wifi", "storage", "power",
                                      "occupancy", "shape", "safe_area"}) {
                    const Value* f = s.type() == Value::Type::Map ? s.find(k) : nullptr;
                    out[k] = f != nullptr ? *f : Value();
                }
                return done(std::move(out));
            },
    }));

    WISP_TRY(reg.add({
        .name = "settings.get",
        .description = get_description(),
        .thread = ToolThread::Agent,
        .run =
            [d](const Value&, ToolCtx&) {
                return done({{"settings", Value(agent_view(d->settings.cell().get()))}});
            },
    }));

    WISP_TRY(reg.add({
        .name = "settings.set",
        .description = "Changes one setting, checked as the settings page checks it; see "
                       "settings.get for the keys and ranges.",
        .schema_json = set_schema(),
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .run =
            [d](const Value& a, ToolCtx& ctx) {
                const std::string key = arg_str(a, "key");
                const Value value = *a.find("value");
                struct Change {
                    Value before, after;
                };
                auto r = run_io<Change>(
                    d->submit_io,
                    [d, key, value]() -> Result<Change> {
                        Map was = agent_view(d->settings.get());
                        WISP_TRY(d->settings.set({{key, value}}, core::SettingSurface::Agent));
                        Map now = agent_view(d->settings.get());
                        return Result<Change>::ok(Change{was[key], now[key]});
                    },
                    kIoMs, ctx);
                if (!r) return fail(r.error());
                const ui::Lang lang = lang_of(ctx);
                // set() accepted the key, so it is an agent row; every agent
                // row has a device label (DeviceTools tests).
                const core::SettingSpec* spec = core::find_setting(key);
                const std::optional<ui::StrId> label =
                    spec != nullptr ? ui::setting_label(spec->label) : std::nullopt;
                std::string note =
                    ui::tr_fill_n(ui::StrId::AgentNoteSetting, lang,
                                  {label ? std::string(ui::tr(*label, lang)) : key,
                                   text_of(key, r.value().before), text_of(key, r.value().after)});
                return done({{"key", Value(key)}, {"value", r.value().after}}, std::move(note));
            },
    }));

    WISP_TRY(reg.add({
        .name = "notify.list",
        .description = "Unread notifications, newest first: source (\"system\" or a scene id), "
                       "id, and the text.",
        .thread = ToolThread::Agent,
        .run =
            [d](const Value&, ToolCtx&) {
                List list;
                for (const core::StoredNotification& n : d->notes.unread()) {
                    const std::string source(n.source.data(), n.source.size());
                    Map item{{"source", Value(source)},
                             {"id", Value(std::string(n.id.data(), n.id.size()))},
                             {"posted_unix", Value(n.posted_unix)}};
                    if (source == "system") {
                        ui::NoticeText t =
                            ui::system_notice_text(std::string_view(n.code.data(), n.code.size()),
                                                   std::string_view(n.arg.data(), n.arg.size()));
                        item["text"] = Value(std::move(t.body));
                    } else {
                        // A scene wrote these: data, never instructions
                        // (spec 6.3).
                        item["name"] = Value(scene_data(
                            source, std::string_view(n.source_name.data(), n.source_name.size())));
                        item["title"] = Value(
                            scene_data(source, std::string_view(n.title.data(), n.title.size())));
                        item["body"] = Value(
                            scene_data(source, std::string_view(n.body.data(), n.body.size())));
                    }
                    list.emplace_back(std::move(item));
                }
                return done({{"notifications", Value(std::move(list))}});
            },
    }));

    return reg.add({
        .name = "notify.dismiss",
        .description = "Dismisses one unread notification.",
        .schema_json = R"({"type":"object","properties":{"source":{"type":"string",)"
                       R"("maxLength":32},"id":{"type":"string","maxLength":64}},)"
                       R"("required":["source","id"]})",
        .thread = ToolThread::Agent,
        .run =
            [d](const Value& a, ToolCtx&) {
                if (!d->notes.dismiss(arg_str(a, "source"), arg_str(a, "id"),
                                      core::DismissReason::Dismiss)) {
                    return tool_error("not_found", "no such unread notification");
                }
                return done();
            },
    });
}

}  // namespace wisp::agent
