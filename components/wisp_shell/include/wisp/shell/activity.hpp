#pragma once

// The agent's steps as the screen names them (MVP spec 6.2): what the
// home screen's step list and the talk overlay show while a turn runs.
//
// A step's label is a friendly name for its tool from the string table
// (the Act* ids; "调用 <tool>" for a tool without one) and, for the tools
// that act on one thing, that thing: one short target taken from the
// arguments' summary (activity_target()). Nothing else of the arguments
// ever shows: file bodies, memory text and settings values can be
// personal, and a settings value may be a key, so the settings tools
// never show a target at all.
//
// Everything is built into os::LargeString (PSRAM on the device): the
// labels are made on wisp.agent for every tool event.

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "wisp/os/large.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::shell {

// A target is cut to this many code points.
inline constexpr size_t kActivityTargetChars = 32;

// The value of the first of the keys "path", "name", "title", "id" found
// in `args` (a JSON object's text, possibly cut anywhere: the agent's
// summaries are its first 200 code points): a string's text (escapes
// read, control characters dropped; what there is of it when the text is
// cut inside it) or a number. Keys are told from text inside string
// values. "" when there is none. Cut to kActivityTargetChars.
os::LargeString activity_target(std::string_view args);

// `tool` (dotted) with its target from `args`, e.g. "写入文件 main.lua".
os::LargeString activity_label(std::string_view tool, std::string_view args, ui::Lang lang);
inline os::LargeString activity_label(std::string_view tool, std::string_view args) {
    return activity_label(tool, args, ui::lang());
}

// A call the model is still writing: its label without a target and,
// once any arrived, the arguments' size ("写入文件 · 3.2 KB").
os::LargeString activity_prep_label(std::string_view tool, uint32_t bytes, ui::Lang lang);

// The line under the steps: "第 N 步 · 42 秒" while the turn runs,
// "用了 N 步 · 1 分 12 秒" once it has ended.
os::LargeString activity_summary(uint32_t steps, uint64_t elapsed_ms, bool running, ui::Lang lang);

}  // namespace wisp::shell
