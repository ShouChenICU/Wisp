// device.list and device.call (device model spec 4.2): thin adapters over
// core::DeviceService, the one place every check on a self-describing
// device lives (AGENTS.md section 3, service layer). Beyond the registry's
// argument schema (lengths, the op enum, args an object) nothing is checked
// here: no device lookup, no range, no rate, so the agent and the Lua
// bindings are refused for exactly the same reasons.
//
// Both run on the agent's own thread (ToolThread::Agent): DeviceService and
// drivers never touch flash (hal::Device::invoke()'s contract), so no hop to
// the storage worker is needed. An Agent tool runs inline in
// ToolRegistry::call(), so timeout_ms does not interrupt it: a driver is
// bound to return within hal::kDeviceCallBudgetMs, and one that overruns
// holds the agent's thread (and the turn's cancel) until it returns.
//
// Logs carry the device id and command only (DeviceService's line), never
// the arguments, which may carry user content.

#include <string>
#include <utility>

#include "tool_io.hpp"
#include "tools_json.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/core/device_service.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

using detail::arg_str;
using detail::done;
using detail::fail;

// device.list's entries at most. A schema renders to well under 1 KiB, so
// the cap keeps the result a few tens of KB on any board.
constexpr size_t kMaxListed = 32;

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

ToolOutput list_devices(const core::DeviceService& devices) {
    const List list = devices.list();
    ToolOutput out;
    // Written straight into PSRAM (os::LargeString): a board with many
    // devices builds a result past the internal-RAM malloc threshold.
    auto& json = out.result_json;
    json = R"({"ok":true,"devices":[)";
    const size_t n = list.size() < kMaxListed ? list.size() : kMaxListed;
    for (size_t i = 0; i < n; ++i) {
        if (i != 0) json.push_back(',');
        detail::append_value(json, list[i]);
    }
    json.push_back(']');
    if (list.size() > kMaxListed) json.append(R"(,"truncated":true)");
    json.push_back('}');
    return out;
}

ToolOutput call_device(core::DeviceService& devices, const Value& a, const ToolCtx& ctx) {
    const std::string& id = arg_str(a, "id");
    const std::string& name = arg_str(a, "name");
    if (arg_str(a, "op") == "get") {
        auto r = devices.get(id, name);
        if (!r) return fail(r.error());
        return done({{"value", std::move(r.value())}});
    }
    const Value* args = a.find("args");
    auto r = devices.call(id, name, args != nullptr ? *args : Value(), "agent");
    if (!r) return fail(r.error());
    // Asked after the call succeeded, so a refused or failed command shows
    // no note, and the note names only an id and a command that exist.
    std::string note;
    if (devices.changes_state(id, name)) {
        note = ui::tr_fill_n(ui::StrId::AgentNoteDevice, lang_of(ctx), {id, name});
    }
    return done({{"result", std::move(r.value())}}, std::move(note));
}

}  // namespace

Result<void> add_device_call_tools(ToolRegistry& reg, DeviceCallToolDeps deps) {
    core::DeviceService* devices = &deps.devices;

    WISP_TRY(reg.add({
        .name = "device.list",
        .description =
            "Peripherals that describe themselves (a vibration motor, a light, a sensor, ...): "
            "id, class, model, role and schema. schema.props are read-only values: read one "
            "with device_call op \"get\". schema.cmds are commands: run one with op \"invoke\" "
            "and its args (type; min and max inclusive; unit; choices; optional ones have a "
            "default). A command with changes_state false only reads. The screen, microphone, "
            "speaker, Wi-Fi, storage and battery are not here; see device_status.",
        .thread = ToolThread::Agent,
        .run = [devices](const Value&, ToolCtx&) { return list_devices(*devices); },
    }));

    return reg.add({
        .name = "device.call",
        .description =
            "Reads a property (op \"get\", name = the property) or runs a command (op "
            "\"invoke\", name = the command, args = its arguments) of a device listed by "
            "device_list. Arguments are checked against the schema before the device is "
            "touched. Errors: not_found (no such device, property or command), invalid_value "
            "(the message names the argument and the rule), rate_limited (called again too "
            "soon; the message gives retry_after_ms), busy (in use elsewhere, or off while the "
            "screen is off), device_failure.",
        .schema_json = R"({"type":"object","properties":{)"
                       R"("id":{"type":"string","maxLength":32},)"
                       R"("op":{"type":"string","enum":["get","invoke"]},)"
                       R"("name":{"type":"string","maxLength":32},)"
                       R"("args":{"type":"object"}},)"
                       R"("required":["id","op","name"]})",
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = 5000,
        .run = [devices](const Value& a, ToolCtx& ctx) { return call_device(*devices, a, ctx); },
    });
}

}  // namespace wisp::agent
