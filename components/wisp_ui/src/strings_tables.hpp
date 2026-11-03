#pragma once

// The three compiled-in string tables behind ui::tr() (see
// wisp/ui/strings.hpp). Each returns nullptr for an id its table lacks.
// The switches have no `default:` so the compiler flags (-Wswitch) any
// StrId a table forgot.

#include "wisp/ui/strings.hpp"

namespace wisp::ui::detail {

const char* strings_zh(StrId id);
const char* strings_ja(StrId id);
const char* strings_en(StrId id);

}  // namespace wisp::ui::detail
