#pragma once

// LlmConfigService (spec 5.3): the LLM endpoint, model and API key, stored
// in the System area as "secrets/llm.json" ({"base_url","model","api_key"}).
//
// The API key is write-only. get() reports only whether one is set; no
// method returns any part of it except authorization_for_request(), which
// exists solely for the code that sends LLM requests (net::probe_llm and
// the sub-project 4 agent client). Adapters (REST routes, Lua bindings,
// agent tools) must never call it. Nothing here logs the key, the file's
// contents or a base_url+key pair, and error messages never echo the key.
//
// The connectivity test lives in wisp_net (net::probe_llm), not here: it
// needs HttpClient, and wisp_core cannot depend on wisp_net.
//
// Thread-safe: the admin HTTP handler and request-sending code may call in
// concurrently.

#include <optional>
#include <string>

#include "wisp/base/result.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

struct LlmConfig {
    std::string base_url;
    std::string model;
    bool has_key = false;
};

// Fields left as nullopt keep their current value. api_key "" clears the
// stored key.
struct LlmConfigUpdate {
    std::optional<std::string> base_url, model, api_key;
};

class LlmConfigService {
public:
    explicit LlmConfigService(StorageManager& storage);

    LlmConfigService(const LlmConfigService&) = delete;
    LlmConfigService& operator=(const LlmConfigService&) = delete;

    // Loads secrets/llm.json. A missing file is an empty config. A corrupt
    // file logs a warning and is treated as empty (the next set() rewrites
    // it). A storage read failure is returned, leaving the config empty.
    Result<void> init();

    LlmConfig get() const;

    // Validates every provided field before changing anything; on
    // InvalidArgument (or a failed write) nothing changes, in memory or on
    // disk. Rules: base_url starts with "http://" or "https://", has a
    // non-empty host and no whitespace or control characters, and has its
    // trailing '/' characters stripped; model is at most 128 bytes with no
    // control characters; api_key is printable ASCII without spaces (it is
    // sent in an HTTP header, so CR/LF would allow header injection).
    Result<void> set(const LlmConfigUpdate& u);

    // Internal: "Bearer <key>", or "" when no key is set. Only for code that
    // sends LLM requests -- see the file header.
    std::string authorization_for_request() const;

    // Forgets everything and deletes secrets/llm.json (factory reset).
    // Failures to delete are logged, not returned: the caller reboots
    // regardless.
    void clear();

private:
    struct Stored {
        std::string base_url;
        std::string model;
        std::string api_key;
    };

    std::string file_path() const;  // "" if the System area is unavailable
    Result<void> persist(const Stored& next);

    StorageManager& storage_;
    mutable os::Mutex mutex_;
    Stored current_;
};

}  // namespace wisp::core
