#pragma once

// The light Markdown the home screen applies to a reply's body (MVP spec
// 6.2): "**" is dropped, a line starting "- " or "* " (after any
// indentation) starts with "• " instead, and a block fenced by lines
// starting "```" is code, shown as it is (the device has no monospaced
// font: the home screen gives code the body font on a muted background).
// Everything else is plain text. An unterminated fence, as while the
// reply still streams, runs to the end.
//
// Consecutive text lines form one block; blank-only blocks are dropped.
// Blocks are in PSRAM: a reply's body can run to kilobytes.

#include <string_view>

#include "wisp/os/large.hpp"

namespace wisp::shell {

struct BodyBlock {
    os::LargeString text;
    bool code = false;

    bool operator==(const BodyBlock&) const = default;
};

os::LargeVector<BodyBlock> light_markdown(std::string_view body);

}  // namespace wisp::shell
