#pragma once

// Simulator backend for wisp::core::Settings: persists every namespace/key
// to a single JSON file, shaped as {"<ns>": {"<key>": <value>, ...}, ...}.
// This is host-only platform code (not built for the ESP port); a later
// task adds an NVS-backed wisp::core::SettingsBackend for ESP-IDF with the
// same validate_settings_name() behavior (see wisp/core/settings.hpp).
//
// os::Mutex only guards concurrent access from *within this process*: a
// JsonFileSettingsBackend instance is safe to share across threads, but
// there is no file locking, so two backend instances (in this process or
// another) pointed at the same file_path will race and can corrupt each
// other's writes. Construct exactly one backend per settings file.

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
    // Rewrites the file as an empty store; on an IoError nothing changes,
    // in memory or on disk.
    Result<void> erase_all() override;

private:
    // Rewrites the whole store to file_path_: serializes to JSON, writes
    // it to a sibling "<file_path_>.tmp" file, then renames that temp file
    // over file_path_ (POSIX rename overwrites the destination on Linux).
    // Called with mutex_ already held. Returns ErrorCode::IoError if the
    // temp file can't be written or the rename fails; on either failure,
    // the .tmp file is removed (best-effort) and the in-memory store_ is
    // left exactly as the caller found it (see set()/erase(), which roll
    // back their mutation to store_ before returning this error).
    Result<void> write_through();

    std::string file_path_;
    os::Mutex mutex_;
    std::map<std::string, std::map<std::string, Value>> store_;
};

}  // namespace wisp::port::sim
