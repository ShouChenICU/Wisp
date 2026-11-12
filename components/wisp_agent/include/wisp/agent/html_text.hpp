#pragma once

// html_to_text(): the "as":"text" conversion http.fetch offers (agent-tools
// spec 3.1) -- strips markup down to the words a reader would see, so the
// model is not handed a page's tags and scripts.
//
// Rules: <script>, <style>, <noscript> and their content, and HTML
// comments, are dropped entirely (to the matching close tag, or to the end
// of input when it is missing -- a half-received page must not hang the
// conversion). Every other tag is removed; block tags (p, div, br, li,
// h1-h6, tr) leave a newline where they stood. Entities are decoded:
// &amp; &lt; &gt; &quot; &#39; &nbsp; and numeric entities (&#NNN; and
// &#xHHH;); an unrecognized or unterminated "&" is kept literally. Runs of
// whitespace collapse to one space, runs that contain a newline to one
// newline, and the result is trimmed at both ends.
//
// The input is a response body, not necessarily well-formed HTML or even
// valid UTF-8 (a mislabeled encoding, or binary mistaken for text): any
// byte sequence that is not valid UTF-8 becomes U+FFFD, one per
// ill-formed byte, so the result is always valid UTF-8. A numeric entity
// outside the Unicode range or in the surrogate range (U+D800-U+DFFF)
// becomes U+FFFD as well.

#include <string_view>

#include "wisp/os/large.hpp"

namespace wisp::agent {

os::LargeString html_to_text(std::string_view html);

}  // namespace wisp::agent
