#pragma once

// Self-describing devices (device model spec 3.2): a driver declares the
// read-only properties and the commands a device offers as a constant
// table, and Device::describe() returns a pointer to it. The table is the
// only source: DeviceRegistry::manifest(), the admin device list and the
// agent's device.list render it into caps "schema" (render_schema()), so
// no driver writes that key by hand.
//
// Memory: a table is static data defined once per driver, not per device,
// and describe() returns a pointer to it, never a copy. ParamSpec holds a
// Value (the default of an optional argument), whose constructors are not
// constexpr, so a table is dynamically initialised into .data/.bss rather
// than placed in flash: sizeof(ParamSpec) is about 88 bytes and
// sizeof(CmdSpec) 24 on a 32-bit target, a typical table well under 1 KB, the limits below at most
// 16 + 16 * 8 ParamSpecs. Only rendering (manifest, device list) builds a
// Value tree, and only for as long as the caller keeps it.
//
// Pure functions over the table: no logging, no allocation outside the
// returned values, safe from any thread.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

enum class ParamType : uint8_t { Bool, Int, Number, String, Enum };

// One property (read-only, returned by Device::get()) or one command
// argument.
struct ParamSpec {
    const char* name;  // ^[a-z][a-z0-9_]{0,31}$
    ParamType type;
    const char* desc = "";                      // English, one line, for the agent
    double min = 0, max = 0;                    // Int/Number, inclusive; both 0: no range
    const char* unit = "";                      // "ms", "%", "°C", "lx", ...
    std::span<const char* const> choices = {};  // Enum
    uint16_t max_len = 64;                      // String, bytes of UTF-8
    bool optional = false;                      // args only
    Value def = {};                             // default for an optional arg
};

struct CmdSpec {
    const char* name;  // ^[a-z][a-z0-9_]{0,31}$
    const char* desc;
    std::span<const ParamSpec> args;  // at most 8
    bool changes_state = true;        // false: a pure query that needs arguments
    uint32_t min_interval_ms = 0;     // calls closer together are refused
};

struct DeviceSchema {
    std::span<const ParamSpec> props;  // read-only, at most 16
    std::span<const CmdSpec> cmds;     // at most 16
};

// How long Device::get()/invoke() may take (spec 3.3).
inline constexpr uint32_t kDeviceCallBudgetMs = 50;
inline constexpr size_t kMaxSchemaProps = 16, kMaxSchemaCmds = 16, kMaxCmdArgs = 8;

// "bool", "int", "number", "string", "enum".
const char* to_string(ParamType t);

// InvalidArgument naming the offending entry ("cmd 'pulse' arg 'ms': min >
// max") when: a name does not match ^[a-z][a-z0-9_]{0,31}$ or repeats
// within its list (props, cmds, one command's args; a prop and a cmd may
// share a name); a count exceeds its limit; a command's desc, or a param's
// desc or unit, is null; min > max; a range is set on a type other than
// Int/Number, or an Int range has non-integral bounds; an Enum has no
// choices or a null one, or choices are set on another type; a prop is
// optional; an optional arg's non-null default fails the arg's own
// validation. DeviceRegistry::add() runs it, so a bad table degrades only
// its device.
Result<void> check_schema(const DeviceSchema& schema);

// The caps "schema" Value; lists keep the declared order:
// {"props":[P...],"cmds":[{"name","desc","changes_state","min_interval_ms",
// "args":[P...]}]}, P = {"name","type"} plus "desc" and "unit" when
// non-empty, "min"/"max" when a range is set (int for Int, double for
// Number), "choices" for Enum, "max_len" for String, and "optional": true
// with "default" (when non-null) for an optional arg.
Value render_schema(const DeviceSchema& schema);

// Checks `args` (null means {}; anything else but a Map is refused) against
// `cmd` and returns the complete argument map, optional args that are
// missing filled with their non-null default. Int accepts an Int or an
// integral Double (stored as Int); Number accepts Int or Double (stored as
// given). Every failure is InvalidArgument naming the argument and the
// rule: "unknown argument 'n'", "missing argument 'n'", "'n' must be int",
// "'n' must be between 10 and 2000", "'n' must be one of: a, b",
// "'n' must be at most 4 bytes", "'n' must be valid UTF-8".
Result<Map> validate_args(const CmdSpec& cmd, const Value& args);

// The entry named `name`, or nullptr.
const ParamSpec* find_prop(const DeviceSchema& schema, std::string_view name);
const CmdSpec* find_cmd(const DeviceSchema& schema, std::string_view name);

// info().caps plus "schema" (render_schema(*describe())) when the device
// describes itself; what the manifest and the admin device list show.
Properties caps_of(const Device& device);

}  // namespace wisp::hal
