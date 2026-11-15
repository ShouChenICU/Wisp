#pragma once

// Device text for the settings schema's label ids (core::SettingSpec::label,
// settings-schema spec 3.1). The schema lives in wisp_core, which cannot
// name StrId, so the label is a plain id and this maps it to the text the
// settings page shows. Labels only the web page shows (time servers, the
// agent limits) have no device text.

#include <optional>
#include <string_view>

#include "wisp/ui/strings.hpp"

namespace wisp::ui {

// The device text id for a schema label id (core::SettingSpec::label).
std::optional<StrId> setting_label(std::string_view label);

}  // namespace wisp::ui
