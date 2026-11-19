#pragma once

#include <string>
#include <string_view>

#include "wisp/base/result.hpp"

namespace wisp::media {

// The three top-level folders under the media root; the only places a
// write may land.
inline constexpr std::string_view kFolders[] = {"music", "download", "document"};

// Longest accepted relative path, in bytes (the FAT long-name limit).
inline constexpr size_t kMaxPathBytes = 255;

// Validates a path relative to the media root and returns it unchanged.
// Accepted: non-empty, at most kMaxPathBytes, '/'-separated segments that
// are non-empty, do not start with '.', and hold no control characters.
// Rejected (InvalidArgument): everything else, including a leading '/',
// '\\', "." and ".." segments and the hidden index file. Bytes of UTF-8
// pass through untouched.
Result<std::string> normalize(std::string_view rel);

// Like normalize(), but the empty string is the media root and is accepted.
Result<std::string> normalize_dir(std::string_view rel);

// normalize() plus: the path names a file inside one of kFolders (at least
// two segments, the first a member of kFolders).
Result<std::string> normalize_for_write(std::string_view rel);

// True when the file name's extension (case-insensitive) is a playable
// track format: mp3 or wav.
bool is_track_name(std::string_view name);

}  // namespace wisp::media
