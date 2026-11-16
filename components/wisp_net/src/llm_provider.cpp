#include "wisp/net/llm_provider.hpp"

namespace wisp::net {

const char* to_string(LlmError e) {
    switch (e) {
        case LlmError::None:
            return "";
        case LlmError::NoModel:
            return "no_model";
        case LlmError::Unauthorized:
            return "unauthorized";
        case LlmError::RateLimited:
            return "rate_limited";
        case LlmError::Server:
            return "server";
        case LlmError::Http:
            return "http";
        case LlmError::Transport:
            return "transport";
        case LlmError::Cancelled:
            return "cancelled";
        case LlmError::BadStream:
            return "bad_stream";
        case LlmError::ContextLength:
            return "context_length";
    }
    return "";
}

}  // namespace wisp::net
