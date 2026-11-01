#pragma once

// Simulator backend for wisp::core::Settings: persists every namespace/key
// to a single JSON file, shaped as {"<ns>": {"<key>": <value>, ...}, ...}.
// This is host-only platform code (not built for the ESP port); a later
// task adds an NVS-backed wisp::core::SettingsBackend for ESP-IDF with the
// same validate_settings_name() behavior (see wisp/core/settings.hpp).

#include <map>
#include <string>

#include "wisp/base/value.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/os/os.hpp"

namespace wisp::port::sim {

class JsonFileSettingsBackend : public core::SettingsBackend {
public:
    // Loads `file_path` if it already exists and parses as the expected
    // {"<ns>": {"<key>": value}} shape. If the file is missing, cannot be
    // read, or does not parse as that shape, construction still succeeds:
    // the backend starts empty and logs a warning (WLOG_W), rather than
    // failing.
    explicit JsonFileSettingsBackend(std::string file_path);

    Result<Value> get(const std::string& ns, const std::string& key) override;
    Result<void> set(const std::string& ns, const std::string& key, const Value& value) override;
    Result<void> erase(const std::string& ns, const std::string& key) override;

private:
    // Rewrites the whole store to file_path_: serializes to JSON, writes
    // it to a sibling "<file_path_>.tmp" file, then renames that temp file
    // over file_path_ (POSIX rename overwrites the destination on Linux).
    // Called with mutex_ already held. Returns ErrorCode::IoError if the
    // temp file can't be written or the rename fails.
    Result<void> write_through();

    std::string file_path_;
    os::Mutex mutex_;
    std::map<std::string, std::map<std::string, Value>> store_;
};

}  // namespace wisp::port::sim
