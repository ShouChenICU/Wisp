// device.status, settings.get, settings.set, notify.list and
// notify.dismiss (agent-core spec 4.4): thin adapters over App's status
// provider, SystemSettings and NotificationService. They run on the
// agent's thread; SystemSettings reads and writes NVS, which is flash, so
// every call into it is a job on the storage worker (run_io).

#include <string>
#include <utility>

#include "tool_io.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_notice.hpp"

namespace wisp::agent {

namespace {

using detail::arg_str;
using detail::done;
using detail::fail;
using detail::run_io;

constexpr uint32_t kIoMs = 5000;

// The keys the agent may read and change, with what the note calls them.
struct SettingKey {
    const char* key;
    ui::StrId label;
};
constexpr SettingKey kKeys[] = {
    {"brightness", ui::StrId::AgentSetBrightness}, {"volume", ui::StrId::AgentSetVolume},
    {"notify_vol", ui::StrId::AgentSetNotifyVol},  {"screen_off_s", ui::StrId::AgentSetScreenOff},
    {"language", ui::StrId::AgentSetLanguage},
};

Map agent_view(const core::SystemSettingsValues& v) {
    Map all = core::to_map(v);
    Map out;
    for (const SettingKey& k : kKeys) {
        auto it = all.find(k.key);
        out[k.key] = it != all.end() ? it->second : Value();
    }
    return out;
}

std::string text_of(const Value& v) {
    if (v.type() == Value::Type::String) return v.as_string();
    if (v.type() == Value::Type::Int) return std::to_string(v.as_int());
    return to_json(v);
}

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

}  // namespace

Result<void> add_device_tools(ToolRegistry& reg, DeviceToolDeps deps) {
    auto d = std::make_shared<DeviceToolDeps>(std::move(deps));

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
        .description = "The settings the agent may change: brightness (10-255), volume and "
                       "notify_vol (0-100), screen_off_s (0 never, or 15-3600) and language "
                       "(zh, ja, en).",
        .thread = ToolThread::Agent,
        .run =
            [d](const Value&, ToolCtx& ctx) {
                auto r = run_io<Map>(
                    d->submit_io, [d]() { return Result<Map>::ok(agent_view(d->settings.get())); },
                    kIoMs, ctx);
                if (!r) return fail(r.error());
                return done({{"settings", Value(std::move(r.value()))}});
            },
    }));

    WISP_TRY(reg.add({
        .name = "settings.set",
        .description = "Changes one setting, checked as the settings page checks it; see "
                       "settings.get for the keys and ranges.",
        .schema_json = R"({"type":"object","properties":{"key":{"type":"string","enum":)"
                       R"(["brightness","volume","notify_vol","screen_off_s","language"]},)"
                       R"("value":{"type":["integer","string"]}},"required":["key","value"]})",
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
                        WISP_TRY(d->settings.set({{key, value}}));
                        Map now = agent_view(d->settings.get());
                        return Result<Change>::ok(Change{was[key], now[key]});
                    },
                    kIoMs, ctx);
                if (!r) return fail(r.error());
                ui::StrId label = ui::StrId::AgentSetBrightness;
                for (const SettingKey& k : kKeys) {
                    if (key == k.key) label = k.label;
                }
                const ui::Lang lang = lang_of(ctx);
                std::string note = ui::tr_fill_n(
                    ui::StrId::AgentNoteSetting, lang,
                    {ui::tr(label, lang), text_of(r.value().before), text_of(r.value().after)});
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
