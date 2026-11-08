#pragma once

// The light Markdown the home screen and the history page apply to a
// reply's body (MVP spec 6.2): "**" is dropped, a line starting "- " or
// "* " (after any indentation) starts with "• " instead. Everything else
// is plain text.
//
// A block fenced by lines starting "```" (after any indentation) is code,
// and code is never shown line by line: the block is folded to one line
// (code_fold_text(), "代码 · 120 行"), so it keeps only its line count and
// the fence's language tag, when that is short (up to kCodeLangMax of
// [A-Za-z0-9+#._-], e.g. "lua"). An unterminated fence, as while the reply
// still streams, runs to the end and counts the lines so far (one begun
// after the last newline counts; an empty one does not yet). Agents write
// scenes through their tools, so a block here is the model pasting source
// it should not have; folding it keeps the screen readable and the work a
// streamed reply costs bounded.
//
// The text is capped at max_chars code points (newlines included), keeping
// the head: a reply is read from its start, and what lies beyond the cap
// is still in the stored conversation (the admin page shows it). The cut
// ends the last text block with "…" and nothing after it is shown.
// Folded code does not count toward the cap.
//
// Consecutive text lines form one block; blank-only text blocks are
// dropped. Blocks are in PSRAM: a reply's body can run to kilobytes.
// Display only: the stored reply, what is spoken and what the agent keeps
// are untouched.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "wisp/os/large.hpp"

namespace wisp::shell {

inline constexpr size_t kBodyMaxChars = 3000;
inline constexpr size_t kCodeLangMax = 12;

struct BodyBlock {
    // A text block's text; empty for code.
    os::LargeString text;
    bool code = false;
    // Code: its lines so far, and the fence's tag ("" when it has none or
    // a long one).
    uint32_t lines = 0;
    std::string lang;

    bool operator==(const BodyBlock&) const = default;
};

os::LargeVector<BodyBlock> light_markdown(std::string_view body, size_t max_chars = kBodyMaxChars);

// The one line a code block shows, in the UI language (ui::tr()).
std::string code_fold_text(const BodyBlock& b);

}  // namespace wisp::shell
