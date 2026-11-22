#pragma once

#include "wisp/base/error.hpp"

namespace wisp::media {

// The wire name of an error in the media spec (section 8): one of
// media_unavailable, not_found, invalid_path, permission_denied,
// unsupported_format, decode_error, busy, io_error. Codes with no
// dedicated name map to io_error.
const char* wire_name(const Error& e);

// The REST status that goes with wire_name(): 503, 404, 400, 403, 415, 422,
// 503 and 500 in the order listed above.
int http_status(const Error& e);

}  // namespace wisp::media
