#include "wisp/media/path.hpp"

#include <algorithm>

namespace wisp::media {

namespace {

Result<std::string> invalid(const char* why) {
    return Result<std::string>::err(ErrorCode::InvalidArgument, why);
}

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

}  // namespace

Result<std::string> normalize(std::string_view rel) {
    if (rel.empty()) return invalid("empty path");
    if (rel.size() > kMaxPathBytes) return invalid("path too long");
    if (rel.front() == '/') return invalid("absolute path");

    bool segment_start = true;
    for (char ch : rel) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7f) return invalid("control character");
        if (ch == '\\') return invalid("backslash");
        if (ch == '/') {
            if (segment_start) return invalid("empty segment");
            segment_start = true;
            continue;
        }
        // A leading dot covers ".", ".." and hidden files such as the
        // library's own index.
        if (segment_start && ch == '.') return invalid("hidden or relative segment");
        segment_start = false;
    }
    if (segment_start) return invalid("empty segment");
    return Result<std::string>::ok(std::string(rel));
}

Result<std::string> normalize_dir(std::string_view rel) {
    if (rel.empty()) return Result<std::string>::ok(std::string());
    return normalize(rel);
}

Result<std::string> normalize_for_write(std::string_view rel) {
    auto n = normalize(rel);
    if (!n) return n;
    const std::string_view path = n.value();
    const auto slash = path.find('/');
    if (slash == std::string_view::npos) return invalid("write outside a media folder");
    const auto first = path.substr(0, slash);
    if (std::find(std::begin(kFolders), std::end(kFolders), first) == std::end(kFolders)) {
        return invalid("write outside a media folder");
    }
    return n;
}

bool is_track_name(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    const auto ext = name.substr(dot + 1);
    auto is = [&](std::string_view want) {
        return ext.size() == want.size() &&
               std::equal(ext.begin(), ext.end(), want.begin(),
                          [](char a, char b) { return lower(a) == b; });
    };
    return is("mp3") || is("wav");
}

}  // namespace wisp::media
